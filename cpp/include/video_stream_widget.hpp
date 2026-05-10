#pragma once

#include <atomic>

#include <QString>
#include <QWidget>

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

    void setupPipeline(int port);
    void destroyPipeline();
    static GstBusSyncReply busSyncHandler(GstBus* bus, GstMessage* msg, gpointer data);
    static gboolean busCallback(GstBus* bus, GstMessage* msg, gpointer data);
    static GstPadProbeReturn firstFrameProbe(GstPad* pad, GstPadProbeInfo* info, gpointer data);
};

}  // namespace f2c_cpp
