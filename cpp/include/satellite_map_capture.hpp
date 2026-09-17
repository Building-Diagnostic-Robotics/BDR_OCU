/**
 * @file satellite_map_capture.hpp
 * @brief Robot map collection for the Stage 6 alignment flow.
 *
 * Drives `robot_map_collection.launch.py` over SSH (arm, 360° spin, GPS
 * baseline, save map + pose), pulls the resulting PCD and `*_final_pose.yaml`
 * back to the laptop, re-origins the cloud on the robot's final pose, and
 * rasterises it top-down into a QImage the operator can pick points on.
 *
 * The (image, bounds_m) pair IS the scale bookkeeping — there is no separate
 * metres-per-pixel member. Use pcdImageToWorld / worldToPcdImage to convert.
 */

#pragma once

#include "satellite_job_model.hpp"

#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>

class QProcess;
class QTimer;

namespace f2c_cpp {

/** Top-down raster of the collected map, plus everything derived from it. */
struct MapCapture {
    QImage image;        // ARGB32, grey with hit-count alpha; row 0 = max northing
    QRectF bounds_m;     // world extent in robot_init metres
    GpsFix gps;          // seed fix written by map_collection_node
    QString pcd_path;    // local re-origined .pcd
    QString pose_path;   // local *_final_pose.yaml
    QString label;
    QString error;

    bool valid() const { return error.isEmpty() && !image.isNull(); }
};

/** Image pixel -> robot_init metres. Row 0 is max northing, hence the flip. */
QPointF pcdImageToWorld(const QRectF& bounds_m, const QSize& image_size,
                        const QPointF& px);
/** Inverse of pcdImageToWorld. */
QPointF worldToPcdImage(const QRectF& bounds_m, const QSize& image_size,
                        const QPointF& world_m);

/** `key: value` lines -> map. Tolerates comments and blank lines. */
QMap<QString, QString> parseSimpleYaml(const QString& text);

/**
 * Renders a cloud top-down: flat grey, alpha from log-scaled hit count
 * between the 2nd and 98th percentile so a dense roof does not wash out the
 * sparse returns at its edges. Writes the world extent to `bounds_out`.
 */
QImage renderTopDownAlphaDensity(const QString& pcd_path, QRectF* bounds_out,
                                 QString* error);

/**
 * Recolors a raster from `renderTopDownAlphaDensity` for a light canvas.
 *
 * The renderer draws every point in one grey and puts all the density
 * information in the alpha channel, which is what makes this cheap: only the
 * RGB triplet changes, so the density shading survives untouched. Light grey
 * points are near-invisible on a white drafting surface, and this is the
 * raster the operator picks alignment correspondences on, so legibility here
 * is not cosmetic. Re-rasterising instead would mean re-running PCL over
 * millions of points; this is a single pass over ~1 MP and only runs on a
 * theme flip.
 */
QImage tintDensityRasterForLightCanvas(const QImage& raster);

/**
 * Async driver for one map-collection run. Stages: SSH launch + manifest
 * poll -> scp PCD -> scp pose -> off-thread re-origin + raster.
 *
 * The SSH stage does NOT wait for `ros2 launch` to exit — Fast-LIO, Livox and
 * the ODrive nodes routinely ignore the shutdown request, so the remote
 * script polls for a fresh manifest and then SIGINTs the launch tree itself.
 */
class MapCaptureRunner : public QObject {
    Q_OBJECT

public:
    explicit MapCaptureRunner(QObject* parent = nullptr);
    ~MapCaptureRunner() override;

    enum class Stage {
        Idle,
        Collecting,
        DownloadingPcd,
        DownloadingPose,
        Rendering,
    };

    /** `local_dir` receives the downloaded pcd/pose and the re-origined pcd. */
    bool start(const QString& host, const QString& ssh_user,
               const QString& local_dir, QString* error = nullptr);
    void cancel();
    bool busy() const { return stage_ != Stage::Idle; }
    Stage stage() const { return stage_; }

signals:
    void progress(const QString& message);
    void finished(const MapCapture& capture);

private:
    void runCommand(const QString& program, const QStringList& args,
                    int timeout_ms);
    void onProcessFinished(int exit_code);
    void onTimeout();
    void startDownloadPcd();
    void startDownloadPose();
    void startRendering();
    void onRenderFinished();
    void fail(const QString& message);
    QStringList sshBaseArgs() const;

    // The spin takes ~40 s but the stack boot in front of it is the variable
    // part; 4 min covers a cold Livox/Fast-LIO start without hanging forever.
    static constexpr int kCollectTimeoutMs = 240000;
    static constexpr int kScpPcdTimeoutMs = 180000;
    static constexpr int kScpPoseTimeoutMs = 30000;

    QProcess* proc_ = nullptr;
    QTimer* timeout_ = nullptr;
    /** Re-origin + raster run here: a roof cloud is millions of points and
        PCL would otherwise hold the GUI thread for seconds. */
    QFutureWatcher<MapCapture>* render_watcher_ = nullptr;
    Stage stage_ = Stage::Idle;
    bool cancelling_ = false;

    QString host_;
    QString ssh_user_;
    QString local_dir_;
    QString remote_pcd_;
    QString remote_pose_;
    QString local_pcd_;
    QString local_pose_;
    QString origin_pcd_;
    GpsFix manifest_gps_;
};

}  // namespace f2c_cpp
