/**
 * @file fpv_camera_view.hpp
 * @brief Reusable FPV camera display.
 *
 * Mirrors the Exploration FPV layout pattern (no HUD overlay) so other
 * stages can embed the same low-latency video feed without inheriting
 * the deeply nested styled chrome that produced repaint storms on
 * the Planner Scan stage.
 */

#pragma once

#include <QWidget>

class QLabel;
class QStackedWidget;

namespace f2c_cpp {

class VideoStreamWidget;

class FPVCameraView : public QWidget {
    Q_OBJECT

public:
    explicit FPVCameraView(QWidget* parent = nullptr);

    /** Configure the text shown while the stream is not yet active. */
    void setPlaceholderText(const QString& title, const QString& subtitle);

    /** Start receiving on the given UDP port (forwards to VideoStreamWidget). */
    void startStream(int port);
    /** Stop the stream and revert to the placeholder. */
    void stopStream();

    bool isPlaying() const;
    int currentPort() const;

    VideoStreamWidget* streamWidget() const { return stream_widget_; }
    QWidget* placeholderWidget() const { return placeholder_; }

signals:
    void streamStarted();
    void streamStopped();
    void streamError(const QString& msg);
    void firstFrameReady();

private:
    void showPlaceholder();
    void showStream();

    QStackedWidget* media_stack_ = nullptr;
    QWidget* placeholder_ = nullptr;
    QLabel* placeholder_title_ = nullptr;
    QLabel* placeholder_subtitle_ = nullptr;
    VideoStreamWidget* stream_widget_ = nullptr;
};

}  // namespace f2c_cpp
