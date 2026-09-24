#include "components/satellite_plan_confirm_dialog.hpp"

#include "ui_theme_constants.hpp"
#include "units_system.hpp"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace f2c_cpp {

namespace {

constexpr int kDialogWidth = 760;
constexpr QSize kThumbnailSize(340, 240);

QLabel* makeLabel(const QString& text, const char* object_name,
                  QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QLatin1String(object_name));
    label->setWordWrap(true);
    return label;
}

}  // namespace

SatellitePlanConfirmDialog::SatellitePlanConfirmDialog(
    TileService* tiles, const Job& job, const QPixmap& thumbnail,
    const PrefetchRequest& request, bool already_cached, QWidget* parent)
    : QDialog(parent),
      tiles_(tiles),
      base_request_(request),
      already_cached_(already_cached) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    setObjectName("PlanConfirmDialog");
    setFixedWidth(kDialogWidth);

    prefetcher_ = new SitePrefetcher(tiles_, this);
    connect(prefetcher_, &SitePrefetcher::statusChanged, this,
            [this](const QString& text) { status_label_->setText(text); });
    connect(prefetcher_, &SitePrefetcher::progress, this,
            [this](int done, int total) {
                progress_->setRange(0, std::max(total, 1));
                progress_->setValue(done);
            });
    connect(prefetcher_, &SitePrefetcher::finished, this,
            &SatellitePlanConfirmDialog::onPrefetchFinished);

    buildUi(job, thumbnail);
    applyStyle();
    refreshEstimate();

    if (!tiles_->hasApiKey()) {
        // Nothing can be fetched in this build. Make that plain up front
        // rather than letting the operator watch a download fail.
        phase_ = Phase::Failed;
        status_label_->setText(
            QStringLiteral("No ArcGIS API key is compiled into this build, "
                           "so imagery cannot be downloaded here."));
        status_label_->setVisible(true);
        primary_button_->setVisible(false);
        close_button_->setVisible(false);
        without_button_->setVisible(true);
        without_button_->setDefault(true);
    }
}

void SatellitePlanConfirmDialog::buildUi(const Job& job,
                                         const QPixmap& thumbnail) {
    // Translucent top-level + a styled inner panel: the pattern
    // bdr_message_box.cpp uses, because a QDialog's own stylesheet
    // background is not painted reliably on a translucent window.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* panel = new QWidget(this);
    panel->setObjectName("PlanConfirmPanel");
    panel->setAttribute(Qt::WA_StyledBackground, true);
    outer->addWidget(panel);

    auto* root = new QVBoxLayout(panel);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(16);

    root->addWidget(makeLabel(QStringLiteral("Confirm Plan"),
                              "PlanConfirmHeader", this));
    root->addWidget(makeLabel(
        already_cached_
            ? QStringLiteral("Saving \"%1\" will refresh the cached site "
                             "imagery so the plan works on the roof without "
                             "a connection.")
                  .arg(job.name)
            : QStringLiteral("Saving \"%1\" will download the site imagery "
                             "so the plan works on the roof without a "
                             "connection.")
                  .arg(job.name),
        "PlanConfirmBody", this));

    // ---- Review row: thumbnail | summary ----
    auto* review = new QHBoxLayout();
    review->setSpacing(20);

    auto* thumb = new QLabel(this);
    thumb->setObjectName("PlanConfirmThumb");
    thumb->setFixedSize(kThumbnailSize);
    thumb->setAlignment(Qt::AlignCenter);
    if (!thumbnail.isNull()) {
        thumb->setPixmap(thumbnail.scaled(kThumbnailSize, Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
    } else {
        thumb->setText(QStringLiteral("No preview"));
    }
    review->addWidget(thumb, 0, Qt::AlignTop);

    estimate_label_ = makeLabel(QString(), "PlanConfirmRow", this);
    estimate_label_->setAlignment(Qt::AlignTop);
    review->addWidget(estimate_label_, 1, Qt::AlignTop);
    root->addLayout(review);

    // ---- Progress ----
    progress_ = new QProgressBar(this);
    progress_->setObjectName("PlanConfirmProgress");
    progress_->setTextVisible(false);
    progress_->setFixedHeight(8);
    progress_->setVisible(false);
    root->addWidget(progress_);

    status_label_ = makeLabel(QString(), "PlanConfirmStatus", this);
    status_label_->setVisible(false);
    root->addWidget(status_label_);

    // ---- Footer ----
    auto* footer = new QHBoxLayout();
    footer->setSpacing(12);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button_->setObjectName("PlanConfirmDanger");
    cancel_button_->setCursor(Qt::PointingHandCursor);
    footer->addWidget(cancel_button_);
    footer->addStretch(1);
    without_button_ = new QPushButton(
        QStringLiteral("Save without imagery"), this);
    without_button_->setObjectName("PlanConfirmLink");
    without_button_->setCursor(Qt::PointingHandCursor);
    without_button_->setVisible(false);
    footer->addWidget(without_button_);
    close_button_ = new QPushButton(QStringLiteral("Save for Later"), this);
    close_button_->setObjectName("PlanConfirmLater");
    close_button_->setCursor(Qt::PointingHandCursor);
    footer->addWidget(close_button_);
    primary_button_ = new QPushButton(QStringLiteral("Save && Continue"),
                                      this);
    primary_button_->setObjectName("PlanConfirmPrimary");
    primary_button_->setCursor(Qt::PointingHandCursor);
    primary_button_->setDefault(true);
    footer->addWidget(primary_button_);
    root->addLayout(footer);

    connect(primary_button_, &QPushButton::clicked, this,
            &SatellitePlanConfirmDialog::onPrimaryClicked);
    connect(close_button_, &QPushButton::clicked, this,
            &SatellitePlanConfirmDialog::onSaveAndCloseClicked);
    connect(without_button_, &QPushButton::clicked, this,
            &SatellitePlanConfirmDialog::onSaveWithoutImagery);
    connect(cancel_button_, &QPushButton::clicked, this,
            &SatellitePlanConfirmDialog::onCancel);
}

void SatellitePlanConfirmDialog::applyStyle() {
    const UiThemeTokens t = appThemeTokens();
    setStyleSheet(QStringLiteral(R"CSS(
        #PlanConfirmPanel {
            background: %1; border: 1px solid %2; border-radius: 14px;
        }
        QLabel { background: transparent; }
        #PlanConfirmHeader {
            color: %3; font-family: 'Arimo'; font-size: 20px; font-weight: 700;
        }
        #PlanConfirmBody {
            color: %4; font-family: 'Arimo'; font-size: 14px;
        }
        #PlanConfirmRow {
            color: %3; font-family: 'Arimo'; font-size: 13px;
        }
        #PlanConfirmThumb {
            background: %6; border: 1px solid %2; border-radius: 10px;
            color: %5; font-family: 'Arimo'; font-size: 13px;
        }
        #PlanConfirmProgress {
            background: %6; border: none; border-radius: 4px;
        }
        #PlanConfirmProgress::chunk { background: %7; border-radius: 4px; }
        #PlanConfirmStatus {
            color: %4; font-family: 'Arimo'; font-size: 13px;
        }
        QPushButton#PlanConfirmPrimary {
            background: %7; border: none; border-radius: 10px; padding: 0 22px;
            min-height: 40px; color: %8; font-family: 'Arimo'; font-size: 14px;
            font-weight: 700;
        }
        QPushButton#PlanConfirmPrimary:hover { background: %9; }
        QPushButton#PlanConfirmPrimary:disabled { background: %2; color: %5; }
        QPushButton#PlanConfirmLater {
            background: transparent; border: 1px solid %13; border-radius: 10px;
            padding: 0 18px; min-height: 40px; color: %13;
            font-family: 'Arimo'; font-size: 14px; font-weight: 700;
        }
        QPushButton#PlanConfirmLater:hover { background: %10; }
        QPushButton#PlanConfirmLater:disabled {
            border-color: %2; color: %5;
        }
        QPushButton#PlanConfirmDanger {
            background: %11; border: none; border-radius: 10px;
            padding: 0 18px; min-height: 40px; color: #FFFFFF;
            font-family: 'Arimo'; font-size: 14px; font-weight: 700;
        }
        QPushButton#PlanConfirmDanger:hover { background: %12; }
        QPushButton#PlanConfirmDanger:disabled { background: %2; color: %5; }
        QPushButton#PlanConfirmLink {
            background: transparent; border: none; color: %5;
            font-family: 'Arimo'; font-size: 13px; text-decoration: underline;
        }
        QPushButton#PlanConfirmLink:hover { color: %3; }
    )CSS")
                      .arg(t.surface, t.raised_border, t.text, t.body, t.muted,
                           t.raised, t.accent_green, t.on_accent,
                           t.accent_green_hover)
                      .arg(t.neutral_hover, t.danger_fill, t.danger_fill_hover,
                           t.accent_text));
}

void SatellitePlanConfirmDialog::refreshEstimate() {
    estimate_label_->setText(
        QStringLiteral("%1 around this address")
            .arg(units::formatLength(base_request_.radius_m, 0)));
}

void SatellitePlanConfirmDialog::setDownloading(bool downloading) {
    primary_button_->setEnabled(!downloading);
    close_button_->setEnabled(!downloading);
    progress_->setVisible(downloading || phase_ == Phase::Done);
    status_label_->setVisible(true);
    cancel_button_->setText(downloading ? QStringLiteral("Stop")
                                        : QStringLiteral("Cancel"));
}

void SatellitePlanConfirmDialog::onPrimaryClicked() {
    close_after_save_ = false;
    beginDownload();
}

void SatellitePlanConfirmDialog::onSaveAndCloseClicked() {
    close_after_save_ = true;
    beginDownload();
}

void SatellitePlanConfirmDialog::beginDownload() {
    switch (phase_) {
        case Phase::Done:
            accept();
            return;
        case Phase::Downloading:
            return;
        case Phase::Review:
        case Phase::Failed:
            break;
    }
    phase_ = Phase::Downloading;
    without_button_->setVisible(false);
    progress_->setRange(0, 0);  // busy until the age probe answers
    setDownloading(true);
    prefetcher_->start(base_request_);
}

void SatellitePlanConfirmDialog::onPrefetchFinished(
    const PrefetchResult& result) {
    if (result.ok) {
        phase_ = Phase::Done;
        manifest_ = result.manifest;
        outcome_ = Outcome::SavedWithImagery;
        setDownloading(false);
        progress_->setRange(0, 1);
        progress_->setValue(1);
        status_label_->setText(
            QStringLiteral("Imagery cached — %1").arg(result.message));
        // Both saves are one press: the download is the same, and the
        // button the operator chose already says where they land.
        accept();
        return;
    }
    phase_ = Phase::Failed;
    setDownloading(false);
    progress_->setVisible(false);
    status_label_->setText(result.message);
    primary_button_->setText(QStringLiteral("Retry && Continue"));
    close_button_->setText(QStringLiteral("Retry && Save for Later"));
    // A partial cache is still worth keeping; the plan just is not field-
    // ready until a retry completes.
    without_button_->setVisible(true);
}

void SatellitePlanConfirmDialog::onSaveWithoutImagery() {
    outcome_ = Outcome::SavedWithoutImagery;
    accept();
}

void SatellitePlanConfirmDialog::onCancel() {
    if (phase_ == Phase::Downloading) {
        prefetcher_->cancel();  // finished(ok=false) lands us in Failed
        return;
    }
    outcome_ = Outcome::Cancelled;
    reject();
}

void SatellitePlanConfirmDialog::closeEvent(QCloseEvent* event) {
    if (phase_ == Phase::Downloading) {
        prefetcher_->cancel();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void SatellitePlanConfirmDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        onCancel();
        return;
    }
    QDialog::keyPressEvent(event);
}

}  // namespace f2c_cpp
