#include "satellite_map_capture.hpp"

#include "coverage_pipeline.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>

#include <pcl/common/transforms.h>
#include <pcl/io/pcd_io.h>

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace f2c_cpp {

namespace {

// The raster is aspect-preserving; the longer world axis gets the max
// dimension, so effective resolution is max(range_x, range_y) / 4096 m/px.
constexpr int kProjectionMaxDim = 4096;
constexpr int kProjectionMinDim = 64;

// Everything a ground robot can see on a roof lives well inside this band;
// clipping it keeps stray sky/ground returns out of the density stretch.
constexpr double kTopDownZMin = -10.0;
constexpr double kTopDownZMax = 10.0;

constexpr const char* kManifestBegin = "__MANIFEST_BEGIN__";
constexpr const char* kManifestEnd = "__MANIFEST_END__";

QString shellQuote(const QString& value) {
    QString escaped = value;
    escaped.replace('\'', QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

GpsFix gpsFromYaml(const QMap<QString, QString>& yaml) {
    GpsFix fix;
    const QString valid = yaml.value(QStringLiteral("gps_valid")).toLower();
    if (valid != QLatin1String("true") && valid != QLatin1String("1")) {
        return fix;
    }
    bool lat_ok = false;
    bool lon_ok = false;
    const double lat = yaml.value(QStringLiteral("lat")).toDouble(&lat_ok);
    const double lon = yaml.value(QStringLiteral("lon")).toDouble(&lon_ok);
    if (!lat_ok || !lon_ok) {
        return fix;
    }
    fix.valid = true;
    fix.lat = lat;
    fix.lon = lon;
    fix.alt_m = yaml.value(QStringLiteral("alt_m")).toDouble();
    // heading_deg is null when the forward/back baseline was too short to
    // resolve a bearing — a position-only seed is still useful.
    const QString heading = yaml.value(QStringLiteral("heading_deg"));
    bool heading_ok = false;
    const double heading_value = heading.toDouble(&heading_ok);
    if (heading_ok && heading.compare(QLatin1String("null"),
                                      Qt::CaseInsensitive) != 0) {
        fix.heading_valid = true;
        fix.heading_deg = heading_value;
    }
    fix.fix_type = yaml.value(QStringLiteral("fix_type"));
    fix.hacc_m = yaml.value(QStringLiteral("hacc_m")).toDouble();
    fix.num_sats = yaml.value(QStringLiteral("num_sats")).toInt();
    fix.utc = QDateTime::fromString(yaml.value(QStringLiteral("utc")),
                                    Qt::ISODate);
    fix.source = yaml.value(QStringLiteral("gps_source"));
    return fix;
}

bool parsePoseYaml(const QString& path, Eigen::Isometry3d* T_map_final,
                   GpsFix* gps, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Could not read pose file:\n") + path;
        }
        return false;
    }
    const QMap<QString, QString> yaml =
        parseSimpleYaml(QString::fromUtf8(file.readAll()));
    const double x = yaml.value(QStringLiteral("x")).toDouble();
    const double y = yaml.value(QStringLiteral("y")).toDouble();
    const double z = yaml.value(QStringLiteral("z")).toDouble();
    Eigen::Quaterniond q(yaml.value(QStringLiteral("qw")).toDouble(),
                         yaml.value(QStringLiteral("qx")).toDouble(),
                         yaml.value(QStringLiteral("qy")).toDouble(),
                         yaml.value(QStringLiteral("qz")).toDouble());
    if (q.norm() < 1e-9) {
        q = Eigen::Quaterniond(Eigen::AngleAxisd(
            yaml.value(QStringLiteral("yaw")).toDouble(),
            Eigen::Vector3d::UnitZ()));
    } else {
        q.normalize();
    }
    T_map_final->setIdentity();
    T_map_final->translation() = Eigen::Vector3d(x, y, z);
    T_map_final->linear() = q.toRotationMatrix();
    if (gps) {
        *gps = gpsFromYaml(yaml);
    }
    return true;
}

}  // namespace

QMap<QString, QString> parseSimpleYaml(const QString& text) {
    QMap<QString, QString> out;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            continue;
        }
        out.insert(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
    }
    return out;
}

QPointF pcdImageToWorld(const QRectF& bounds_m, const QSize& image_size,
                        const QPointF& px) {
    const double u = px.x() / std::max(1, image_size.width() - 1);
    const double v = px.y() / std::max(1, image_size.height() - 1);
    return QPointF(bounds_m.left() + u * bounds_m.width(),
                   bounds_m.bottom() - v * bounds_m.height());
}

QPointF worldToPcdImage(const QRectF& bounds_m, const QSize& image_size,
                        const QPointF& world_m) {
    const double width = std::max(1e-9, bounds_m.width());
    const double height = std::max(1e-9, bounds_m.height());
    const double u = (world_m.x() - bounds_m.left()) / width;
    const double v = (bounds_m.bottom() - world_m.y()) / height;
    return QPointF(u * std::max(1, image_size.width() - 1),
                   v * std::max(1, image_size.height() - 1));
}

QImage renderTopDownAlphaDensity(const QString& pcd_path, QRectF* bounds_out,
                                 QString* error) {
    QImage image;
    PointCloudPtr cloud = loadPointCloudFile(pcd_path.toStdString());
    if (!cloud || cloud->empty()) {
        if (error) {
            *error = QStringLiteral("Point cloud is empty or unreadable:\n") +
                     pcd_path;
        }
        return image;
    }
    cloud = filterByZRange(cloud, kTopDownZMin, kTopDownZMax);
    if (!cloud || cloud->empty()) {
        if (error) {
            *error = QStringLiteral("No points survived the Z crop.");
        }
        return image;
    }

    double min_x = std::numeric_limits<double>::max();
    double min_y = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double max_y = std::numeric_limits<double>::lowest();
    for (const auto& pt : cloud->points) {
        if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
            continue;
        }
        min_x = std::min(min_x, double(pt.x));
        max_x = std::max(max_x, double(pt.x));
        min_y = std::min(min_y, double(pt.y));
        max_y = std::max(max_y, double(pt.y));
    }
    if (min_x > max_x || min_y > max_y) {
        if (error) {
            *error = QStringLiteral("Point cloud has no finite XY extent.");
        }
        return image;
    }
    if (std::abs(max_x - min_x) < 1e-6) {
        min_x -= 0.5;
        max_x += 0.5;
    }
    if (std::abs(max_y - min_y) < 1e-6) {
        min_y -= 0.5;
        max_y += 0.5;
    }

    const double range_x = max_x - min_x;
    const double range_y = max_y - min_y;
    const double max_range = std::max(range_x, range_y);
    const int width =
        std::clamp(int(std::ceil((range_x / max_range) * kProjectionMaxDim)),
                   kProjectionMinDim, kProjectionMaxDim);
    const int height =
        std::clamp(int(std::ceil((range_y / max_range) * kProjectionMaxDim)),
                   kProjectionMinDim, kProjectionMaxDim);

    image = QImage(width, height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    std::vector<unsigned int> pixel_counts(size_t(width) * size_t(height), 0u);
    unsigned int max_count = 0u;
    for (const auto& pt : cloud->points) {
        if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
            continue;
        }
        const int u = std::clamp(
            int(std::llround(((double(pt.x) - min_x) / range_x) * (width - 1))),
            0, width - 1);
        // Row 0 is max northing so the raster reads like a map, not like an
        // ENU plot. pcdImageToWorld undoes this.
        const int v = std::clamp(
            int(std::llround(((max_y - double(pt.y)) / range_y) * (height - 1))),
            0, height - 1);
        max_count =
            std::max(max_count, ++pixel_counts[size_t(v) * size_t(width) + size_t(u)]);
    }
    if (max_count == 0u) {
        if (error) {
            *error = QStringLiteral("Point cloud rasterised to nothing.");
        }
        return QImage();
    }

    std::vector<unsigned int> occupied;
    occupied.reserve(pixel_counts.size() / 8);
    for (unsigned int count : pixel_counts) {
        if (count > 0u) {
            occupied.push_back(count);
        }
    }
    auto percentileCount = [&occupied](double percentile) -> unsigned int {
        if (occupied.empty()) {
            return 1u;
        }
        const size_t idx = std::min(
            occupied.size() - 1,
            size_t(std::floor(percentile * double(occupied.size() - 1))));
        std::nth_element(occupied.begin(),
                         occupied.begin() + std::ptrdiff_t(idx),
                         occupied.end());
        return std::max(1u, occupied[idx]);
    };

    const unsigned int low_count = percentileCount(0.02);
    const unsigned int high_count = std::max(low_count, percentileCount(0.98));
    const double log_low = std::log1p(double(low_count - 1u));
    const double log_high = std::log1p(double(high_count - 1u));
    const double log_range = std::max(1e-6, log_high - log_low);
    const bool has_contrast = high_count > low_count;
    constexpr int kSingleHitAlpha = 120;
    constexpr int kFlatAlpha = 170;
    constexpr int kMaxHitAlpha = 235;
    constexpr int kPointGray = 210;

    for (int v = 0; v < height; ++v) {
        QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(v));
        for (int u = 0; u < width; ++u) {
            const unsigned int count =
                pixel_counts[size_t(v) * size_t(width) + size_t(u)];
            if (count == 0u) {
                continue;
            }
            int alpha = kFlatAlpha;
            if (has_contrast) {
                const double t =
                    std::clamp((std::log1p(double(count - 1u)) - log_low) /
                                   log_range,
                               0.0, 1.0);
                alpha = kSingleHitAlpha +
                        int(std::round(t * double(kMaxHitAlpha - kSingleHitAlpha)));
            }
            row[u] = qRgba(kPointGray, kPointGray, kPointGray,
                           std::clamp(alpha, kSingleHitAlpha, kMaxHitAlpha));
        }
    }

    if (bounds_out) {
        *bounds_out =
            QRectF(QPointF(min_x, min_y), QPointF(max_x, max_y)).normalized();
    }
    return image;
}

// ---- Runner -----------------------------------------------------------------

MapCaptureRunner::MapCaptureRunner(QObject* parent) : QObject(parent) {
    proc_ = new QProcess(this);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    timeout_ = new QTimer(this);
    timeout_->setSingleShot(true);
    connect(timeout_, &QTimer::timeout, this, &MapCaptureRunner::onTimeout);
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                onProcessFinished(code);
            });
}

MapCaptureRunner::~MapCaptureRunner() {
    if (proc_->state() != QProcess::NotRunning) {
        proc_->kill();
        proc_->waitForFinished(2000);
    }
}

QStringList MapCaptureRunner::sshBaseArgs() const {
    return QStringList() << "-o" << "ConnectTimeout=10"
                         << "-o" << "StrictHostKeyChecking=no"
                         << "-o" << "UserKnownHostsFile=/dev/null"
                         << "-o" << "BatchMode=yes"
                         << QStringLiteral("%1@%2").arg(ssh_user_, host_);
}

bool MapCaptureRunner::start(const QString& host, const QString& ssh_user,
                             const QString& local_dir, QString* error) {
    if (busy()) {
        if (error) *error = QStringLiteral("A map collection is already running.");
        return false;
    }
    if (host.isEmpty()) {
        if (error) *error = QStringLiteral("No robot host resolved.");
        return false;
    }
    if (!QDir().mkpath(local_dir)) {
        if (error) {
            *error = QStringLiteral("Could not create %1").arg(local_dir);
        }
        return false;
    }
    host_ = host;
    ssh_user_ = ssh_user;
    local_dir_ = local_dir;
    remote_pcd_.clear();
    remote_pose_.clear();
    manifest_gps_ = GpsFix{};
    cancelling_ = false;
    stage_ = Stage::Collecting;

    // `ros2 launch` frequently never returns here: Fast-LIO, the Livox driver
    // and the ODrive nodes all ignore the shutdown request. So the remote
    // script backgrounds the launch, polls for a manifest NEWER than the one
    // already on disk, prints it, then SIGINTs the tree itself.
    const QString manifest =
        QStringLiteral("/R_DATA/raw_maps/map_collection_latest.yaml");
    const QString script = QStringLiteral(
        "set -u; "
        "if [ -f /opt/ros/humble/setup.bash ]; then . /opt/ros/humble/setup.bash; fi; "
        "if [ -f \"$HOME/pilot_ws/install/setup.bash\" ]; then . \"$HOME/pilot_ws/install/setup.bash\"; fi; "
        "M=%1; "
        "OLD=$(stat -c %%Y \"$M\" 2>/dev/null || echo 0); "
        "ros2 launch pilot_control robot_map_collection.launch.py "
        "> /tmp/ocu_map_collection.log 2>&1 & LP=$!; "
        "for i in $(seq 1 120); do "
        "  sleep 2; "
        "  NEW=$(stat -c %%Y \"$M\" 2>/dev/null || echo 0); "
        "  if [ \"$NEW\" -gt \"$OLD\" ] && grep -qE '^status: (done|failed)' \"$M\"; then "
        "    echo %2; cat \"$M\"; echo %3; break; "
        "  fi; "
        "done; "
        "kill -INT $LP >/dev/null 2>&1 || true; "
        "disown $LP >/dev/null 2>&1 || true; "
        "exit 0")
                              .arg(manifest,
                                   QLatin1String(kManifestBegin),
                                   QLatin1String(kManifestEnd));

    QStringList args = sshBaseArgs();
    args << QStringLiteral("bash -lc %1").arg(shellQuote(script));
    emit progress(QStringLiteral(
        "Collecting map on the robot (arm, 360° spin, GPS baseline)…"));
    runCommand(QStringLiteral("ssh"), args, kCollectTimeoutMs);
    return true;
}

void MapCaptureRunner::runCommand(const QString& program,
                                  const QStringList& args, int timeout_ms) {
    proc_->start(program, args);
    timeout_->start(timeout_ms);
}

void MapCaptureRunner::cancel() {
    if (!busy() || cancelling_) {
        return;
    }
    cancelling_ = true;
    timeout_->stop();
    if (proc_->state() != QProcess::NotRunning) {
        proc_->kill();
    }
    stage_ = Stage::Idle;
    MapCapture capture;
    capture.error = QStringLiteral("Map collection cancelled.");
    emit finished(capture);
}

void MapCaptureRunner::fail(const QString& message) {
    timeout_->stop();
    stage_ = Stage::Idle;
    MapCapture capture;
    capture.error = message;
    emit finished(capture);
}

void MapCaptureRunner::onTimeout() {
    // Rendering runs locally and owns no process; its "timeout" would just
    // abort a healthy long parse of a big cloud.
    if (stage_ == Stage::Rendering || stage_ == Stage::Idle) {
        return;
    }
    if (proc_->state() != QProcess::NotRunning) {
        proc_->kill();
    }
    fail(QStringLiteral("Timed out waiting for the robot during map "
                        "collection. Check the link and try again."));
}

void MapCaptureRunner::onProcessFinished(int exit_code) {
    if (cancelling_ || stage_ == Stage::Idle) {
        return;
    }
    timeout_->stop();
    const QString output = QString::fromUtf8(proc_->readAll());

    switch (stage_) {
        case Stage::Collecting: {
            const int begin = output.indexOf(QLatin1String(kManifestBegin));
            const int end = output.indexOf(QLatin1String(kManifestEnd));
            if (begin < 0 || end <= begin) {
                fail(QStringLiteral(
                         "The robot never produced a map manifest (rc=%1). "
                         "Check /tmp/ocu_map_collection.log on the robot.")
                         .arg(exit_code));
                return;
            }
            const QString body =
                output.mid(begin + int(qstrlen(kManifestBegin)),
                           end - begin - int(qstrlen(kManifestBegin)));
            const QMap<QString, QString> yaml = parseSimpleYaml(body);
            if (yaml.value(QStringLiteral("status")) !=
                QLatin1String("done")) {
                fail(QStringLiteral("Map collection failed on the robot: %1")
                         .arg(yaml.value(QStringLiteral("message"),
                                         QStringLiteral("unknown reason"))));
                return;
            }
            remote_pcd_ = yaml.value(QStringLiteral("pcd"));
            remote_pose_ = yaml.value(QStringLiteral("pose"));
            manifest_gps_ = gpsFromYaml(yaml);
            if (remote_pcd_.isEmpty() || remote_pose_.isEmpty()) {
                fail(QStringLiteral(
                    "Map manifest is missing the pcd/pose paths."));
                return;
            }
            const QString base = QFileInfo(remote_pcd_).completeBaseName();
            local_pcd_ = local_dir_ + QLatin1Char('/') + base +
                         QStringLiteral(".pcd");
            local_pose_ = local_dir_ + QLatin1Char('/') + base +
                          QStringLiteral("_final_pose.yaml");
            origin_pcd_ = local_dir_ + QLatin1Char('/') + base +
                          QStringLiteral("_origin.pcd");
            startDownloadPcd();
            return;
        }
        case Stage::DownloadingPcd:
            if (exit_code != 0) {
                fail(QStringLiteral("Failed to download the map: %1")
                         .arg(output.trimmed()));
                return;
            }
            startDownloadPose();
            return;
        case Stage::DownloadingPose:
            if (exit_code != 0) {
                fail(QStringLiteral("Failed to download the final pose: %1")
                         .arg(output.trimmed()));
                return;
            }
            startRendering();
            return;
        default:
            return;
    }
}

void MapCaptureRunner::startDownloadPcd() {
    stage_ = Stage::DownloadingPcd;
    emit progress(QStringLiteral("Downloading the map…"));
    QStringList args;
    args << "-o" << "ConnectTimeout=10"
         << "-o" << "StrictHostKeyChecking=no"
         << "-o" << "UserKnownHostsFile=/dev/null"
         << "-o" << "BatchMode=yes"
         << QStringLiteral("%1@%2:%3").arg(ssh_user_, host_, remote_pcd_)
         << local_pcd_;
    runCommand(QStringLiteral("scp"), args, kScpPcdTimeoutMs);
}

void MapCaptureRunner::startDownloadPose() {
    stage_ = Stage::DownloadingPose;
    emit progress(QStringLiteral("Downloading the final pose…"));
    QStringList args;
    args << "-o" << "ConnectTimeout=10"
         << "-o" << "StrictHostKeyChecking=no"
         << "-o" << "UserKnownHostsFile=/dev/null"
         << "-o" << "BatchMode=yes"
         << QStringLiteral("%1@%2:%3").arg(ssh_user_, host_, remote_pose_)
         << local_pose_;
    runCommand(QStringLiteral("scp"), args, kScpPoseTimeoutMs);
}

void MapCaptureRunner::startRendering() {
    stage_ = Stage::Rendering;
    emit progress(QStringLiteral("Re-origining and rendering the map…"));

    MapCapture capture;
    capture.pcd_path = origin_pcd_;
    capture.pose_path = local_pose_;
    capture.gps = manifest_gps_;

    Eigen::Isometry3d T_map_final = Eigen::Isometry3d::Identity();
    GpsFix pose_gps;
    QString error;
    if (!parsePoseYaml(local_pose_, &T_map_final, &pose_gps, &error)) {
        fail(error);
        return;
    }
    if (pose_gps.valid) {
        capture.gps = pose_gps;
    }

    // Everything downstream — correspondences, the ROI export, the director's
    // roi_vertices — is expressed in robot_init, so the cloud is re-origined
    // on the robot's final pose before anything looks at it.
    PointCloudPtr raw = loadPointCloudFile(local_pcd_.toStdString());
    if (!raw || raw->empty()) {
        fail(QStringLiteral("Downloaded map is empty:\n") + local_pcd_);
        return;
    }
    PointCloudPtr origined(new PointCloud);
    pcl::transformPointCloud(*raw, *origined,
                             T_map_final.inverse().matrix().cast<float>());
    if (pcl::io::savePCDFileBinary(origin_pcd_.toStdString(), *origined) < 0) {
        fail(QStringLiteral("Could not write the re-origined map:\n") +
             origin_pcd_);
        return;
    }

    capture.image =
        renderTopDownAlphaDensity(origin_pcd_, &capture.bounds_m, &error);
    if (capture.image.isNull()) {
        fail(error.isEmpty() ? QStringLiteral("Could not render the map.")
                             : error);
        return;
    }
    capture.label = QFileInfo(origin_pcd_).fileName();

    stage_ = Stage::Idle;
    emit finished(capture);
}

}  // namespace f2c_cpp
