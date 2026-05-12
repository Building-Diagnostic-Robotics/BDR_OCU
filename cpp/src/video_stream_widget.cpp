#include "video_stream_widget.hpp"

#include <iostream>

#include <QDateTime>
#include <QHideEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

namespace f2c_cpp {

namespace {

struct FirstFrameProbeContext {
    VideoStreamWidget* widget = nullptr;
    qulonglong generation = 0;
};

}  // namespace

VideoStreamWidget::VideoStreamWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setAutoFillBackground(true);

    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }
}

VideoStreamWidget::~VideoStreamWidget() {
    destroyPipeline();
}

void VideoStreamWidget::setupPipeline(int port) {
    destroyPipeline();
    const qulonglong generation = stream_generation_.fetch_add(1) + 1;
    first_frame_emitted_ = false;
    // Stage 6 freeze detector: a fresh pipeline starts with "no frames
    // yet" so AppShellWindow's slow tick doesn't read a stale value
    // from a previous run.  Updated by frameStampProbe on every buffer.
    last_frame_wall_ms_.store(0);

    QString pipelineStr = QString(
        "udpsrc port=%1 buffer-size=212992 "
        "caps=\"application/x-rtp, media=video, encoding-name=H264, payload=96, clock-rate=90000\" "
        "! rtpjitterbuffer latency=20 do-lost=true "
        "! rtph264depay "
        "! h264parse "
        "! avdec_h264 "
        "! videoconvert "
        "! identity name=first_frame_probe "
        "! autovideosink sync=false name=videosink"
    ).arg(port);

    GError* error = nullptr;
    pipeline_ = gst_parse_launch(pipelineStr.toUtf8().constData(), &error);

    if (error) {
        QString errMsg = QString("Pipeline error: %1").arg(error->message);
        g_error_free(error);
        emit streamError(errMsg);
        std::cerr << "[VideoStream] " << errMsg.toStdString() << std::endl;
        return;
    }

    if (!pipeline_) {
        emit streamError("Failed to create pipeline");
        return;
    }

    if (GstElement* first_frame_probe = gst_bin_get_by_name(GST_BIN(pipeline_), "first_frame_probe")) {
        if (GstPad* probe_pad = gst_element_get_static_pad(first_frame_probe, "src")) {
            auto* context = new FirstFrameProbeContext{this, generation};
            gst_pad_add_probe(
                probe_pad,
                GST_PAD_PROBE_TYPE_BUFFER,
                firstFrameProbe,
                context,
                [](gpointer data) { delete static_cast<FirstFrameProbeContext*>(data); });
            // Stage 6: persistent buffer stamp probe.  Same pad as the
            // first-frame probe but does NOT remove itself; fires once
            // per delivered frame and does a single atomic store into
            // last_frame_wall_ms_ on the streaming thread.  Safe: no
            // Qt signal emission, no allocation, no locks.
            gst_pad_add_probe(
                probe_pad,
                GST_PAD_PROBE_TYPE_BUFFER,
                frameStampProbe,
                this,
                nullptr);
            gst_object_unref(probe_pad);
        }
        gst_object_unref(first_frame_probe);
    }

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    gst_bus_set_sync_handler(bus, busSyncHandler, this, nullptr);
    gst_object_unref(bus);

    if (!bus_poll_timer_) {
        bus_poll_timer_ = new QTimer(this);
        connect(bus_poll_timer_, &QTimer::timeout, this, &VideoStreamWidget::pollBus);
    }
    bus_poll_timer_->start(100);

    current_port_ = port;
    std::cout << "[VideoStream] Pipeline created for port " << port << std::endl;
}

GstBusSyncReply VideoStreamWidget::busSyncHandler(GstBus* bus, GstMessage* msg, gpointer data) {
    Q_UNUSED(bus);
    VideoStreamWidget* self = static_cast<VideoStreamWidget*>(data);

    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT) {
        if (gst_is_video_overlay_prepare_window_handle_message(msg)) {
            WId winId = self->winId();
            gst_video_overlay_set_window_handle(
                GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg)),
                static_cast<guintptr>(winId));
            gst_message_unref(msg);
            return GST_BUS_DROP;
        }
    }
    return GST_BUS_PASS;
}

GstPadProbeReturn VideoStreamWidget::firstFrameProbe(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
    Q_UNUSED(pad);

    if (!info || (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }

    auto* context = static_cast<FirstFrameProbeContext*>(data);
    if (!context || !context->widget) {
        return GST_PAD_PROBE_REMOVE;
    }

    QMetaObject::invokeMethod(context->widget,
                              "handleFirstFrameReady",
                              Qt::QueuedConnection,
                              Q_ARG(qulonglong, context->generation));

    return GST_PAD_PROBE_REMOVE;
}

void VideoStreamWidget::handleFirstFrameReady(qulonglong generation) {
    if (generation != stream_generation_.load()) {
        return;
    }
    if (first_frame_emitted_) {
        return;
    }
    first_frame_emitted_ = true;
    emit firstFrameReady();
}

GstPadProbeReturn VideoStreamWidget::frameStampProbe(GstPad* pad,
                                                    GstPadProbeInfo* info,
                                                    gpointer data) {
    Q_UNUSED(pad);
    if (!info || (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }
    auto* self = static_cast<VideoStreamWidget*>(data);
    if (!self) {
        return GST_PAD_PROBE_OK;
    }
    self->last_frame_wall_ms_.store(QDateTime::currentMSecsSinceEpoch());
    return GST_PAD_PROBE_OK;  // intentionally NOT REMOVE — we want every frame
}

void VideoStreamWidget::pollBus() {
    if (!pipeline_) return;

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    if (!bus) return;

    GstMessage* msg;
    while ((msg = gst_bus_pop(bus)) != nullptr) {
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_QOS:
                break;
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(msg, &err, &debug);
                QString errMsg = QString("Stream error: %1").arg(err ? err->message : "unknown");
                std::cerr << "[VideoStream] Error: " << errMsg.toStdString() << std::endl;
                if (debug) std::cerr << "[VideoStream] Debug: " << debug << std::endl;
                if (err) g_error_free(err);
                if (debug) g_free(debug);
                emit streamError(errMsg);
                break;
            }
            case GST_MESSAGE_EOS:
                std::cout << "[VideoStream] End of stream" << std::endl;
                playing_ = false;
                emit streamStopped();
                break;
            case GST_MESSAGE_STATE_CHANGED:
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
                    GstState old_state, new_state, pending_state;
                    gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                    std::cout << "[VideoStream] State: " << gst_element_state_get_name(old_state)
                              << " -> " << gst_element_state_get_name(new_state) << std::endl;
                    if (new_state == GST_STATE_PLAYING && !playing_) {
                        playing_ = true;
                        emit streamStarted();
                    } else if (new_state == GST_STATE_NULL && playing_) {
                        playing_ = false;
                        emit streamStopped();
                    }
                }
                break;
            default:
                break;
        }
        gst_message_unref(msg);
    }

    gst_object_unref(bus);
}

gboolean VideoStreamWidget::busCallback(GstBus* bus, GstMessage* msg, gpointer data) {
    Q_UNUSED(bus);
    Q_UNUSED(msg);
    Q_UNUSED(data);
    return TRUE;
}

void VideoStreamWidget::startStream(int port) {
    const bool already_playing_same_port = pipeline_ && current_port_ == port && playing_;
    if (already_playing_same_port) {
        return;
    }
    setupPipeline(port);
    if (pipeline_) {
        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            emit streamError("Failed to start stream");
            std::cerr << "[VideoStream] Failed to set pipeline to PLAYING" << std::endl;
        } else {
            std::cout << "[VideoStream] Starting stream on port " << port << std::endl;
        }
    }
}

void VideoStreamWidget::stopStream() {
    if (!pipeline_ && !playing_) {
        return;
    }
    destroyPipeline();
    emit streamStopped();
    std::cout << "[VideoStream] Stream stopped" << std::endl;
}

void VideoStreamWidget::destroyPipeline() {
    if (bus_poll_timer_) {
        bus_poll_timer_->stop();
    }
    stream_generation_.fetch_add(1);
    first_frame_emitted_ = false;

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_element_get_state(pipeline_, nullptr, nullptr, GST_SECOND);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        playing_ = false;
    }
}

void VideoStreamWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (auto_start_on_show_ && !playing_) {
        startStream(current_port_);
    }
}

void VideoStreamWidget::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
}

void VideoStreamWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
}

}  // namespace f2c_cpp
