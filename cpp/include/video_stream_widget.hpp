#pragma once

#include <atomic>

#include <QString>
#include <QWidget>
#include <QtGlobal>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

class QHideEvent;
class QResizeEvent;
class QShowEvent;
class QTimer;

namespace f2c_cpp {

class VideoStreamWidget : public QWidget {
    Q_OBJECT

public:
    explicit VideoStreamWidget(QWidget* parent = nullptr);
    ~VideoStreamWidget();

    void startStream(int port);
    void stopStream();
    bool isPlaying() const { return playing_; }
    int currentPort() const { return current_port_; }

    // Wall-clock ms (QDateTime::currentMSecsSinceEpoch) of the most
    // recently delivered RTP buffer.  0 if no frames yet, or the
    // pipeline isn't running.  Used by AppShellWindow's slow tick to
    // stamp LinkHealthMonitor::Source::FpvFrame and detect "video
    // froze but ROS is still healthy" (the inverse of the original
    // disconnect bug, where ROS dies but FPV stays live).
    qint64 lastFrameWallMs() const { return last_frame_wall_ms_.load(); }

signals:
    void streamError(const QString& msg);
    void firstFrameReady();
    void streamStarted();
    void streamStopped();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void pollBus();
    void handleFirstFrameReady(qulonglong generation);

private:
    GstElement* pipeline_ = nullptr;
    int current_port_ = 5600;
    bool playing_ = false;
    bool auto_start_on_show_ = false;
    QTimer* bus_poll_timer_ = nullptr;
    std::atomic_ullong stream_generation_{0};
    bool first_frame_emitted_ = false;
    // Updated from the gstreamer streaming thread by frameStampProbe.
    // atomic so AppShellWindow's Qt main thread can read it lockfree.
    // Reset to 0 in destroyPipeline() / startStream() so a fresh
    // pipeline doesn't inherit the previous run's "last frame".
    std::atomic<qint64> last_frame_wall_ms_{0};

    void setupPipeline(int port);
    void destroyPipeline();
    static GstBusSyncReply busSyncHandler(GstBus* bus, GstMessage* msg, gpointer data);
    static gboolean busCallback(GstBus* bus, GstMessage* msg, gpointer data);
    static GstPadProbeReturn firstFrameProbe(GstPad* pad, GstPadProbeInfo* info, gpointer data);
    // Persistent (does NOT remove itself) probe that stamps every
    // delivered RTP buffer into last_frame_wall_ms_.  Cheap: a single
    // atomic store on each frame on the streaming thread.
    static GstPadProbeReturn frameStampProbe(GstPad* pad, GstPadProbeInfo* info, gpointer data);
};

}  // namespace f2c_cpp
