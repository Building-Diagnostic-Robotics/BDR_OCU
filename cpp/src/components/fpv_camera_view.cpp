#include "components/fpv_camera_view.hpp"

#include "video_stream_widget.hpp"

#include <QLabel>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace f2c_cpp {

FPVCameraView::FPVCameraView(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    media_stack_ = new QStackedWidget(this);
    media_stack_->setContentsMargins(0, 0, 0, 0);
    media_stack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    placeholder_ = new QWidget(media_stack_);
    placeholder_->setObjectName("FPVCameraViewPlaceholder");
    placeholder_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* placeholder_layout = new QVBoxLayout(placeholder_);
    placeholder_layout->setContentsMargins(8, 8, 8, 8);
    placeholder_layout->setSpacing(4);

    placeholder_title_ = new QLabel(placeholder_);
    placeholder_title_->setAlignment(Qt::AlignCenter);
    placeholder_title_->setStyleSheet(
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 700; color: #D4D4D8; "
                       "background: transparent;"));

    placeholder_subtitle_ = new QLabel(placeholder_);
    placeholder_subtitle_->setAlignment(Qt::AlignCenter);
    placeholder_subtitle_->setStyleSheet(
        QStringLiteral("font-family: 'Arimo'; font-size: 12px; font-weight: 400; color: #9F9FA9; "
                       "background: transparent;"));

    placeholder_layout->addStretch(1);
    placeholder_layout->addWidget(placeholder_title_, 0, Qt::AlignCenter);
    placeholder_layout->addWidget(placeholder_subtitle_, 0, Qt::AlignCenter);
    placeholder_layout->addStretch(1);
    media_stack_->addWidget(placeholder_);

    stream_widget_ = new VideoStreamWidget(media_stack_);
    stream_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    media_stack_->addWidget(stream_widget_);
    media_stack_->setCurrentWidget(placeholder_);

    layout->addWidget(media_stack_);

    connect(stream_widget_, &VideoStreamWidget::firstFrameReady, this, [this]() {
        showStream();
        emit firstFrameReady();
    });
    connect(stream_widget_, &VideoStreamWidget::streamStarted, this,
            &FPVCameraView::streamStarted);
    connect(stream_widget_, &VideoStreamWidget::streamStopped, this, [this]() {
        showPlaceholder();
        emit streamStopped();
    });
    connect(stream_widget_, &VideoStreamWidget::streamError, this,
            [this](const QString& msg) {
                showPlaceholder();
                emit streamError(msg);
            });
}

void FPVCameraView::setPlaceholderText(const QString& title, const QString& subtitle) {
    if (placeholder_title_) {
        placeholder_title_->setText(title);
        placeholder_title_->setVisible(!title.isEmpty());
    }
    if (placeholder_subtitle_) {
        placeholder_subtitle_->setText(subtitle);
        placeholder_subtitle_->setVisible(!subtitle.isEmpty());
    }
}

void FPVCameraView::startStream(int port) {
    if (!stream_widget_) {
        return;
    }
    showPlaceholder();
    stream_widget_->startStream(port);
}

void FPVCameraView::stopStream() {
    if (stream_widget_) {
        stream_widget_->stopStream();
    }
    showPlaceholder();
}

bool FPVCameraView::isPlaying() const {
    return stream_widget_ && stream_widget_->isPlaying();
}

int FPVCameraView::currentPort() const {
    return stream_widget_ ? stream_widget_->currentPort() : 0;
}

void FPVCameraView::showPlaceholder() {
    if (media_stack_ && placeholder_ &&
        media_stack_->currentWidget() != placeholder_) {
        media_stack_->setCurrentWidget(placeholder_);
    }
}

void FPVCameraView::showStream() {
    if (media_stack_ && stream_widget_ &&
        media_stack_->currentWidget() != stream_widget_) {
        media_stack_->setCurrentWidget(stream_widget_);
    }
}

}  // namespace f2c_cpp
