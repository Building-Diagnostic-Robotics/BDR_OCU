/**
 * @file upload_runner.cpp
 * @brief Implementation of UploadStateProbe + UploadRunner.
 */

#include "upload_runner.hpp"

#include "ocu_log_store.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSet>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTime>
#include <QStringList>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cstdio>
#include <unistd.h>

namespace f2c_cpp {

namespace {

// Sentinel files `uploader.py` keeps next to the data. Excluded from
// the "total files" count so progress reads N/N at completion.
// The `.tmp` siblings are atomic-write leftovers (`save_state` /
// manifest write). Any other `*.tmp` name is data.
const char* const kStateFile = "upload_state.json";
const char* const kPauseFile = "pause.flag";
const char* const kManifestFile = "manifest.json";
const char* const kCloudVerifiedFile = "cloud_verified.json";
const char* const kThumbManifestFile = "thumbsync_manifest.json";

bool isUploadBookkeepingName(const QString& name) {
    const QString state = QLatin1String(kStateFile);
    const QString manifest = QLatin1String(kManifestFile);
    const QString verified = QLatin1String(kCloudVerifiedFile);
    const QString recopy = QLatin1String("needs_recopy.json");
    return name == state || name == QLatin1String(kPauseFile) ||
           name == manifest || name == verified || name == recopy ||
           name == state + QStringLiteral(".tmp") ||
           name == manifest + QStringLiteral(".tmp") ||
           name == verified + QStringLiteral(".tmp") ||
           name == recopy + QStringLiteral(".tmp");
}

// Where create_deb.sh installs the canonical script.
const char* const kInstalledLocalScript =
    "/usr/share/bdr-coverage-planner/uploader.py";

// Length of a JSON array field in a small sentinel file; 0 on any
// problem. Used for `manifest.json:files` and `upload_state.json:completed`.
int jsonArrayLength(const QString& path, const char* key) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return 0;
    return doc.object().value(QLatin1String(key)).toArray().size();
}

bool isSectionFolderName(const QString& name) {
    return name.startsWith(QLatin1String("Section_")) ||
           name.startsWith(QLatin1String("Mission_"));
}

QDateTime parseFolderStamp(const QString& date_folder, const QString& section,
                           const QString& hms_override = QString()) {
    QDate day = QDate::fromString(date_folder, QStringLiteral("MMMM_d_yyyy"));
    if (!day.isValid()) {
        day = QDate::fromString(date_folder, QStringLiteral("MMMM_dd_yyyy"));
    }
    QString hms = hms_override;
    if (hms.isEmpty()) {
        static const QRegularExpression kHmsRx(QStringLiteral("_(\\d{6})$"));
        const auto m = kHmsRx.match(section);
        if (m.hasMatch()) {
            hms = m.captured(1);
        }
    }
    const QTime clock = QTime::fromString(hms, QStringLiteral("HHmmss"));
    if (!day.isValid()) {
        return {};
    }
    return QDateTime(day, clock.isValid() ? clock : QTime(0, 0));
}

void fillTargetMetadata(UploadTarget& t) {
    const QString cfg_name = t.isMission()
                                 ? QStringLiteral("mission_config.json")
                                 : QStringLiteral("session_config.json");
    QFile f(t.data_path + QLatin1Char('/') + cfg_name);
    QJsonObject obj;
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject()) {
            obj = doc.object();
        }
    }
    t.building_name = obj.value(QStringLiteral("building_name")).toString();
    if (t.building_name.isEmpty()) {
        t.building_name = t.building_slug;
    }
    t.operator_name = obj.value(QStringLiteral("operator_name")).toString();

    const QString iso = obj.value(QStringLiteral("mission_started_at")).toString();
    if (!iso.isEmpty()) {
        QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
        if (!dt.isValid()) {
            dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
        }
        if (dt.isValid()) {
            t.captured_at = dt;
            return;
        }
    }
    QString day = obj.value(QStringLiteral("day_name")).toString();
    if (day.isEmpty()) {
        day = t.date_folder;
    }
    QString hms = obj.value(QStringLiteral("timestamp")).toString();
    if (hms.isEmpty()) {
        hms = obj.value(QStringLiteral("mission_timestamp")).toString();
    }
    t.captured_at = parseFolderStamp(day, t.section_name, hms);
}

// Single quote a string for safe inclusion inside a `bash -lc '...'`
// remote command. We never interpolate operator-controlled data into
// this, but `run_id` derives from operator-typed building names so we
// keep escaping disciplined.
QString shellSingleQuote(const QString& s) {
    QString out = QStringLiteral("'");
    for (const QChar c : s) {
        if (c == QLatin1Char('\'')) {
            out += QStringLiteral("'\\''");
        } else {
            out += c;
        }
    }
    out += QLatin1Char('\'');
    return out;
}

// Default install path for `uploader.py` after `colcon build` lands the
// pilot_control package on the robot. Matches the directory used for
// `finalize_mission_local.py` (same packaging rule in
// `pilot_ws/src/pilot_control/CMakeLists.txt`).
QString defaultRemoteScriptPath(const QString& ssh_user) {
    return QStringLiteral(
               "/home/%1/pilot_ws/install/pilot_control/lib/pilot_control/uploader.py")
        .arg(ssh_user);
}

QStringList sshOptions() {
    return QStringList()
        << QStringLiteral("-o") << QStringLiteral("ConnectTimeout=8")
        << QStringLiteral("-o") << QStringLiteral("ServerAliveInterval=10")
        << QStringLiteral("-o") << QStringLiteral("ServerAliveCountMax=3")
        << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=no")
        << QStringLiteral("-o") << QStringLiteral("UserKnownHostsFile=/dev/null")
        << QStringLiteral("-o") << QStringLiteral("BatchMode=yes");
}

// Probe script: emit one `SECTION|...|...|` line per section/mission
// folder under /R_DATA at depth 3 (date/building/section). Status
// derives from sentinel files; counts come from `find` and `du`.
QString buildProbeRemoteCommand(const QString& data_root) {
    const QString quoted_root = shellSingleQuote(data_root);
    return QStringLiteral(
               "set -e; cd %1 2>/dev/null || { echo NO_R_DATA; exit 0; }; "
               "find . -mindepth 3 -maxdepth 3 -type d "
               "\\( -name 'Section_*' -o -name 'Mission_*' \\) "
               "| sort | while read -r p; do "
               "  date=$(echo \"$p\" | cut -d/ -f2); "
               "  building=$(echo \"$p\" | cut -d/ -f3); "
               "  section=$(basename \"$p\"); "
               "  if [ -f \"$p/manifest.json\" ] && [ ! -f \"$p/upload_state.json\" ]; then "
               "    state=done; "
               "    completed=$(python3 -c \"import json,sys;d=json.load(open(sys.argv[1]));print(len(d.get('files',[])))\" \"$p/manifest.json\" 2>/dev/null || echo 0); "
               "  elif [ -f \"$p/upload_state.json\" ]; then "
               "    state=partial; "
               "    completed=$(python3 -c \"import json,sys;d=json.load(open(sys.argv[1]));print(len(d.get('completed',[])))\" \"$p/upload_state.json\" 2>/dev/null || echo 0); "
               "  else "
               "    state=none; completed=0; "
               "  fi; "
               "  total=$(find \"$p\" -type f "
               "    ! -name upload_state.json ! -name pause.flag ! -name manifest.json "
               "    ! -name cloud_verified.json "
               "    ! -name upload_state.json.tmp ! -name manifest.json.tmp "
               "    ! -name cloud_verified.json.tmp "
               "    ! -name needs_recopy.json ! -name needs_recopy.json.tmp "
               "    | wc -l); "
               "  size=$(du -sb \"$p\" 2>/dev/null | awk '{print $1}'); "
               "  cfg=\"$p/session_config.json\"; "
               "  case \"$section\" in Mission_*) cfg=\"$p/mission_config.json\";; esac; "
               "  meta=$(python3 -c \""
               "import json,sys\n"
               "b=o=t=''\n"
               "try:\n"
               " d=json.load(open(sys.argv[1]))\n"
               " b=str(d.get('building_name') or '')\n"
               " o=str(d.get('operator_name') or '')\n"
               " t=str(d.get('mission_started_at') or d.get('timestamp') or d.get('mission_timestamp') or '')\n"
               "except Exception:\n"
               " pass\n"
               "print(b.replace('|',' ')+'|'+o.replace('|',' ')+'|'+t.replace('|',' '))\n"
               "\" \"$cfg\" 2>/dev/null || echo '||'); "
               "  echo \"SECTION|$date|$building|$section|$state|$completed|$total|$size|$meta\"; "
               "done")
        .arg(quoted_root);
}

UploadStatus parseStatus(const QString& token) {
    if (token == QLatin1String("done")) return UploadStatus::Done;
    if (token == QLatin1String("partial")) return UploadStatus::Partial;
    return UploadStatus::None;
}

}  // namespace

QStringList uploaderArguments(const QString& script_path,
                              const QString& data_path,
                              const QString& robot_id,
                              const QString& run_id,
                              bool force) {
    QStringList args{QStringLiteral("-u"), script_path, data_path, robot_id, run_id};
    if (force) args << QStringLiteral("--force");
    return args;
}

namespace {

bool isSha256Hex(const QString& text) {
    if (text.size() != 64) return false;
    for (const QChar c : text) {
        const ushort u = c.unicode();
        const bool hex = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') ||
                         (u >= 'A' && u <= 'F');
        if (!hex) return false;
    }
    return true;
}

QString cloudVerifiedPath(const QString& folder) {
    return folder + QLatin1Char('/') + QLatin1String(kCloudVerifiedFile);
}

bool removeIfPresent(const QString& path) {
    if (!QFileInfo::exists(path)) return true;
    return QFile::remove(path);
}

}  // namespace

QByteArray fileSha256Hex(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (true) {
        const QByteArray chunk = file.read(1 << 20);
        if (chunk.isEmpty()) break;
        hash.addData(chunk);
    }
    if (file.error() != QFile::NoError) return {};
    return hash.result().toHex();
}

CloudVerifiedProof readCloudVerified(const QString& folder) {
    CloudVerifiedProof proof;
    const QString manifest = folder + QLatin1Char('/') + QLatin1String(kManifestFile);
    const QString state = folder + QLatin1Char('/') + QLatin1String(kStateFile);
    if (!QFileInfo::exists(manifest) || QFileInfo::exists(state)) return proof;

    QFile file(cloudVerifiedPath(folder));
    if (!file.open(QIODevice::ReadOnly)) return proof;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return proof;
    const QString expected = doc.object()
                                 .value(QStringLiteral("manifest_sha256"))
                                 .toString()
                                 .trimmed();
    if (!isSha256Hex(expected)) return proof;

    const QByteArray actual = fileSha256Hex(manifest);
    if (actual.isEmpty()) return proof;
    if (QString::fromLatin1(actual).compare(expected, Qt::CaseInsensitive) != 0) {
        return proof;
    }
    proof.valid = true;
    proof.manifest_sha256 = QString::fromLatin1(actual);
    return proof;
}

bool writeCloudVerified(const QString& folder,
                        const QString& client_id,
                        const QString& robot_id,
                        int file_count) {
    const QString manifest = folder + QLatin1Char('/') + QLatin1String(kManifestFile);
    const QByteArray hash = fileSha256Hex(manifest);
    if (hash.size() != 64) return false;

    QJsonObject obj;
    obj.insert(QStringLiteral("schema"), 1);
    obj.insert(QStringLiteral("manifest_sha256"), QString::fromLatin1(hash));
    obj.insert(QStringLiteral("verified_at"),
               QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")));
    obj.insert(QStringLiteral("client_id"), client_id);
    obj.insert(QStringLiteral("robot_id"), robot_id);
    obj.insert(QStringLiteral("files"), qMax(0, file_count));
    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';

    const QString dest = cloudVerifiedPath(folder);
    const QString tmp = dest + QStringLiteral(".tmp");
    QFile out(tmp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (out.write(bytes) != bytes.size() || !out.flush()) {
        out.close();
        QFile::remove(tmp);
        return false;
    }
    // Best effort. exFAT (the stick) can reject fsync; the rename is
    // what makes the file appear whole.
    const int fd = static_cast<int>(out.handle());
    if (fd >= 0) {
        ::fsync(fd);
    }
    out.close();
    if (::rename(QFile::encodeName(tmp).constData(),
                 QFile::encodeName(dest).constData()) != 0) {
        QFile::remove(tmp);
        return false;
    }
    return true;
}

bool removeCloudVerified(const QString& folder) {
    const QString dest = cloudVerifiedPath(folder);
    return removeIfPresent(dest) && removeIfPresent(dest + QStringLiteral(".tmp"));
}

bool settleCloudVerified(const QString& folder,
                         bool matches,
                         const QString& client_id,
                         const QString& robot_id,
                         int file_count) {
    if (!matches) return removeCloudVerified(folder);
    return writeCloudVerified(folder, client_id, robot_id, file_count);
}

// ---------------------------------------------------------------------------
// Stick copy check
// ---------------------------------------------------------------------------

const char* const kNeedsRecopyFile = "needs_recopy.json";

BuildingCopyRecords loadBuildingCopyRecords(const QString& building_dir) {
    BuildingCopyRecords out;
    const QDir dir(building_dir);
    const QStringList missions = dir.entryList(
        {QStringLiteral("Mission_*")},
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const QString& mission : missions) {
        const QString path = dir.filePath(mission) + QLatin1Char('/') +
                             QLatin1String(kThumbManifestFile);
        if (!QFileInfo(path).isFile()) continue;
        BuildingCopyRecords::Record rec;
        rec.mission = mission;
        rec.dir = dir.filePath(mission);
        QFile f(path);
        QJsonParseError err{};
        const QJsonDocument doc = f.open(QIODevice::ReadOnly)
                                      ? QJsonDocument::fromJson(f.readAll(), &err)
                                      : QJsonDocument();
        if (doc.isObject()) {
            rec.readable = true;
            const QJsonObject obj = doc.object();
            for (const QJsonValue& v : obj.value(QStringLiteral("sections")).toArray()) {
                if (v.isString()) rec.sections << v.toString();
            }
            const QJsonValue files = obj.value(QStringLiteral("files"));
            if (files.isObject()) {
                rec.per_file = true;
                out.any_per_file = true;
                const QJsonObject map = files.toObject();
                rec.sizes.reserve(map.size());
                for (auto it = map.begin(); it != map.end(); ++it) {
                    const QJsonValue size = it.value().toObject().value(QStringLiteral("size"));
                    // A malformed entry is the uploader's to report; -1
                    // makes it count as wrong here, so the folder is not
                    // queued only to fail.
                    rec.sizes.insert(it.key(), size.isDouble()
                                                   ? static_cast<qint64>(size.toDouble())
                                                   : -1);
                }
            }
        }
        out.records.append(rec);
    }
    return out;
}

StickCopyVerdict checkStickCopy(const BuildingCopyRecords& records,
                                const QString& folder,
                                const QHash<QString, qint64>& local_sizes) {
    StickCopyVerdict verdict;
    const BuildingCopyRecords::Record* use = nullptr;

    // A mission folder carries its own record.
    for (const auto& rec : records.records) {
        if (rec.mission == folder) {
            if (!rec.readable) {
                verdict.problem = StickCopyProblem::Damaged;
                verdict.mission_dir = rec.dir;
                return verdict;
            }
            use = &rec;
            break;
        }
    }
    if (!use) {
        // A section uses the sibling mission record that lists it. An
        // unreadable sibling cannot say what it covers, so it blocks
        // every section here (uploader.py does the same).
        int matches = 0;
        for (const auto& rec : records.records) {
            if (!rec.readable) {
                verdict.problem = StickCopyProblem::Damaged;
                verdict.mission_dir = rec.dir;
                return verdict;
            }
            if (rec.sections.contains(folder)) {
                ++matches;
                use = &rec;
            }
        }
        if (matches > 1) {
            verdict.problem = StickCopyProblem::Damaged;
            return verdict;
        }
    }
    if (!use) {
        if (records.any_per_file) verdict.problem = StickCopyProblem::Unfinished;
        return verdict;
    }
    verdict.mission_dir = use->dir;
    if (!use->per_file) return verdict;  // older robot: nothing to compare

    const QString prefix = folder + QLatin1Char('/');
    for (auto it = use->sizes.cbegin(); it != use->sizes.cend(); ++it) {
        if (!it.key().startsWith(prefix)) continue;
        const QString rel = it.key().mid(prefix.size());
        const auto found = local_sizes.constFind(rel);
        if (found == local_sizes.cend() || found.value() != it.value()) {
            verdict.bad_keys << it.key();
        }
    }
    if (!verdict.bad_keys.isEmpty()) {
        verdict.bad_keys.sort();
        verdict.problem = StickCopyProblem::Damaged;
    }
    return verdict;
}

bool mergeNeedsRecopy(const QString& mission_dir, const QStringList& keys) {
    if (mission_dir.isEmpty() || keys.isEmpty()) return true;
    const QString dest = mission_dir + QLatin1Char('/') + QLatin1String(kNeedsRecopyFile);
    QStringList files;
    {
        QFile in(dest);
        if (in.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(in.readAll());
            for (const QJsonValue& v : doc.object().value(QStringLiteral("files")).toArray()) {
                if (v.isString()) files << v.toString();
            }
        }
    }
    const int before = files.size();
    QSet<QString> seen(files.begin(), files.end());
    for (const QString& key : keys) {
        if (!seen.contains(key)) {
            seen.insert(key);
            files << key;
        }
    }
    if (files.size() == before && QFileInfo::exists(dest)) return true;
    files.sort();

    QJsonObject obj;
    obj.insert(QStringLiteral("schema"), 1);
    obj.insert(QStringLiteral("written_by"), QStringLiteral("ocu"));
    obj.insert(QStringLiteral("updated_at"),
               QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")));
    obj.insert(QStringLiteral("files"), QJsonArray::fromStringList(files));
    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Indented);

    const QString tmp = dest + QStringLiteral(".tmp");
    QFile out(tmp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (out.write(bytes) != bytes.size() || !out.flush()) {
        out.close();
        QFile::remove(tmp);
        return false;
    }
    const int fd = static_cast<int>(out.handle());
    if (fd >= 0) ::fsync(fd);  // best effort, as writeCloudVerified
    out.close();
    if (::rename(QFile::encodeName(tmp).constData(),
                 QFile::encodeName(dest).constData()) != 0) {
        QFile::remove(tmp);
        return false;
    }
    return true;
}

namespace {

// Oldest visit first. Equal timestamps put Mission_* ahead of Section_*,
// which is the tie on a scan whose mission and section share a minute.
// A later visit's mission keeps its own later timestamp, so it does not
// jump ahead of an earlier visit's section.
bool uploadBefore(const UploadTarget& a, const UploadTarget& b) {
    const bool a_ok = a.captured_at.isValid();
    const bool b_ok = b.captured_at.isValid();
    if (a_ok != b_ok) return a_ok;
    if (a_ok && a.captured_at != b.captured_at) return a.captured_at < b.captured_at;
    if (a.date_folder != b.date_folder) return a.date_folder < b.date_folder;
    if (a.isMission() != b.isMission()) return a.isMission();
    return a.run_id < b.run_id;
}

bool folderNeedsUpload(const UploadTarget& target) {
    if (target.status != UploadStatus::Done) return true;
    return target.cloud_check == CloudCheck::Missing;
}

/// Not uploaded yet, and the stick copy has a problem only the robot
/// can fix. Never queued.
bool folderNeedsRobot(const UploadTarget& target) {
    return target.status != UploadStatus::Done &&
           target.copy_problem != StickCopyProblem::None;
}

QString needsRobotText(bool any_damaged) {
    return any_damaged
               ? QStringLiteral("Copy damaged — plug drive into ROOFUS")
               : QStringLiteral("Copy not finished — plug drive into ROOFUS");
}

UploadRowItem operatorFolderItem(const UploadTarget& target) {
    if (target.isMission()) return {QStringLiteral("Other scan files"), QString()};
    static const QRegularExpression kIndex(QStringLiteral("^Section_(\\d+)"));
    const QRegularExpressionMatch m = kIndex.match(target.section_name);
    UploadRowItem item;
    item.name = m.hasMatch() ? QStringLiteral("Section %1").arg(m.captured(1).toInt())
                             : QStringLiteral("Section");
    if (target.captured_at.isValid()) {
        item.when = QLocale().toString(target.captured_at,
                                       QStringLiteral("MMM d, h:mm AP"));
    }
    return item;
}

UploadRowSummary summarizeBuilding(const QList<UploadTarget>& oldest_first) {
    UploadRowSummary s;
    // Sections first, oldest first; "Other scan files" after them.
    QList<UploadRowItem> other_fix;
    QList<UploadRowItem> other_again;
    for (const UploadTarget& target : oldest_first) {
        const bool robot = folderNeedsRobot(target);
        const bool gone = !robot && target.status == UploadStatus::Done &&
                          target.cloud_check == CloudCheck::Missing;
        if (target.isMission()) {
            if (robot && other_fix.isEmpty()) other_fix << operatorFolderItem(target);
            if (gone && other_again.isEmpty()) other_again << operatorFolderItem(target);
            continue;
        }
        if (robot) s.fix_on_robot << operatorFolderItem(target);
        if (gone) s.upload_again << operatorFolderItem(target);
        ++s.sections;
        if (robot) ++s.need_robot;
        else if (gone) ++s.missing;
        else if (target.status == UploadStatus::Done) ++s.uploaded;
        else ++s.left;
    }
    s.fix_on_robot += other_fix;
    s.upload_again += other_again;
    return s;
}

}  // namespace

QString uploadRowTooltipText(const UploadRowSummary& s) {
    QStringList summary;
    summary << (s.sections == 1 ? QStringLiteral("1 section")
                                : QStringLiteral("%1 sections").arg(s.sections));
    if (s.uploaded > 0) summary << QStringLiteral("%1 uploaded").arg(s.uploaded);
    if (s.left > 0) summary << QStringLiteral("%1 left to upload").arg(s.left);
    if (s.need_robot > 0) {
        summary << QStringLiteral("%1 need%2 ROOFUS")
                       .arg(s.need_robot)
                       .arg(s.need_robot == 1 ? QStringLiteral("s") : QString());
    }
    if (s.missing > 0) summary << QStringLiteral("%1 missing in cloud").arg(s.missing);

    QStringList lines{summary.join(QStringLiteral(" · "))};
    const auto addGroup = [&lines](const QString& heading,
                                   const QList<UploadRowItem>& items) {
        if (items.isEmpty()) return;
        lines << heading;
        const int shown = qMin(kUploadRowListCap, int(items.size()));
        for (int i = 0; i < shown; ++i) {
            const UploadRowItem& item = items.at(i);
            lines << QStringLiteral("  ") + item.name +
                         (item.when.isEmpty() ? QString()
                                              : QStringLiteral(" · ") + item.when);
        }
        if (items.size() > shown) {
            lines << QStringLiteral("  … and %1 more").arg(items.size() - shown);
        }
    };
    addGroup(QStringLiteral("Plug the drive into ROOFUS to fix:"), s.fix_on_robot);
    addGroup(QStringLiteral("Missing in the cloud — select to upload again:"),
             s.upload_again);
    return lines.join(QLatin1Char('\n'));
}

namespace {

UploadBuildingRow makeBuildingRow(const QString& key, QList<UploadTarget> members) {
    UploadBuildingRow row;
    row.key = key;
    if (members.isEmpty()) return row;

    std::sort(members.begin(), members.end(), uploadBefore);
    for (const UploadTarget& target : members) {
        row.run_ids << target.run_id;
        if (target.total_bytes > 0) row.total_bytes += target.total_bytes;
    }

    const UploadTarget* newest = &members.first();
    for (const UploadTarget& target : members) {
        if (!target.captured_at.isValid()) continue;
        if (!newest->captured_at.isValid() || target.captured_at > newest->captured_at) {
            newest = &target;
        }
    }
    row.newest = newest->captured_at;
    row.newest_date_folder = newest->date_folder;
    row.building_label = newest->building_name.trimmed();
    if (row.building_label.isEmpty()) row.building_label = newest->building_slug.trimmed();
    if (row.building_label.isEmpty()) row.building_label = newest->section_name;
    if (row.building_label.isEmpty()) row.building_label = QStringLiteral("Unknown");

    QList<UploadTarget> newest_first = members;
    std::sort(newest_first.begin(), newest_first.end(),
              [](const UploadTarget& a, const UploadTarget& b) {
                  return uploadBefore(b, a);
              });
    for (const UploadTarget& target : newest_first) {
        if (!target.operator_name.trimmed().isEmpty()) {
            row.operator_name = target.operator_name.trimmed();
            break;
        }
    }

    bool any_missing = false;
    bool any_open = false;
    bool any_partial = false;
    bool any_checking = false;
    bool all_verified = true;
    int needs_robot = 0;
    int sections_need_robot = 0;  // what the badge counts; missions are hidden
    bool any_damaged = false;
    qint64 open_done = 0;
    qint64 open_total = 0;
    for (const UploadTarget& target : members) {
        if (folderNeedsRobot(target)) {
            ++needs_robot;
            if (!target.isMission()) ++sections_need_robot;
            all_verified = false;
            if (target.copy_problem == StickCopyProblem::Damaged) any_damaged = true;
            continue;
        }
        if (target.status == UploadStatus::Done &&
            target.cloud_check == CloudCheck::Missing) {
            any_missing = true;
            all_verified = false;
            continue;
        }
        if (target.status != UploadStatus::Done) {
            any_open = true;
            all_verified = false;
            if (target.status == UploadStatus::Partial) any_partial = true;
            open_done += qMax(0, target.completed_files);
            open_total += qMax(0, target.total_files);
            continue;
        }
        if (target.cloud_check == CloudCheck::Checking) any_checking = true;
        if (target.cloud_check != CloudCheck::Verified) all_verified = false;
    }

    if (any_missing) {
        row.badge = UploadRowBadge::Missing;
        row.badge_text = QStringLiteral("Missing in cloud");
        row.selectable = true;
    } else if (any_open && any_partial) {
        row.badge = UploadRowBadge::Partial;
        row.selectable = true;
        if (open_total > 0) {
            row.partial_percent = static_cast<int>(
                qBound<qint64>(0, open_done * 100 / open_total, 100));
            row.badge_text = QStringLiteral("Partial · %1%").arg(row.partial_percent);
        } else {
            row.badge_text = QStringLiteral("Partial");
        }
    } else if (any_open) {
        row.badge = UploadRowBadge::Pending;
        row.badge_text = QStringLiteral("Pending");
        row.selectable = true;
    } else if (needs_robot > 0) {
        // Nothing here can upload until the robot repairs the copy.
        row.badge = UploadRowBadge::NeedsRobot;
        row.badge_text = needsRobotText(any_damaged);
    } else if (any_checking) {
        row.badge = UploadRowBadge::Checking;
        row.badge_text = QStringLiteral("Uploaded · checking…");
    } else if (all_verified) {
        row.badge = UploadRowBadge::Verified;
        row.badge_text = QStringLiteral("Uploaded · Verified");
    } else {
        row.badge = UploadRowBadge::Uploaded;
        row.badge_text = QStringLiteral("Uploaded");
    }

    if (needs_robot > 0 && row.badge != UploadRowBadge::NeedsRobot) {
        // The rest of the building still uploads; say what is held back.
        // Counts sections, the unit the operator knows; a mission folder
        // alone still says ROOFUS is needed, without a number.
        row.badge_text += sections_need_robot > 0
                              ? QStringLiteral(" · %1 need%2 ROOFUS")
                                    .arg(sections_need_robot)
                                    .arg(sections_need_robot == 1 ? QStringLiteral("s")
                                                                  : QString())
                              : QStringLiteral(" · needs ROOFUS");
    }

    row.summary = summarizeBuilding(members);
    row.tooltip = uploadRowTooltipText(row.summary);
    return row;
}

}  // namespace

QString uploadBuildingKey(const UploadTarget& target) {
    const QString slug = target.building_slug.trimmed();
    if (!slug.isEmpty()) return slug;
    // Leading unit separator so a slug can never equal this key.
    return QStringLiteral("\x1e") + target.run_id;
}

QList<UploadBuildingRow> groupUploadTargets(
    const QHash<QString, UploadTarget>& targets) {
    QHash<QString, QList<UploadTarget>> buckets;
    for (const UploadTarget& target : targets) {
        if (target.run_id.isEmpty()) continue;
        buckets[uploadBuildingKey(target)].append(target);
    }

    QList<UploadBuildingRow> rows;
    rows.reserve(buckets.size());
    for (auto it = buckets.cbegin(); it != buckets.cend(); ++it) {
        rows.append(makeBuildingRow(it.key(), it.value()));
    }
    std::sort(rows.begin(), rows.end(),
              [](const UploadBuildingRow& a, const UploadBuildingRow& b) {
                  const bool a_ok = a.newest.isValid();
                  const bool b_ok = b.newest.isValid();
                  if (a_ok != b_ok) return a_ok;
                  if (a_ok && a.newest != b.newest) return a.newest > b.newest;
                  return a.key < b.key;
              });
    return rows;
}

QList<UploadTarget> uploadTargetsForRow(const QHash<QString, UploadTarget>& targets,
                                        const UploadBuildingRow& row) {
    QList<UploadTarget> out;
    for (const QString& run_id : row.run_ids) {
        const auto it = targets.constFind(run_id);
        if (it == targets.constEnd()) continue;
        if (!folderNeedsUpload(*it)) continue;
        if (folderNeedsRobot(*it)) continue;
        UploadTarget copy = *it;
        // The cloud check records the decision on the folder. Setting
        // the flag here means a missing folder cannot resume against
        // the manifest that just failed the check. A retry has already
        // moved the folder to Partial, so it does not pass --force.
        if (copy.cloud_check == CloudCheck::Missing) copy.force_reupload = true;
        out.append(copy);
    }
    return out;
}

// ---------------------------------------------------------------------------
// BuildingUploadProgress
// ---------------------------------------------------------------------------

namespace {
int folderFiles(const UploadTarget& t) {
    return qMax(0, qMax(t.total_files, t.completed_files));
}
}  // namespace

void BuildingUploadProgress::reset(const QList<UploadTarget>& queue) {
    groups_.clear();
    group_ = -1;
    folder_in_group_ = 0;
    base_done_ = 0;
    current_done_ = 0;
    current_total_ = 0;
    current_run_id_.clear();
    for (const UploadTarget& t : queue) {
        const QString key = uploadBuildingKey(t);
        if (groups_.isEmpty() || groups_.last().key != key) {
            Group g;
            g.key = key;
            g.label = t.building_name.trimmed().isEmpty() ? t.building_slug
                                                          : t.building_name.trimmed();
            if (g.label.isEmpty()) g.label = t.section_name;
            groups_.append(g);
        }
        groups_.last().files += folderFiles(t);
        groups_.last().folders += 1;
    }
}

void BuildingUploadProgress::targetStarted(const UploadTarget& target) {
    if (!current_run_id_.isEmpty() && target.run_id == current_run_id_) {
        return;  // a retry of the same folder keeps what it already sent
    }
    if (!current_run_id_.isEmpty()) targetFinished();
    const QString key = uploadBuildingKey(target);
    if (group_ < 0 || group_ >= groups_.size() || groups_.at(group_).key != key) {
        int next = group_ + 1;
        while (next < groups_.size() && groups_.at(next).key != key) ++next;
        if (next >= groups_.size()) {
            // Not in the plan (should not happen): give it a group of its own.
            Group g;
            g.key = key;
            g.label = target.building_slug;
            g.files = folderFiles(target);
            g.folders = 1;
            groups_.append(g);
            next = groups_.size() - 1;
        }
        group_ = next;
        folder_in_group_ = 0;
        base_done_ = 0;
    }
    folder_in_group_ += 1;
    current_run_id_ = target.run_id;
    current_done_ = qMax(0, target.completed_files);
    current_total_ = folderFiles(target);
}

void BuildingUploadProgress::fileDone() {
    current_done_ += 1;
}

void BuildingUploadProgress::targetFinished() {
    if (current_run_id_.isEmpty()) return;
    base_done_ += qMax(current_total_, current_done_);
    current_done_ = 0;
    current_total_ = 0;
    current_run_id_.clear();
}

int BuildingUploadProgress::total() const {
    const int planned =
        (group_ >= 0 && group_ < groups_.size()) ? groups_.at(group_).files : 0;
    // A folder can hold more than the probe counted (OCU logs added at
    // launch); the bar never runs past its end.
    return qMax(planned, done());
}

int BuildingUploadProgress::folderCount() const {
    return (group_ >= 0 && group_ < groups_.size()) ? groups_.at(group_).folders : 0;
}

QString BuildingUploadProgress::buildingLabel() const {
    return (group_ >= 0 && group_ < groups_.size()) ? groups_.at(group_).label
                                                    : QString();
}

QList<UploadTarget> cloudCheckTargets(const QHash<QString, UploadTarget>& targets) {
    QList<UploadTarget> unproved;
    QList<UploadTarget> proved;
    for (const UploadTarget& target : targets) {
        if (target.run_id.isEmpty()) continue;
        if (target.status != UploadStatus::Done) continue;
        (target.cloud_check == CloudCheck::Verified ? proved : unproved).append(target);
    }
    std::sort(unproved.begin(), unproved.end(), uploadBefore);
    std::sort(proved.begin(), proved.end(), uploadBefore);
    // A proof says S3 held the folder when it was written, not now: an
    // object deleted from S3 later must show up as missing while the
    // stick still has the data. Unproved folders go first.
    return unproved + proved;
}

// ---------------------------------------------------------------------------
// UploadStateProbe
// ---------------------------------------------------------------------------

UploadStateProbe::UploadStateProbe(QObject* parent) : QObject(parent) {}

UploadStateProbe::~UploadStateProbe() {
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        proc_->disconnect(this);
        proc_->kill();
        proc_->waitForFinished(500);
    }
    // A local walk in flight holds no reference to `this` (its
    // QFutureWatcher is parented here and dies with us), so nothing
    // else to do.
}

void UploadStateProbe::setRemote(const QString& host, const QString& ssh_user) {
    remote_host_ = host.trimmed();
    ssh_user_ = ssh_user.trimmed();
}

void UploadStateProbe::setDataRoot(const QString& data_root) {
    if (!data_root.trimmed().isEmpty()) {
        data_root_ = QDir::cleanPath(data_root.trimmed());
    }
}

bool UploadStateProbe::isRunning() const {
    return local_running_ || (proc_ && proc_->state() != QProcess::NotRunning);
}

QList<UploadTarget> UploadStateProbe::scanLocalDataRoot(const QString& data_root) {
    QList<UploadTarget> out;
    const QDir root(data_root);
    if (!root.exists()) return out;

    const QDir::Filters dir_filters =
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks;
    const QStringList dates =
        root.entryList(dir_filters, QDir::Name);
    for (const QString& date : dates) {
        // The robot's offload bookkeeping lives in `.offload/`; a hidden
        // folder never holds mission data.
        if (date.startsWith(QLatin1Char('.'))) continue;
        const QDir date_dir(root.filePath(date));
        const QStringList buildings = date_dir.entryList(dir_filters, QDir::Name);
        for (const QString& building : buildings) {
            const QDir building_dir(date_dir.filePath(building));
            const QStringList sections =
                building_dir.entryList(dir_filters, QDir::Name);
            // Read once per building: a mission record can list every
            // section here and hold tens of thousands of entries.
            const BuildingCopyRecords copy_records =
                loadBuildingCopyRecords(building_dir.absolutePath());
            QHash<QString, QStringList> recopy_by_mission;
            for (const QString& section : sections) {
                if (!isSectionFolderName(section)) continue;

                UploadTarget t;
                t.date_folder = date;
                t.building_slug = building;
                t.section_name = section;
                t.run_id = QStringLiteral("%1/%2/%3").arg(date, building, section);
                t.data_path = building_dir.filePath(section);

                const QString manifest =
                    t.data_path + QLatin1Char('/') + QLatin1String(kManifestFile);
                const QString state =
                    t.data_path + QLatin1Char('/') + QLatin1String(kStateFile);
                // `uploader.py` deletes upload_state.json only after
                // /complete returns. A manifest that still has the state
                // file beside it was not confirmed.
                if (QFileInfo::exists(manifest) && !QFileInfo::exists(state)) {
                    t.status = UploadStatus::Done;
                    t.completed_files = jsonArrayLength(manifest, "files");
                    // A valid proof survives an OCU restart, so the dialog
                    // can show Verified without calling the cloud again.
                    if (readCloudVerified(t.data_path).valid) {
                        t.cloud_check = CloudCheck::Verified;
                    }
                } else if (QFileInfo::exists(state)) {
                    t.status = UploadStatus::Partial;
                    t.completed_files = jsonArrayLength(state, "completed");
                } else {
                    t.status = UploadStatus::None;
                    t.completed_files = 0;
                }

                // Same counting rule as the SSH probe: every regular file
                // except the bookkeeping sentinels; bytes over everything.
                const bool check_copy = t.status != UploadStatus::Done;
                QHash<QString, qint64> local_sizes;
                const int prefix_len = t.data_path.size() + 1;
                QDirIterator it(t.data_path,
                                QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                                QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    const QFileInfo fi = it.fileInfo();
                    t.total_bytes += fi.size();
                    if (check_copy && !fi.isSymLink()) {
                        local_sizes.insert(it.filePath().mid(prefix_len), fi.size());
                    }
                    if (isUploadBookkeepingName(fi.fileName())) {
                        continue;
                    }
                    t.total_files += 1;
                }
                if (check_copy) {
                    // A finished upload was already checked file by file
                    // against the robot's hashes; only pending work is
                    // checked here, before anything is queued.
                    const StickCopyVerdict verdict =
                        checkStickCopy(copy_records, section, local_sizes);
                    t.copy_problem = verdict.problem;
                    t.copy_bad_files = verdict.bad_keys.size();
                    if (!verdict.bad_keys.isEmpty() && !verdict.mission_dir.isEmpty()) {
                        recopy_by_mission[verdict.mission_dir] << verdict.bad_keys;
                    }
                }
                fillTargetMetadata(t);
                out.append(t);
            }
            // Tell the robot which files to re-copy the next time the
            // stick is plugged into it. Best effort: the badge still
            // blocks the upload if the stick refuses the write.
            for (auto rit = recopy_by_mission.cbegin(); rit != recopy_by_mission.cend(); ++rit) {
                if (!mergeNeedsRecopy(rit.key(), rit.value())) {
                    qWarning().noquote()
                        << "[UploadStateProbe] could not write needs_recopy.json in"
                        << rit.key();
                }
            }
        }
    }
    return out;
}

void UploadStateProbe::start() {
    if (source_ == UploadSource::ThumbDrive) {
        // A local walk may be superseded: re-probing after the stick was
        // swapped must not wait for (or report) the stale walk.
        startLocal();
        return;
    }
    if (isRunning()) {
        return;
    }
    startSsh();
}

void UploadStateProbe::startLocal() {
    const QString root = data_root_;
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        emit targetsReady(false, {},
                          QStringLiteral("Thumb drive is not mounted."));
        return;
    }

    local_running_ = true;
    const int generation = ++local_generation_;

    auto* watcher = new QFutureWatcher<QList<UploadTarget>>(this);
    connect(watcher, &QFutureWatcher<QList<UploadTarget>>::finished, this,
            [this, watcher, generation, root]() {
                const QList<UploadTarget> result = watcher->result();
                watcher->deleteLater();
                if (generation != local_generation_) {
                    // Root changed underneath us (stick swapped);
                    // the newer walk owns the signal.
                    return;
                }
                local_running_ = false;
                if (!QFileInfo(root).isDir()) {
                    emit targetsReady(false, {},
                                      QStringLiteral("Thumb drive was removed "
                                                     "during the scan."));
                    return;
                }
                emit targetsReady(true, result, QString());
            });
    // The walk captures only the path — never `this` — so a probe
    // deleted mid-scan just drops the result with its watcher.
    watcher->setFuture(QtConcurrent::run(
        [root]() { return UploadStateProbe::scanLocalDataRoot(root); }));
}

void UploadStateProbe::startSsh() {
    if (remote_host_.isEmpty() || ssh_user_.isEmpty()) {
        emit targetsReady(false, {},
                          QStringLiteral("No robot host/SSH user configured."));
        return;
    }

    stdout_buf_.clear();
    stderr_buf_.clear();
    last_error_.clear();

    proc_ = new QProcess(this);
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UploadStateProbe::onProcessFinished);
    connect(proc_, &QProcess::errorOccurred,
            this, &UploadStateProbe::onProcessError);

    QStringList args = sshOptions();
    args << QStringLiteral("%1@%2").arg(ssh_user_, remote_host_);
    args << QStringLiteral("bash -lc %1")
                .arg(shellSingleQuote(buildProbeRemoteCommand(data_root_)));

    proc_->setProgram(QStringLiteral("ssh"));
    proc_->setArguments(args);
    proc_->start();
}

void UploadStateProbe::onProcessError(QProcess::ProcessError error) {
    Q_UNUSED(error);
    if (!proc_) return;
    last_error_ = proc_->errorString();
}

void UploadStateProbe::onProcessFinished(int exit_code,
                                         QProcess::ExitStatus status) {
    if (!proc_) {
        return;
    }
    stdout_buf_ = proc_->readAllStandardOutput();
    stderr_buf_ = proc_->readAllStandardError();

    if (status == QProcess::CrashExit || exit_code != 0) {
        QString err = last_error_;
        if (err.isEmpty()) {
            err = QString::fromUtf8(stderr_buf_).trimmed();
        }
        if (err.isEmpty()) {
            err = QStringLiteral("ssh probe exited with code %1").arg(exit_code);
        }
        emit targetsReady(false, {}, err);
        proc_->deleteLater();
        proc_ = nullptr;
        return;
    }

    QList<UploadTarget> out;
    const QString stdout_text = QString::fromUtf8(stdout_buf_);
    const QStringList lines = stdout_text.split(QLatin1Char('\n'),
                                                Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line == QLatin1String("NO_R_DATA")) {
            // /R_DATA missing on robot — empty list, but not a hard
            // error. Operators see "no scans recorded" in the dialog.
            break;
        }
        if (!line.startsWith(QLatin1String("SECTION|"))) {
            continue;
        }
        const QStringList fields = line.split(QLatin1Char('|'));
        if (fields.size() < 8) {
            continue;
        }
        UploadTarget t;
        t.date_folder = fields.at(1);
        t.building_slug = fields.at(2);
        t.section_name = fields.at(3);
        t.status = parseStatus(fields.at(4));
        t.completed_files = fields.at(5).toInt();
        t.total_files = fields.at(6).toInt();
        t.total_bytes = fields.at(7).toLongLong();
        t.run_id = QStringLiteral("%1/%2/%3")
                       .arg(t.date_folder, t.building_slug, t.section_name);
        t.data_path = QStringLiteral("%1/%2")
                          .arg(data_root_, t.run_id);
        if (fields.size() >= 11) {
            t.building_name = fields.at(8);
            t.operator_name = fields.at(9);
            const QString raw_when = fields.at(10);
            QDateTime iso = QDateTime::fromString(raw_when, Qt::ISODate);
            if (!iso.isValid()) {
                iso = QDateTime::fromString(raw_when, Qt::ISODateWithMs);
            }
            t.captured_at = iso.isValid()
                                ? iso
                                : parseFolderStamp(t.date_folder, t.section_name,
                                                   raw_when);
        }
        if (t.building_name.isEmpty()) {
            t.building_name = t.building_slug;
        }
        if (!t.date_folder.isEmpty() && !t.building_slug.isEmpty() &&
            !t.section_name.isEmpty()) {
            out.append(t);
        }
    }

    emit targetsReady(true, out, QString());
    proc_->deleteLater();
    proc_ = nullptr;
}

// ---------------------------------------------------------------------------
// UploadRunner
// ---------------------------------------------------------------------------

UploadRunner::UploadRunner(QObject* parent) : QObject(parent) {}

UploadRunner::~UploadRunner() {
    cancelPendingRetry();
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        proc_->disconnect(this);
        proc_->kill();
        proc_->waitForFinished(500);
    }
}

int UploadRunner::retryBackoffMs(int attempt) {
    // Exponential backoff: 5 s → 15 s → 45 s.  Locked numbers per
    // operator design review.  `attempt` is 1-based; values outside
    // [1, kMaxConnectionRetries] clamp to 5 s so a buggy caller can
    // never accidentally sleep for hours.
    switch (attempt) {
        case 1: return 5000;
        case 2: return 15000;
        case 3: return 45000;
        default: return 5000;
    }
}

void UploadRunner::setSource(UploadSource source) {
    if (busy_) {
        qWarning("[UploadRunner] setSource while busy: ignored. Cancel first.");
        return;
    }
    source_ = source;
}

void UploadRunner::setRemote(const QString& host, const QString& ssh_user) {
    remote_host_ = host.trimmed();
    ssh_user_ = ssh_user.trimmed();
}

void UploadRunner::setLocalScriptPath(const QString& path) {
    local_script_path_ = path.trimmed();
}

QString UploadRunner::resolveLocalScriptPath(const QString& override_path) {
    QStringList candidates;
    if (!override_path.trimmed().isEmpty()) {
        candidates << override_path.trimmed();
    }
    candidates << QString::fromLatin1(kInstalledLocalScript);
    // Dev builds: binary in cpp/build/, script in cpp/scripts/.
    const QString app_dir = QCoreApplication::instance()
                                ? QCoreApplication::applicationDirPath()
                                : QString();
    if (!app_dir.isEmpty()) {
        candidates << QDir(app_dir).filePath(QStringLiteral("../scripts/uploader.py"))
                   << QDir(app_dir).filePath(QStringLiteral("scripts/uploader.py"));
    }
    for (const QString& c : candidates) {
        const QFileInfo fi(c);
        if (fi.isFile() && fi.isReadable()) {
            return fi.canonicalFilePath();
        }
    }
    return QString();
}

void UploadRunner::setCloudAuth(const QString& api_base,
                                const QString& client_id,
                                const QString& device_token) {
    cloud_api_base_ = api_base.trimmed();
    while (cloud_api_base_.endsWith('/')) cloud_api_base_.chop(1);
    cloud_client_id_ = client_id.trimmed();
    cloud_device_token_ = device_token.trimmed();
}

void UploadRunner::setRobotId(const QString& robot_id) {
    robot_id_ = robot_id.trimmed();
}

void UploadRunner::setRemoteScriptPath(const QString& path) {
    remote_script_path_ = path.trimmed();
}

void UploadRunner::setQueue(const QList<UploadTarget>& targets) {
    if (busy_) {
        qWarning("[UploadRunner] setQueue while busy: ignored. Cancel first.");
        return;
    }
    queue_ = targets;
    queue_total_ = queue_.size();
    current_index_ = -1;
}

UploadTarget UploadRunner::activeTarget() const {
    if (current_index_ >= 0 && current_index_ < queue_.size()) {
        return queue_.at(current_index_);
    }
    return UploadTarget{};
}

void UploadRunner::resetQueueState() {
    busy_ = false;
    // An attach still copying belongs to the queue being dropped.
    attach_pending_ = false;
    ++attach_seq_;
    cancel_requested_ = false;
    paused_during_target_ = false;
    pause_reason_.clear();
    fatal_error_.clear();
    stdout_carry_.clear();
    retry_attempts_done_ = 0;
    cancelPendingRetry();
}

void UploadRunner::cancelPendingRetry() {
    retry_pending_ = false;
    if (retry_timer_) {
        retry_timer_->stop();
        retry_timer_->deleteLater();
        retry_timer_ = nullptr;
    }
}

bool UploadRunner::isQueueStoppingError(const QString& error) {
    static const QRegularExpression auth(
        QStringLiteral("Cloud refused \\S+ \\(HTTP 40[13]\\)"));
    return auth.match(error).hasMatch() ||
           error.contains(QLatin1String("env var unset")) ||
           error.contains(QLatin1String("python3-requests is not installed")) ||
           error.contains(QLatin1String("python3 could not be started")) ||
           error.contains(QLatin1String("data_root does not exist"));
}

void UploadRunner::start() {
    if (busy_) {
        return;
    }
    stopped_early_ = false;
    cancelPendingRetry();
    if (queue_.isEmpty()) {
        emit queueFinished(false);
        return;
    }
    if (source_ == UploadSource::RobotSsh) {
        if (remote_host_.isEmpty() || ssh_user_.isEmpty()) {
            emit targetFailed(activeTarget(),
                              QStringLiteral("No robot host/SSH user configured."));
            emit queueFinished(false);
            return;
        }
    } else if (resolveLocalScriptPath(local_script_path_).isEmpty()) {
        emit targetFailed(activeTarget(),
                          QStringLiteral("uploader.py not found on this laptop "
                                         "(expected %1). Reinstall the OCU package.")
                              .arg(QLatin1String(kInstalledLocalScript)));
        emit queueFinished(false);
        return;
    }
    if (cloud_client_id_.isEmpty() || cloud_device_token_.isEmpty()) {
        emit targetFailed(activeTarget(),
                          QStringLiteral("Cloud credentials missing for this robot. "
                                         "Update robots.json with cloud_client_id "
                                         "+ cloud_device_token."));
        emit queueFinished(false);
        return;
    }
    if (cloud_api_base_.isEmpty()) {
        emit targetFailed(activeTarget(),
                          QStringLiteral("cloud_api_base not configured."));
        emit queueFinished(false);
        return;
    }

    busy_ = true;
    cancel_requested_ = false;
    if (current_index_ < 0) {
        current_index_ = 0;
    }
    retry_attempts_done_ = 0;
    launchNext();
}

void UploadRunner::launchNext() {
    if (cancel_requested_) {
        resetQueueState();
        emit queueFinished(true);
        return;
    }
    if (current_index_ >= queue_.size()) {
        resetQueueState();
        emit queueFinished(false);
        return;
    }

    // Fresh target — reset retry counter.  Retries reuse
    // `launchActiveTarget()` directly without bumping current_index_.
    retry_attempts_done_ = 0;
    launchActiveTarget();
}

void UploadRunner::launchActiveTarget() {
    if (current_index_ < 0 || current_index_ >= queue_.size()) {
        return;
    }
    paused_during_target_ = false;
    pause_reason_.clear();
    fatal_error_.clear();
    stdout_carry_.clear();

    // Snapshot first. Clearing the flag before the process is built
    // means a connection retry launches without `--force` and keeps
    // whatever upload_state.json the first launch already wrote.
    const UploadTarget target = queue_.at(current_index_);
    queue_[current_index_].force_reupload = false;
    emit targetStarted(current_index_, queue_total_, target);

    // The script only ever *reads* pause.flag; a flag left by the last
    // Pause would stop this run at its first file boundary.
    clearPauseFlag(target);

    // The laptop's own log of this mission goes up with it. Only before
    // the first full upload: once manifest.json exists, adding a file
    // would just force a re-upload. Off the GUI thread: a long mission's
    // log can be tens of MB on a slow stick.
    if (source_ == UploadSource::ThumbDrive && target.isMission() &&
        !ocu_logs::missionAlreadyUploaded(target.data_path)) {
        const QString mission = target.data_path;
        const QString store = ocuLogStoreDir();
        const int seq = ++attach_seq_;
        const bool first_attempt = retry_attempts_done_ == 0;
        attach_pending_ = true;
        auto* watcher = new QFutureWatcher<ocu_logs::AttachResult>(this);
        connect(watcher, &QFutureWatcher<ocu_logs::AttachResult>::finished, this,
                [this, watcher, seq, first_attempt]() {
                    const ocu_logs::AttachResult r = watcher->result();
                    watcher->deleteLater();
                    if (seq != attach_seq_ || current_index_ < 0 ||
                        current_index_ >= queue_.size()) {
                        return;  // cancelled meanwhile
                    }
                    attach_pending_ = false;
                    if (!r.copied.isEmpty()) {
                        emit logLine(QStringLiteral("OCU logs added: %1")
                                         .arg(r.copied.join(QStringLiteral(", "))));
                    }
                    if (!r.error.isEmpty()) {
                        emit logLine(QStringLiteral(
                                         "OCU logs not added (%1); uploading "
                                         "the scan data without them")
                                         .arg(r.error));
                    }
                    if (!r.missing.isEmpty() && first_attempt) {
                        emit logLine(QStringLiteral(
                                         "OCU log %1 is not on this laptop "
                                         "(run from another OCU?)")
                                         .arg(r.missing.join(QStringLiteral(", "))));
                    }
                    const UploadTarget current = queue_.at(current_index_);
                    if (paused_during_target_) {
                        // Paused while copying: same outcome as a pause
                        // before the first file.
                        emit targetPaused(current,
                                          pause_reason_.isEmpty()
                                              ? QStringLiteral("Paused")
                                              : pause_reason_);
                        busy_ = false;
                        emit queueFinished(false);
                        return;
                    }
                    spawnUploader(current);
                });
        watcher->setFuture(QtConcurrent::run([mission, store]() {
            return ocu_logs::attachToMission(mission, store);
        }));
        return;
    }
    spawnUploader(target);
}

QString UploadRunner::ocuLogStoreDir() const {
    return ocu_log_store_dir_.isEmpty() ? ocu_logs::storeDir()
                                        : ocu_log_store_dir_;
}

void UploadRunner::spawnUploader(const UploadTarget& target) {
    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc_, &QProcess::readyReadStandardOutput,
            this, &UploadRunner::onStdoutReady);
    connect(proc_, &QProcess::readyReadStandardError,
            this, &UploadRunner::onStderrReady);
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UploadRunner::onProcessFinished);
    connect(proc_, &QProcess::errorOccurred,
            this, &UploadRunner::onProcessError);

    if (source_ == UploadSource::ThumbDrive) {
        configureLocalProcess(proc_, target);
    } else {
        configureSshProcess(proc_, target);
    }
    proc_->start();
}

void UploadRunner::configureLocalProcess(QProcess* proc, const UploadTarget& target) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("BDR_CLOUD_API_BASE"), cloud_api_base_);
    env.insert(QStringLiteral("BDR_CLOUD_CLIENT_ID"), cloud_client_id_);
    env.insert(QStringLiteral("BDR_CLOUD_DEVICE_TOKEN"), cloud_device_token_);
    env.insert(QStringLiteral("BDR_UPLOAD_WORKERS"), QStringLiteral("12"));
    // Belt and braces with `-u`: never let a libc buffer hide a line.
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    proc->setProcessEnvironment(env);
    proc->setProgram(QStringLiteral("python3"));
    proc->setArguments(uploaderArguments(
        resolveLocalScriptPath(local_script_path_),
        target.data_path, robot_id_, target.run_id, target.force_reupload));
}

void UploadRunner::configureSshProcess(QProcess* proc, const UploadTarget& target) {
    QStringList args = sshBaseArgs();
    args << QStringLiteral("%1@%2").arg(ssh_user_, remote_host_);
    args << QStringLiteral("bash -lc %1")
                .arg(shellSingleQuote(buildRemoteCommand(target)));
    proc->setProgram(QStringLiteral("ssh"));
    proc->setArguments(args);
}

QStringList UploadRunner::sshBaseArgs() const {
    return sshOptions();
}

QString UploadRunner::buildRemoteCommand(const UploadTarget& target) const {
    const QString script = remote_script_path_.isEmpty()
                               ? defaultRemoteScriptPath(ssh_user_)
                               : remote_script_path_;

    // Compose the env(1) prefix manually so we control quoting; QProcess'
    // default arg-list path won't help us because everything past the
    // ssh user@host arg is a single shell command on the remote side.
    // The leading `rm -f` clears a pause.flag left by the previous
    // Pause, in the same shell so it is ordered before the launch.
    // `--force` comes from the same argument list as the local launch.
    const QString flag = target.data_path + QLatin1Char('/') +
                         QLatin1String(kPauseFile);
    const QStringList args = uploaderArguments(
        script, target.data_path, robot_id_, target.run_id, target.force_reupload);
    QStringList quoted;
    for (const QString& arg : args) quoted << shellSingleQuote(arg);
    return QStringLiteral(
               "rm -f %1; "
               "env "
               "BDR_CLOUD_API_BASE=%2 "
               "BDR_CLOUD_CLIENT_ID=%3 "
               "BDR_CLOUD_DEVICE_TOKEN=%4 "
               "BDR_UPLOAD_WORKERS=12 "
               "python3 %5")
        .arg(shellSingleQuote(flag),
             shellSingleQuote(cloud_api_base_),
             shellSingleQuote(cloud_client_id_),
             shellSingleQuote(cloud_device_token_),
             quoted.join(QLatin1Char(' ')));
}

void UploadRunner::clearPauseFlag(const UploadTarget& target) {
    // RobotSsh clears the flag inside the remote command (see
    // buildRemoteCommand) so it is ordered before the launch.
    if (source_ != UploadSource::ThumbDrive) return;
    QFile::remove(target.data_path + QLatin1Char('/') +
                  QLatin1String(kPauseFile));
}

void UploadRunner::requestPause(const QString& reason) {
    // The caller says why. A pause the script reports itself (a
    // connection error) keeps its own text.
    const QString why = reason.isEmpty() ? QStringLiteral("Paused") : reason;
    if (busy_ && attach_pending_ && current_index_ >= 0 &&
        current_index_ < queue_.size()) {
        // Still copying the OCU logs: stop before the uploader starts.
        paused_during_target_ = true;
        if (pause_reason_.isEmpty()) {
            pause_reason_ = why;
        }
        return;
    }
    if (!busy_ || !proc_ || current_index_ < 0 ||
        current_index_ >= queue_.size()) {
        return;
    }
    const UploadTarget& target = queue_.at(current_index_);
    paused_during_target_ = true;
    if (pause_reason_.isEmpty()) {
        pause_reason_ = why;
    }

    const QString flag = target.data_path + QLatin1Char('/') +
                         QLatin1String(kPauseFile);

    if (source_ == UploadSource::ThumbDrive) {
        // Touch the flag on the stick; the active script polls for it
        // and exits at the next file boundary.
        QFile f(flag);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
            // Stick yanked or read-only: fall back to a SIGTERM. The
            // script's per-file state write keeps the run resumable.
            proc_->terminate();
        }
        return;
    }

    // RobotSsh: a tiny `touch <pause.flag>` in a separate process.
    QStringList args = sshOptions();
    args << QStringLiteral("%1@%2").arg(ssh_user_, remote_host_);
    args << QStringLiteral("touch %1").arg(shellSingleQuote(flag));

    auto* tap = new QProcess(this);
    connect(tap,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            tap, &QProcess::deleteLater);
    tap->setProgram(QStringLiteral("ssh"));
    tap->setArguments(args);
    tap->start();
}

void UploadRunner::requestCancel() {
    cancel_requested_ = true;
    cancelPendingRetry();
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        // SIGTERM first, then forced kill if it lingers. The script's
        // upload_state.json is written after each file, so a hard kill
        // mid-upload still leaves a resumable state on disk.
        proc_->terminate();
        if (!proc_->waitForFinished(2000)) {
            proc_->kill();
            proc_->waitForFinished(500);
        }
    } else {
        // Idle cancel (or cancel-while-retry-armed) — just clear the
        // queue tail and emit done.
        const UploadTarget current = (current_index_ >= 0 &&
                                      current_index_ < queue_.size())
                                         ? queue_.at(current_index_)
                                         : UploadTarget{};
        queue_.clear();
        current_index_ = -1;
        resetQueueState();
        if (!current.run_id.isEmpty()) {
            emit targetPaused(current,
                              QStringLiteral("Cancelled by operator"));
        }
        emit queueFinished(true);
    }
}

void UploadRunner::onStdoutReady() {
    if (!proc_) return;
    stdout_carry_ += QString::fromUtf8(proc_->readAllStandardOutput());
    while (true) {
        const int nl = stdout_carry_.indexOf(QLatin1Char('\n'));
        if (nl < 0) break;
        const QString line = stdout_carry_.left(nl).trimmed();
        stdout_carry_.remove(0, nl + 1);
        if (!line.isEmpty()) {
            processStdoutLine(line);
        }
    }
}

void UploadRunner::onStderrReady() {
    if (!proc_) return;
    const QString err = QString::fromUtf8(proc_->readAllStandardError())
                            .trimmed();
    if (!err.isEmpty()) {
        // python3 (and, over SSH, the ssh client's) stderr is mostly
        // noise but useful in the log strip when a real error fires.
        emit logLine(QStringLiteral("[stderr] ") + err);
    }
}

void UploadRunner::processStdoutLine(const QString& line) {
    if (current_index_ < 0 || current_index_ >= queue_.size()) {
        return;
    }
    const UploadTarget& target = queue_.at(current_index_);

    static const QChar kCheck = QChar(0x2713);  // matches uploader.py "✓"
    if (line.startsWith(kCheck)) {
        // "✓ Uploaded: <relpath>"
        const int colon = line.indexOf(QLatin1Char(':'));
        const QString relpath =
            (colon >= 0) ? line.mid(colon + 1).trimmed() : line.mid(1).trimmed();
        emit fileUploaded(target, relpath);
        return;
    }
    if (line.startsWith(QLatin1String("Skipping already uploaded:"))) {
        const QString relpath = line.section(QLatin1Char(':'), 1).trimmed();
        emit fileSkipped(target, relpath);
        return;
    }
    if (line.startsWith(QLatin1String("Connection error:"))) {
        pause_reason_ = line;
        paused_during_target_ = true;
        emit logLine(line);
        return;
    }
    if (line.startsWith(QLatin1String("Auto-pausing"))) {
        if (pause_reason_.isEmpty()) {
            pause_reason_ = line;
        }
        paused_during_target_ = true;
        emit logLine(line);
        return;
    }
    if (line.startsWith(QLatin1String("Manual pause detected"))) {
        paused_during_target_ = true;
        if (pause_reason_.isEmpty()) {
            pause_reason_ = QStringLiteral("Manual pause");
        }
        emit logLine(line);
        return;
    }
    if (line.startsWith(QLatin1String("Already uploaded:"))) {
        // No-op short-circuit on a `Done` re-run — surface as log line.
        emit logLine(line);
        return;
    }
    if (line.startsWith(QLatin1String("Unexpected error:"))) {
        fatal_error_ = line;
        emit logLine(line);
        return;
    }
    // "Resuming upload.", "Uploading N files...", "Generating manifest...",
    // "Upload complete: ...", "State cleaned up."
    emit logLine(line);
}

void UploadRunner::onProcessError(QProcess::ProcessError error) {
    if (!proc_) return;
    fatal_error_ = proc_->errorString();
    if (error == QProcess::FailedToStart) {
        // No `finished` follows a failed start; route through the same
        // completion path so the queue never sticks in busy_.
        if (source_ == UploadSource::ThumbDrive) {
            fatal_error_ = QStringLiteral("python3 could not be started (%1). "
                                          "Install python3 + python3-requests.")
                               .arg(proc_->errorString());
        }
        QTimer::singleShot(0, this, [this]() {
            onProcessFinished(-1, QProcess::NormalExit);
        });
    }
}

void UploadRunner::onProcessFinished(int exit_code,
                                     QProcess::ExitStatus status) {
    if (current_index_ < 0 || current_index_ >= queue_.size()) {
        if (proc_) {
            proc_->deleteLater();
            proc_ = nullptr;
        }
        resetQueueState();
        emit queueFinished(cancel_requested_);
        return;
    }

    const UploadTarget current = queue_.at(current_index_);
    if (proc_) {
        // Drain any tail of stdout that didn't end in newline before
        // reporting per-target outcome.
        if (!stdout_carry_.isEmpty()) {
            const QString tail = stdout_carry_.trimmed();
            stdout_carry_.clear();
            if (!tail.isEmpty()) {
                processStdoutLine(tail);
            }
        }
        proc_->deleteLater();
        proc_ = nullptr;
    }

    if (cancel_requested_) {
        // Operator hit Cancel — emit pause for the in-flight target so
        // the row reflects "stopped (resumable)" rather than failed.
        emit targetPaused(current,
                          QStringLiteral("Cancelled by operator"));
        queue_.clear();
        current_index_ = -1;
        resetQueueState();
        emit queueFinished(true);
        return;
    }

    if (status == QProcess::CrashExit) {
        emit targetFailed(current,
                          QStringLiteral("uploader crashed (signalled exit)"));
        resetQueueState();
        queue_.clear();
        current_index_ = -1;
        emit queueFinished(false);
        return;
    }

    if (paused_during_target_) {
        // Decide between auto-retry (transient cloud connection error)
        // and operator-visible pause.  Manual pauses never auto-retry
        // — that would defeat the operator's intent.
        const bool is_transient_conn_err =
            pause_reason_.contains(QStringLiteral("Connection error"),
                                   Qt::CaseInsensitive);
        const bool can_retry =
            is_transient_conn_err &&
            retry_enabled_ &&
            retry_attempts_done_ < kMaxConnectionRetries;

        if (can_retry) {
            const int next_attempt = retry_attempts_done_ + 1;
            const int wait_ms = retryBackoffMs(next_attempt);
            retry_pending_ = true;
            cancelPendingRetry();  // also resets retry_pending_ — restore
            retry_pending_ = true;
            retry_timer_ = new QTimer(this);
            retry_timer_->setSingleShot(true);
            retry_timer_->setInterval(wait_ms);
            const UploadTarget snapshot = current;  // copy in case queue mutates
            connect(retry_timer_, &QTimer::timeout, this,
                    [this]() {
                        retry_pending_ = false;
                        if (retry_timer_) {
                            retry_timer_->deleteLater();
                            retry_timer_ = nullptr;
                        }
                        if (cancel_requested_ ||
                            current_index_ < 0 ||
                            current_index_ >= queue_.size()) {
                            return;
                        }
                        retry_attempts_done_ += 1;
                        launchActiveTarget();
                    });
            emit targetRetryScheduled(snapshot, next_attempt, wait_ms,
                                      pause_reason_);
            // A slot may cancel during the signal (quit during a retry
            // wait). cancelPendingRetry() has already dropped the timer.
            if (retry_pending_ && retry_timer_) retry_timer_->start();
            // Stay busy_ — we are still owning the active target.
            return;
        }

        // Either non-transient pause (operator) OR retry budget
        // exhausted.  In the exhausted-budget case we promote to a
        // hard failure so the dialog modal fires.
        if (is_transient_conn_err) {
            const QString final_msg =
                QStringLiteral("Connection error persisted after %1 retries: %2")
                    .arg(kMaxConnectionRetries)
                    .arg(pause_reason_);
            emit targetFailed(current, final_msg);
            queue_.clear();
            current_index_ = -1;
            resetQueueState();
            emit queueFinished(false);
            return;
        }

        emit targetPaused(current,
                          pause_reason_.isEmpty()
                              ? QStringLiteral("Paused")
                              : pause_reason_);
        // Keep the queue intact so the operator can resume by pressing
        // Upload again — the target stays at `current_index_`.
        busy_ = false;
        emit queueFinished(false);
        return;
    }

    if (!fatal_error_.isEmpty() || exit_code != 0) {
        const QString error =
            fatal_error_.isEmpty()
                ? QStringLiteral("uploader exited with code %1").arg(exit_code)
                : fatal_error_;
        emit targetFailed(current, error);
        if (isQueueStoppingError(error)) {
            // Every other folder would fail the same way (credentials,
            // a missing interpreter, the stick gone): stop here.
            stopped_early_ = current_index_ + 1 < queue_.size();
            resetQueueState();
            queue_.clear();
            current_index_ = -1;
            emit queueFinished(false);
            return;
        }
        // One bad folder (a damaged stick copy, a file over 5 GB) must
        // not hold back the rest of the selection. The dialog lists the
        // failures when the queue ends.
        current_index_ += 1;
        launchNext();
        return;
    }

    if (source_ == UploadSource::ThumbDrive && current.isMission()) {
        // Uploaded logs are the first to go when the laptop runs short.
        ocu_logs::markUploaded(current.data_path, ocuLogStoreDir());
    }
    emit targetCompleted(current);
    current_index_ += 1;
    launchNext();
}

}  // namespace f2c_cpp
