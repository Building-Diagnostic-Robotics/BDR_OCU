/**
 * @file cloud_verifier.hpp
 * @brief Ask the BDR upload API whether a section is still complete in S3.
 *
 * `POST /verify` lists one run's objects. The laptop sends the same
 * `x-client-id` / `x-device-token` headers as `uploader.py` and holds
 * no AWS credentials. One `run_id` per request: a large section's
 * listing is about half a megabyte, and a combined response can hit
 * Lambda's 6 MB limit.
 *
 * Comparison is a pure function so it can be tested without the
 * network. Extra S3 keys (`GPR_Output/…`, `_UPLOAD_COMPLETE.json`) are
 * ignored — they are written in the cloud, not by the stick upload.
 * A non-200 reply, a timeout, or a transport error is "check
 * unavailable": the caller must leave the row as it was.
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace f2c_cpp {

/// One object the `/verify` listing returned. `etag` is lowercase and
/// quote-stripped. `size < 0` means the listing omitted it.
struct CloudObject {
    QString relpath;
    qint64 size = -1;
    QString etag;
};

/// One `files[]` entry from the stick's `manifest.json`.
/// `size_bytes < 0` and an empty `md5` mean the field was absent
/// (manifests written before md5 was recorded).
struct ManifestFile {
    QString relpath;
    qint64 size_bytes = -1;
    QString md5;
};

struct CloudCompareResult {
    /// True when every manifest file is in S3 at the recorded size
    /// (and md5, when the manifest has one) and `manifest.json` itself
    /// is present. Extra S3 keys do not clear this.
    bool matches = false;
    /// Relpaths that failed, `manifest.json` first when it is absent.
    QStringList problems;
};

/// `ok` when nothing failed, otherwise
/// `missing N file(s) (a, b, c)` with at most three names.
QString cloudVerifySummary(const CloudCompareResult& result);

CloudCompareResult compareManifestWithCloud(const QList<ManifestFile>& manifest_files,
                                            const QList<CloudObject>& cloud_files);

/// False when `json` is not an object with a `files` array. Bookkeeping
/// names (`manifest.json`, `upload_state.json`, `pause.flag`, and their
/// `.tmp` siblings) are dropped; they are not scan data.
/// `client_id` and `robot_id`, when non-null, receive the manifest's
/// owner fields (empty when the field is absent).
bool parseManifestFiles(const QByteArray& json, QList<ManifestFile>* out,
                        QString* error, QString* client_id = nullptr,
                        QString* robot_id = nullptr);

/// True when this manifest may be checked against the logged-in client
/// and robot. A recorded `client_id` or `robot_id` that differs is
/// another customer's upload — it must not be marked missing, or the
/// next Upload would put it under the wrong S3 prefix. Empty owner
/// fields are not a disagreement.
bool manifestMatchesLogin(const QString& manifest_client,
                          const QString& manifest_robot,
                          const QString& client_id,
                          const QString& robot_id);

/// Read and parse `<section>/manifest.json`. File I/O — call off the
/// GUI thread. `ok` is false when the file is missing or unreadable;
/// that is a local failure, not "missing in the cloud".
struct LoadedManifest {
    bool ok = false;
    QString error;
    QString client_id;
    QString robot_id;
    QList<ManifestFile> files;
};
LoadedManifest loadManifestAt(const QString& path);

/// Pull `runs[run_id].files` out of a `/verify` body. False when the
/// run is absent or the body is not the expected object — an empty
/// `files` array is a real "nothing in S3" answer and returns true.
bool parseVerifyRun(const QByteArray& json, const QString& run_id,
                    QList<CloudObject>* out, QString* error);

/**
 * Sequential `/verify` client. `verify()` aborts any request still in
 * flight. Results from a superseded generation are not emitted.
 * A non-200 reply or a transport failure emits `runFinished(http_ok=
 * false)` for that run and stops the rest of the queue.
 */
class CloudVerifier : public QObject {
    Q_OBJECT
public:
    explicit CloudVerifier(QObject* parent = nullptr);
    ~CloudVerifier() override;

    void setAuth(const QString& api_base,
                 const QString& client_id,
                 const QString& device_token);
    void setTimeoutMs(int timeout_ms);

    /// `generation` is echoed on every signal so a stick swap or a
    /// closed dialog can drop a late reply.
    void verify(const QString& robot_id, const QStringList& run_ids, int generation);
    void cancel();

signals:
    void runFinished(int generation, const QString& run_id, bool http_ok,
                     const QList<CloudObject>& files, const QString& error);
    void sequenceFinished(int generation);

private:
    void sendNext();
    void finishCurrent();
    void abortInFlight();

    QNetworkAccessManager* nam_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QTimer* timeout_ = nullptr;
    bool timed_out_ = false;

    QString api_base_;
    QString client_id_;
    QString device_token_;
    int timeout_ms_ = 30000;

    int generation_ = -1;
    QString robot_id_;
    QStringList pending_;
    QString current_run_;
};

}  // namespace f2c_cpp
