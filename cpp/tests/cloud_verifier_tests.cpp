/**
 * @file cloud_verifier_tests.cpp
 * @brief Stick manifest vs S3 listing, and `/verify` against a local stub.
 */

#include "cloud_verifier.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

using f2c_cpp::CloudCompareResult;
using f2c_cpp::CloudObject;
using f2c_cpp::CloudVerifier;
using f2c_cpp::LoadedManifest;
using f2c_cpp::ManifestFile;
using f2c_cpp::cloudVerifySummary;
using f2c_cpp::compareManifestWithCloud;
using f2c_cpp::loadManifestAt;
using f2c_cpp::manifestMatchesLogin;
using f2c_cpp::parseManifestFiles;
using f2c_cpp::parseVerifyRun;

namespace {

QCoreApplication* app() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char arg0[] = "cloud_verifier_tests";
        static char* argv[] = {arg0, nullptr};
        new QCoreApplication(argc, argv);
    }
    return QCoreApplication::instance();
}

ManifestFile file(const char* rel, qint64 size, const char* md5 = "") {
    ManifestFile f;
    f.relpath = QString::fromLatin1(rel);
    f.size_bytes = size;
    f.md5 = QString::fromLatin1(md5);
    return f;
}

CloudObject obj(const char* rel, qint64 size, const char* etag = "") {
    CloudObject o;
    o.relpath = QString::fromLatin1(rel);
    o.size = size;
    o.etag = QString::fromLatin1(etag);
    return o;
}

const char* kMd5 = "900150983cd24fb0d6963f7d28e17f72";

class LocalVerifyServer : public QObject {
public:
    enum class Reply { Ok, Forbidden, ServerError, Hang, Garbage };

    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort());
    }
    void setReplies(const QList<Reply>& replies) { replies_ = replies; }

    int accepted = 0;
    QStringList bodies;
    QList<QByteArray> headers;

    LocalVerifyServer() {
        QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
            while (server_.hasPendingConnections()) accept(server_.nextPendingConnection());
        });
    }

private:
    void accept(QTcpSocket* sock) {
        ++accepted;
        const int index = accepted - 1;
        const Reply reply = index < replies_.size() ? replies_.at(index) : Reply::Ok;
        auto* buf = new QByteArray;
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf, reply]() {
            buf->append(sock->readAll());
            const int hdr = buf->indexOf("\r\n\r\n");
            if (hdr < 0) return;
            const QByteArray header = buf->left(hdr);
            int length = 0;
            for (const QByteArray& line : header.split('\n')) {
                if (line.toLower().startsWith("content-length:")) {
                    length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                }
            }
            if (buf->size() < hdr + 4 + length) return;
            headers.append(header);
            bodies.append(QString::fromUtf8(buf->mid(hdr + 4, length)));
            if (reply == Reply::Hang) return;
            respond(sock, reply, bodies.last());
            sock->disconnect(sock, &QTcpSocket::readyRead, sock, nullptr);
        });
    }

    static void respond(QTcpSocket* sock, Reply reply, const QString& request_body) {
        QByteArray payload;
        int status = 200;
        const char* reason = "OK";
        if (reply == Reply::Forbidden) {
            status = 403;
            reason = "Forbidden";
            payload = R"({"message":"forbidden"})";
        } else if (reply == Reply::ServerError) {
            status = 500;
            reason = "Error";
            payload = R"({"message":"boom"})";
        } else if (reply == Reply::Garbage) {
            payload = "not-json";
        } else {
            const QJsonDocument doc = QJsonDocument::fromJson(request_body.toUtf8());
            const QString run_id = doc.object()
                                       .value(QStringLiteral("run_ids"))
                                       .toArray()
                                       .at(0)
                                       .toString();
            payload = (QStringLiteral(
                           R"({"runs":{"%1":{"files":[{"relpath":"a.bin","size":4,"etag":"\"%2\""},{"relpath":"manifest.json","size":2,"etag":"ab"}]}}})")
                           .arg(run_id, QLatin1String(kMd5)))
                           .toUtf8();
        }
        const QByteArray raw =
            QStringLiteral("HTTP/1.1 %1 %2\r\nContent-Length: %3\r\n"
                           "Connection: close\r\n\r\n")
                .arg(status)
                .arg(QLatin1String(reason))
                .arg(payload.size())
                .toUtf8() +
            payload;
        sock->write(raw);
        sock->disconnectFromHost();
    }

    QTcpServer server_;
    QList<Reply> replies_;
};

struct HttpOutcome {
    bool sequence_done = false;
    QStringList runs;
    QList<bool> oks;
    QStringList errors;
    QList<QList<CloudObject>> files;
};

HttpOutcome runVerify(LocalVerifyServer* server, const QStringList& run_ids,
                      int timeout_ms) {
    app();
    CloudVerifier verifier;
    verifier.setAuth(server->baseUrl(), QStringLiteral("client_x"),
                     QStringLiteral("tok_y"));
    verifier.setTimeoutMs(timeout_ms);
    HttpOutcome out;
    QObject::connect(&verifier, &CloudVerifier::runFinished,
                     [&](int, const QString& run, bool ok, const QList<CloudObject>& files,
                         const QString& error) {
                         out.runs << run;
                         out.oks << ok;
                         out.errors << error;
                         out.files << files;
                     });
    QEventLoop loop;
    QObject::connect(&verifier, &CloudVerifier::sequenceFinished, [&](int) {
        out.sequence_done = true;
        loop.quit();
    });
    QTimer::singleShot(timeout_ms + 2000, &loop, &QEventLoop::quit);
    verifier.verify(QStringLiteral("Roofus#0003"), run_ids, 7);
    loop.exec();
    return out;
}

}  // namespace

TEST(CloudCompare, AllPresentWithMd5Matches) {
    const QList<ManifestFile> manifest = {file("a.bin", 4, kMd5), file("sub/b.bin", 2, kMd5)};
    const QList<CloudObject> cloud = {
        obj("a.bin", 4, kMd5),
        obj("sub/b.bin", 2, kMd5),
        obj("manifest.json", 20, "ab"),
    };
    const CloudCompareResult r = compareManifestWithCloud(manifest, cloud);
    EXPECT_TRUE(r.matches);
    EXPECT_TRUE(r.problems.isEmpty());
    EXPECT_EQ(cloudVerifySummary(r), "ok");
}

TEST(CloudCompare, MissingFile) {
    const CloudCompareResult r = compareManifestWithCloud(
        {file("a.bin", 4, kMd5), file("gone.bin", 1, kMd5)},
        {obj("a.bin", 4, kMd5), obj("manifest.json", 2, "ab")});
    EXPECT_FALSE(r.matches);
    EXPECT_EQ(r.problems, QStringList({"gone.bin"}));
    EXPECT_EQ(cloudVerifySummary(r), "missing 1 file (gone.bin)");
}

TEST(CloudCompare, SizeMismatch) {
    const CloudCompareResult r = compareManifestWithCloud(
        {file("a.bin", 4, kMd5)},
        {obj("a.bin", 0, kMd5), obj("manifest.json", 2, "ab")});
    EXPECT_FALSE(r.matches);
    EXPECT_EQ(r.problems, QStringList({"a.bin"}));
}

TEST(CloudCompare, Md5Mismatch) {
    const CloudCompareResult r = compareManifestWithCloud(
        {file("a.bin", 4, kMd5)},
        {obj("a.bin", 4, "00000000000000000000000000000000"),
         obj("manifest.json", 2, "ab")});
    EXPECT_FALSE(r.matches);
    EXPECT_EQ(r.problems, QStringList({"a.bin"}));
}

TEST(CloudCompare, ExtraCloudFilesAreIgnored) {
    const CloudCompareResult r = compareManifestWithCloud(
        {file("a.bin", 4, kMd5)},
        {obj("a.bin", 4, kMd5),
         obj("manifest.json", 2, "ab"),
         obj("GPR_Output/report.csv", 99, kMd5),
         obj("_UPLOAD_COMPLETE.json", 12, kMd5)});
    EXPECT_TRUE(r.matches) << r.problems.join(", ").toStdString();
}

TEST(CloudCompare, LegacyManifestUsesSizeOnly) {
    const QList<CloudObject> cloud = {
        obj("a.bin", 4, "ffffffffffffffffffffffffffffffff"),
        obj("manifest.json", 2, "ab"),
    };
    EXPECT_TRUE(compareManifestWithCloud({file("a.bin", 4)}, cloud).matches);
    const CloudCompareResult mismatch =
        compareManifestWithCloud({file("a.bin", 5)}, cloud);
    EXPECT_FALSE(mismatch.matches);
    EXPECT_EQ(mismatch.problems, QStringList({"a.bin"}));
}

TEST(CloudCompare, MissingManifestJson) {
    const CloudCompareResult r = compareManifestWithCloud(
        {file("a.bin", 4, kMd5)}, {obj("a.bin", 4, kMd5)});
    EXPECT_FALSE(r.matches);
    EXPECT_EQ(r.problems.first(), "manifest.json");
}

TEST(CloudCompare, MultipartEtagIsNotCompared) {
    const CloudCompareResult r = compareManifestWithCloud(
        {file("a.bin", 4, kMd5)},
        {obj("a.bin", 4, "900150983cd24fb0d6963f7d28e17f72-2"),
         obj("manifest.json", 2, "ab")});
    EXPECT_TRUE(r.matches);
}

TEST(CloudCompare, SummaryEllipsizesPastThreeNames) {
    const CloudCompareResult r = compareManifestWithCloud(
        {file("a", 1), file("b", 1), file("c", 1), file("d", 1)},
        {obj("manifest.json", 1, "ab")});
    EXPECT_EQ(cloudVerifySummary(r), "missing 4 files (a, b, c, …)");
}

TEST(CloudCompare, ParseManifestAndVerifyBody) {
    QList<ManifestFile> files;
    QString error;
    ASSERT_TRUE(parseManifestFiles(
        R"({"files":[{"relpath":"a.bin","size_bytes":4,"md5":"900150983CD24FB0D6963F7D28E17F72","sha256":"zz"},
                     {"relpath":"old.bin","size_bytes":3},
                     {"relpath":"manifest.json","size_bytes":1},
                     {"relpath":"notes.tmp","size_bytes":true}]})",
        &files, &error));
    ASSERT_EQ(files.size(), 3);
    EXPECT_EQ(files[0].md5, kMd5);
    EXPECT_EQ(files[1].size_bytes, 3);
    EXPECT_TRUE(files[1].md5.isEmpty());
    EXPECT_EQ(files[2].relpath, "notes.tmp");
    EXPECT_EQ(files[2].size_bytes, -1);

    QList<CloudObject> cloud;
    ASSERT_TRUE(parseVerifyRun(
        R"({"runs":{"day/b/Section_1":{"files":[{"relpath":"a.bin","size":4,"etag":"\"ABCD\""}]}}})",
        QStringLiteral("day/b/Section_1"), &cloud, &error))
        << error.toStdString();
    ASSERT_EQ(cloud.size(), 1);
    EXPECT_EQ(cloud[0].etag, "abcd");
    EXPECT_FALSE(parseVerifyRun(R"({"runs":{}})", QStringLiteral("missing"), &cloud, &error));
}

TEST(CloudCompare, ManifestOwnerMustMatchTheLogin) {
    EXPECT_TRUE(manifestMatchesLogin("durafoam_roofing_CA", "Roofus#0003",
                                     "durafoam_roofing_CA", "Roofus#0003"));
    EXPECT_TRUE(manifestMatchesLogin(" durafoam_roofing_CA ", "Roofus#0003",
                                     "durafoam_roofing_CA", "Roofus#0003"));
    EXPECT_TRUE(manifestMatchesLogin("", "", "durafoam_roofing_CA", "Roofus#0003"));
    EXPECT_FALSE(manifestMatchesLogin("sig_roofing_ID", "Roofus#0003",
                                      "durafoam_roofing_CA", "Roofus#0003"));
    EXPECT_FALSE(manifestMatchesLogin("durafoam_roofing_CA", "Roofus#0002",
                                      "durafoam_roofing_CA", "Roofus#0003"));
    EXPECT_FALSE(manifestMatchesLogin("other", "", "durafoam_roofing_CA", "Roofus#0003"));
}

TEST(CloudCompare, LoadManifestFromDisk) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + "/manifest.json";
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(R"({"client_id":" durafoam_roofing_CA ","robot_id":"Roofus#0003","files":[{"relpath":"a.bin","size_bytes":4,"md5":"900150983cd24fb0d6963f7d28e17f72"}]})");
    f.close();
    const LoadedManifest loaded = loadManifestAt(path);
    ASSERT_TRUE(loaded.ok);
    EXPECT_EQ(loaded.client_id, "durafoam_roofing_CA");
    EXPECT_EQ(loaded.robot_id, "Roofus#0003");
    ASSERT_EQ(loaded.files.size(), 1);
    EXPECT_EQ(loaded.files[0].relpath, "a.bin");
    const LoadedManifest missing = loadManifestAt(tmp.path() + "/nope.json");
    EXPECT_FALSE(missing.ok);
}

TEST(CloudVerifierHttp, OkParsesListingAndSendsAuth) {
    app();
    LocalVerifyServer server;
    server.setReplies({LocalVerifyServer::Reply::Ok});
    ASSERT_TRUE(server.listen());
    const HttpOutcome out = runVerify(&server, {QStringLiteral("day/b/Section_1")}, 1000);
    ASSERT_TRUE(out.sequence_done);
    ASSERT_EQ(out.oks.size(), 1);
    EXPECT_TRUE(out.oks[0]) << out.errors.join(" | ").toStdString();
    ASSERT_EQ(out.files[0].size(), 2);
    EXPECT_EQ(out.files[0][0].relpath, "a.bin");
    EXPECT_EQ(out.files[0][0].size, 4);
    EXPECT_EQ(out.files[0][0].etag, kMd5);
    ASSERT_EQ(server.bodies.size(), 1);
    EXPECT_TRUE(server.bodies[0].contains("day/b/Section_1"));
    EXPECT_TRUE(server.bodies[0].contains("Roofus#0003"));
    const QByteArray header = server.headers[0].toLower();
    EXPECT_TRUE(header.contains("x-client-id: client_x")) << header.toStdString();
    EXPECT_TRUE(header.contains("x-device-token: tok_y")) << header.toStdString();
}

TEST(CloudVerifierHttp, TwoRunsAreSeparateRequests) {
    app();
    LocalVerifyServer server;
    server.setReplies({LocalVerifyServer::Reply::Ok, LocalVerifyServer::Reply::Ok});
    ASSERT_TRUE(server.listen());
    const HttpOutcome out = runVerify(
        &server, {QStringLiteral("run/a"), QStringLiteral("run/b")}, 1000);
    ASSERT_TRUE(out.sequence_done);
    EXPECT_EQ(out.runs, QStringList({"run/a", "run/b"}));
    EXPECT_EQ(server.accepted, 2);
    EXPECT_TRUE(out.oks[0]);
    EXPECT_TRUE(out.oks[1]);
}

TEST(CloudVerifierHttp, ForbiddenStopsTheQueue) {
    app();
    LocalVerifyServer server;
    server.setReplies({LocalVerifyServer::Reply::Forbidden, LocalVerifyServer::Reply::Ok});
    ASSERT_TRUE(server.listen());
    const HttpOutcome out = runVerify(
        &server, {QStringLiteral("run/a"), QStringLiteral("run/b")}, 1000);
    ASSERT_TRUE(out.sequence_done);
    ASSERT_EQ(out.oks.size(), 1);
    EXPECT_FALSE(out.oks[0]);
    EXPECT_TRUE(out.errors[0].contains("403")) << out.errors[0].toStdString();
    EXPECT_EQ(server.accepted, 1);
}

TEST(CloudVerifierHttp, ServerErrorAndGarbageAreUnavailable) {
    app();
    LocalVerifyServer server;
    server.setReplies({LocalVerifyServer::Reply::ServerError});
    ASSERT_TRUE(server.listen());
    const HttpOutcome err = runVerify(&server, {QStringLiteral("run/a")}, 1000);
    ASSERT_TRUE(err.sequence_done);
    ASSERT_EQ(err.oks.size(), 1);
    EXPECT_FALSE(err.oks[0]);
    EXPECT_TRUE(err.errors[0].contains("500")) << err.errors[0].toStdString();

    LocalVerifyServer garbage;
    garbage.setReplies({LocalVerifyServer::Reply::Garbage});
    ASSERT_TRUE(garbage.listen());
    const HttpOutcome bad = runVerify(&garbage, {QStringLiteral("run/a")}, 1000);
    ASSERT_TRUE(bad.sequence_done);
    EXPECT_FALSE(bad.oks[0]);
}

TEST(CloudVerifierHttp, TimeoutDoesNotStartTheNextRun) {
    app();
    LocalVerifyServer server;
    server.setReplies({LocalVerifyServer::Reply::Hang, LocalVerifyServer::Reply::Ok});
    ASSERT_TRUE(server.listen());
    const HttpOutcome out = runVerify(
        &server, {QStringLiteral("run/a"), QStringLiteral("run/b")}, 300);
    ASSERT_TRUE(out.sequence_done);
    ASSERT_EQ(out.oks.size(), 1);
    EXPECT_FALSE(out.oks[0]);
    EXPECT_TRUE(out.errors[0].contains("timed out")) << out.errors[0].toStdString();
    EXPECT_EQ(server.accepted, 1);
}
