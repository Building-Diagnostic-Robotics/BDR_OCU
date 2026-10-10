/**
 * @file cloud_verifier.cpp
 * @brief `/verify` client and the stick-manifest vs S3 comparison.
 */

#include "cloud_verifier.hpp"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace f2c_cpp {

namespace {

constexpr int kSummaryNames = 3;

bool isMd5Hex(const QString& lower) {
    if (lower.size() != 32) return false;
    for (const QChar c : lower) {
        if (c.isDigit()) continue;
        const ushort u = c.unicode();
        if (u < 'a' || u > 'f') return false;
    }
    return true;
}

// Same names `uploader.py` refuses to upload as data. A manifest that
// lists one of them must not make the section look missing.
bool isBookkeepingName(const QString& relpath) {
    const int slash = relpath.lastIndexOf(QLatin1Char('/'));
    const QString base = slash < 0 ? relpath : relpath.mid(slash + 1);
    return base == QLatin1String("upload_state.json") ||
           base == QLatin1String("pause.flag") ||
           base == QLatin1String("manifest.json") ||
           base == QLatin1String("upload_state.json.tmp") ||
           base == QLatin1String("manifest.json.tmp");
}

qint64 jsonSize(const QJsonValue& value) {
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble());
    }
    if (value.isString()) {
        bool ok = false;
        const qint64 n = value.toString().toLongLong(&ok);
        if (ok) return n;
    }
    return -1;
}

QString normalizedEtag(QString raw) {
    raw = raw.trimmed();
    if (raw.size() >= 2 && raw.front() == QLatin1Char('"') &&
        raw.back() == QLatin1Char('"')) {
        raw = raw.mid(1, raw.size() - 2);
    }
    return raw.toLower();
}

}  // namespace

QString cloudVerifySummary(const CloudCompareResult& result) {
    if (result.matches) return QStringLiteral("ok");
    const int n = result.problems.size();
    if (n <= 0) return QStringLiteral("missing");
    const int shown = qMin(kSummaryNames, n);
    QString list = result.problems.mid(0, shown).join(QStringLiteral(", "));
    if (n > shown) list += QStringLiteral(", …");
    return QStringLiteral("missing %1 file%2 (%3)")
        .arg(n)
        .arg(n == 1 ? QString() : QStringLiteral("s"))
        .arg(list);
}

CloudCompareResult compareManifestWithCloud(const QList<ManifestFile>& manifest_files,
                                            const QList<CloudObject>& cloud_files) {
    QHash<QString, CloudObject> cloud;
    for (const CloudObject& obj : cloud_files) {
        if (obj.relpath.isEmpty() || isBookkeepingName(obj.relpath)) continue;
        cloud.insert(obj.relpath, obj);
    }
    // manifest.json is bookkeeping and was skipped above; presence is
    // its own check. Every other extra key (GPR_Output, the complete
    // marker) is ignored by never being looked up.
    bool manifest_in_cloud = false;
    for (const CloudObject& obj : cloud_files) {
        if (obj.relpath == QLatin1String("manifest.json")) {
            manifest_in_cloud = true;
            break;
        }
    }

    CloudCompareResult result;
    if (!manifest_in_cloud) {
        result.problems << QStringLiteral("manifest.json");
    }
    for (const ManifestFile& entry : manifest_files) {
        if (entry.relpath.isEmpty() || isBookkeepingName(entry.relpath)) continue;
        const auto it = cloud.constFind(entry.relpath);
        if (it == cloud.constEnd()) {
            result.problems << entry.relpath;
            continue;
        }
        if (entry.size_bytes >= 0 && it->size >= 0 &&
            entry.size_bytes != it->size) {
            result.problems << entry.relpath;
            continue;
        }
        const QString md5 = entry.md5.trimmed().toLower();
        if (!isMd5Hex(md5)) continue;
        if (isMd5Hex(it->etag) && it->etag != md5) {
            result.problems << entry.relpath;
        }
    }
    result.matches = result.problems.isEmpty();
    return result;
}

bool manifestMatchesLogin(const QString& manifest_client,
                          const QString& manifest_robot,
                          const QString& client_id,
                          const QString& robot_id) {
    const QString recorded_client = manifest_client.trimmed();
    const QString recorded_robot = manifest_robot.trimmed();
    if (!recorded_client.isEmpty() && recorded_client != client_id.trimmed()) {
        return false;
    }
    if (!recorded_robot.isEmpty() && recorded_robot != robot_id.trimmed()) {
        return false;
    }
    return true;
}

bool parseManifestFiles(const QByteArray& json, QList<ManifestFile>* out,
                        QString* error, QString* client_id, QString* robot_id) {
    if (!out) return false;
    out->clear();
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = parse_error.error == QJsonParseError::NoError
                         ? QStringLiteral("manifest is not a JSON object")
                         : parse_error.errorString();
        }
        return false;
    }
    const QJsonObject root = doc.object();
    if (client_id) *client_id = root.value(QStringLiteral("client_id")).toString().trimmed();
    if (robot_id) *robot_id = root.value(QStringLiteral("robot_id")).toString().trimmed();
    const QJsonValue files_value = root.value(QStringLiteral("files"));
    if (!files_value.isArray()) {
        if (error) *error = QStringLiteral("manifest has no file list");
        return false;
    }
    for (const QJsonValue& item : files_value.toArray()) {
        if (!item.isObject()) continue;
        const QJsonObject obj = item.toObject();
        const QString rel = obj.value(QStringLiteral("relpath")).toString();
        if (rel.isEmpty() || isBookkeepingName(rel)) continue;
        ManifestFile file;
        file.relpath = rel;
        file.size_bytes = jsonSize(obj.value(QStringLiteral("size_bytes")));
        const QString md5 = obj.value(QStringLiteral("md5")).toString().trimmed().toLower();
        if (isMd5Hex(md5)) file.md5 = md5;
        out->append(file);
    }
    return true;
}

LoadedManifest loadManifestAt(const QString& path) {
    LoadedManifest loaded;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        loaded.error = file.errorString();
        return loaded;
    }
    QString error;
    if (!parseManifestFiles(file.readAll(), &loaded.files, &error,
                            &loaded.client_id, &loaded.robot_id)) {
        loaded.error = error;
        return loaded;
    }
    loaded.ok = true;
    return loaded;
}

bool parseVerifyRun(const QByteArray& json, const QString& run_id,
                    QList<CloudObject>* out, QString* error) {
    if (!out) return false;
    out->clear();
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = parse_error.error == QJsonParseError::NoError
                         ? QStringLiteral("response is not a JSON object")
                         : parse_error.errorString();
        }
        return false;
    }
    const QJsonObject runs = doc.object().value(QStringLiteral("runs")).toObject();
    if (!runs.contains(run_id)) {
        if (error) *error = QStringLiteral("response has no listing for this section");
        return false;
    }
    const QJsonValue files_value =
        runs.value(run_id).toObject().value(QStringLiteral("files"));
    if (!files_value.isArray()) {
        if (error) *error = QStringLiteral("listing has no file list");
        return false;
    }
    for (const QJsonValue& item : files_value.toArray()) {
        if (!item.isObject()) continue;
        const QJsonObject obj = item.toObject();
        CloudObject file;
        file.relpath = obj.value(QStringLiteral("relpath")).toString();
        if (file.relpath.isEmpty()) continue;
        file.size = jsonSize(obj.value(QStringLiteral("size")));
        file.etag = normalizedEtag(obj.value(QStringLiteral("etag")).toString());
        out->append(file);
    }
    return true;
}

CloudVerifier::CloudVerifier(QObject* parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);
    // Direct connection. A system HTTP proxy would see the device token,
    // and it would also break the localhost tests.
    nam_->setProxy(QNetworkProxy::NoProxy);
    timeout_ = new QTimer(this);
    timeout_->setSingleShot(true);
    connect(timeout_, &QTimer::timeout, this, [this]() {
        if (!reply_) return;
        timed_out_ = true;
        reply_->abort();
    });
}

CloudVerifier::~CloudVerifier() {
    cancel();
}

void CloudVerifier::setAuth(const QString& api_base,
                            const QString& client_id,
                            const QString& device_token) {
    api_base_ = api_base.trimmed();
    while (api_base_.endsWith(QLatin1Char('/'))) {
        api_base_.chop(1);
    }
    client_id_ = client_id.trimmed();
    device_token_ = device_token.trimmed();
}

void CloudVerifier::setTimeoutMs(int timeout_ms) {
    timeout_ms_ = timeout_ms > 0 ? timeout_ms : 30000;
}

void CloudVerifier::verify(const QString& robot_id, const QStringList& run_ids,
                           int generation) {
    abortInFlight();
    generation_ = generation;
    robot_id_ = robot_id;
    pending_ = run_ids;
    current_run_.clear();
    if (pending_.isEmpty()) {
        emit sequenceFinished(generation_);
        return;
    }
    sendNext();
}

void CloudVerifier::cancel() {
    generation_ = -1;
    pending_.clear();
    current_run_.clear();
    abortInFlight();
}

void CloudVerifier::abortInFlight() {
    if (timeout_) timeout_->stop();
    timed_out_ = false;
    if (!reply_) return;
    QNetworkReply* reply = reply_;
    reply_ = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void CloudVerifier::sendNext() {
    if (generation_ < 0) return;
    if (pending_.isEmpty()) {
        emit sequenceFinished(generation_);
        return;
    }
    current_run_ = pending_.takeFirst();

    QJsonObject body;
    body.insert(QStringLiteral("robot_id"), robot_id_);
    QJsonArray ids;
    ids.append(current_run_);
    body.insert(QStringLiteral("run_ids"), ids);

    QNetworkRequest req{QUrl(api_base_ + QStringLiteral("/verify"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    req.setRawHeader("x-client-id", client_id_.toUtf8());
    req.setRawHeader("x-device-token", device_token_.toUtf8());

    timed_out_ = false;
    reply_ = nam_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply_, &QNetworkReply::finished, this, &CloudVerifier::finishCurrent);
    timeout_->start(timeout_ms_);
}

void CloudVerifier::finishCurrent() {
    QNetworkReply* reply = reply_;
    if (!reply || sender() != reply) return;
    reply_ = nullptr;
    if (timeout_) timeout_->stop();
    reply->deleteLater();

    const int generation = generation_;
    const QString run_id = current_run_;
    current_run_.clear();
    if (generation < 0) return;

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // abort() (timeout or cancel) closes the device; reading it warns.
    const QByteArray body = reply->isOpen() ? reply->readAll() : QByteArray();

    auto fail = [this, generation, run_id](const QString& error) {
        pending_.clear();
        emit runFinished(generation, run_id, false, {}, error);
        if (generation_ == generation) {
            emit sequenceFinished(generation);
        }
    };

    if (timed_out_) {
        fail(QStringLiteral("timed out"));
        return;
    }
    if (reply->error() != QNetworkReply::NoError && status == 0) {
        fail(reply->errorString());
        return;
    }
    if (status != 200) {
        QString detail = QStringLiteral("HTTP %1").arg(status);
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            const QString message = doc.object().value(QStringLiteral("message")).toString();
            if (!message.isEmpty()) {
                detail += QStringLiteral(" ") + message.left(200);
            }
        }
        fail(detail);
        return;
    }

    QList<CloudObject> files;
    QString error;
    if (!parseVerifyRun(body, run_id, &files, &error)) {
        fail(error.isEmpty() ? QStringLiteral("unreadable response") : error);
        return;
    }
    emit runFinished(generation, run_id, true, files, QString());
    if (generation_ == generation) {
        sendNext();
    }
}

}  // namespace f2c_cpp
