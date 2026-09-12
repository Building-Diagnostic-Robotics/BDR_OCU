#include "components/satellite_plan_confirm_dialog.hpp"

#include "satellite_geo_math.hpp"
#include "satellite_tile_service.hpp"
#include "units_system.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace f2c_cpp {

namespace {

constexpr int kDialogWidth = 760;
constexpr QSize kThumbnailSize(340, 240);
constexpr int kMaxEdgeRows = 8;

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

    tiles_->listWaybackReleases(
        [this](QVector<TileService::WaybackRelease> releases) {
            for (const TileService::WaybackRelease& r : releases) {
                wayback_combo_->addItem(r.label, r.id);
            }
        });

    if (!tiles_->hasApiKey()) {
        // Nothing can be fetched in this build. Make that plain up front
        // rather than letting the operator watch a download fail.
        phase_ = Phase::Failed;
        status_label_->setText(
            QStringLiteral("No ArcGIS API key is compiled into this build, "
                           "so imagery cannot be downloaded here."));
        status_label_->setVisible(true);
        primary_button_->setVisible(false);
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

    auto* summary = new QVBoxLayout();
    summary->setSpacing(6);

    const RoiPolygon& poly = job.polygon;
    const int n = poly.vertices.size();
    summary->addWidget(makeLabel(
        QStringLiteral("ROI — %1 vertices").arg(n), "PlanConfirmSection",
        this));
    int marked = 0;
    for (int i = 0; i < n; ++i) {
        const QPointF enu =
            geo::enuFromGeo(poly.vertices[i], poly.vertices[(i + 1) % n]);
        const bool roof = i < poly.roof_edges.size() && poly.roof_edges[i];
        marked += roof ? 1 : 0;
        if (i < kMaxEdgeRows) {
            summary->addWidget(makeLabel(
                QStringLiteral("Edge %1 · %2%3%4")
                    .arg(i + 1)
                    .arg(units::formatLength(std::hypot(enu.x(), enu.y()), 1))
                    .arg(poly.lockedLength(i) > 0.0 ? QStringLiteral(" · pinned")
                                                    : QString())
                    .arg(roof ? QStringLiteral(" · roof edge") : QString()),
                roof ? "PlanConfirmRoofEdge" : "PlanConfirmRow", this));
        }
    }
    if (n > kMaxEdgeRows) {
        summary->addWidget(makeLabel(
            QStringLiteral("+%1 more edges").arg(n - kMaxEdgeRows),
            "PlanConfirmRow", this));
    }
    summary->addWidget(makeLabel(
        marked == 0 ? QStringLiteral("No roof edges marked yet — the field "
                                     "Edge Review step will ask.")
                    : QStringLiteral("%1 roof edge%2 marked.")
                          .arg(marked)
                          .arg(marked == 1 ? QString() : QStringLiteral("s")),
        "PlanConfirmMuted", this));

    summary->addSpacing(6);
    summary->addWidget(
        makeLabel(QStringLiteral("Robot"), "PlanConfirmSection", this));
    summary->addWidget(makeLabel(
        job.robot.valid
            ? QStringLiteral("%1, %2 · heading %3°")
                  .arg(job.robot.lat, 0, 'f', 6)
                  .arg(job.robot.lon, 0, 'f', 6)
                  .arg(job.robot.heading_deg, 0, 'f', 0)
            : QStringLiteral("Not placed — the field alignment step "
                             "sets it."),
        job.robot.valid ? "PlanConfirmRow" : "PlanConfirmMuted", this));

    summary->addSpacing(6);
    summary->addWidget(
        makeLabel(QStringLiteral("Imagery"), "PlanConfirmSection", this));
    estimate_label_ = makeLabel(QString(), "PlanConfirmRow", this);
    summary->addWidget(estimate_label_);
    summary->addStretch(1);
    review->addLayout(summary, 1);
    root->addLayout(review);

    // ---- Advanced disclosure ----
    advanced_toggle_ = new QToolButton(this);
    advanced_toggle_->setObjectName("PlanConfirmAdvancedToggle");
    advanced_toggle_->setText(QStringLiteral("Advanced imagery options"));
    advanced_toggle_->setCheckable(true);
    advanced_toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advanced_toggle_->setArrowType(Qt::RightArrow);
    advanced_toggle_->setCursor(Qt::PointingHandCursor);
    root->addWidget(advanced_toggle_, 0, Qt::AlignLeft);

    advanced_host_ = new QWidget(this);
    advanced_host_->setObjectName("PlanConfirmAdvanced");
    auto* form = new QFormLayout(advanced_host_);
    form->setContentsMargins(12, 8, 12, 8);
    form->setSpacing(8);
    form->setLabelAlignment(Qt::AlignLeft);

    radius_spin_ = new QSpinBox(advanced_host_);
    radius_spin_->setRange(100, 3000);
    radius_spin_->setSingleStep(50);
    radius_spin_->setValue(base_request_.radius_m);
    radius_spin_->setSuffix(QStringLiteral(" m"));
    form->addRow(QStringLiteral("Cache radius"), radius_spin_);

    max_zoom_spin_ = new QSpinBox(advanced_host_);
    max_zoom_spin_->setRange(16, 20);
    max_zoom_spin_->setValue(base_request_.max_zoom);
    form->addRow(QStringLiteral("Max detail (zoom)"), max_zoom_spin_);

    max_age_spin_ = new QSpinBox(advanced_host_);
    max_age_spin_->setRange(1, 15);
    max_age_spin_->setValue(base_request_.max_age_years);
    max_age_spin_->setSuffix(QStringLiteral(" years"));
    form->addRow(QStringLiteral("Newest imagery within"), max_age_spin_);

    clarity_check_ =
        new QCheckBox(QStringLiteral("Prefer Esri Clarity"), advanced_host_);
    clarity_check_->setChecked(base_request_.clarity);
    form->addRow(QStringLiteral("Layer"), clarity_check_);

    wayback_combo_ = new QComboBox(advanced_host_);
    wayback_combo_->addItem(QStringLiteral("Live mosaic (date-capped)"),
                            QString());
    form->addRow(QStringLiteral("Wayback release"), wayback_combo_);

    advanced_host_->setVisible(false);
    root->addWidget(advanced_host_);

    connect(advanced_toggle_, &QToolButton::toggled, this, [this](bool on) {
        advanced_toggle_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        advanced_host_->setVisible(on);
        adjustSize();
    });
    const auto refresh = [this] { refreshEstimate(); };
    connect(radius_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            refresh);
    connect(max_zoom_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            refresh);
    connect(max_age_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            refresh);

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
    without_button_ = new QPushButton(
        QStringLiteral("Save without imagery"), this);
    without_button_->setObjectName("PlanConfirmLink");
    without_button_->setCursor(Qt::PointingHandCursor);
    without_button_->setVisible(false);
    footer->addWidget(without_button_);
    footer->addStretch(1);
    cancel_button_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button_->setObjectName("PlanConfirmSecondary");
    cancel_button_->setCursor(Qt::PointingHandCursor);
    footer->addWidget(cancel_button_);
    primary_button_ = new QPushButton(
        already_cached_ ? QStringLiteral("Refresh Imagery && Save")
                        : QStringLiteral("Download && Save"),
        this);
    primary_button_->setObjectName("PlanConfirmPrimary");
    primary_button_->setCursor(Qt::PointingHandCursor);
    primary_button_->setDefault(true);
    footer->addWidget(primary_button_);
    root->addLayout(footer);

    connect(primary_button_, &QPushButton::clicked, this,
            &SatellitePlanConfirmDialog::onPrimaryClicked);
    connect(without_button_, &QPushButton::clicked, this,
            &SatellitePlanConfirmDialog::onSaveWithoutImagery);
    connect(cancel_button_, &QPushButton::clicked, this,
            &SatellitePlanConfirmDialog::onCancel);
}

void SatellitePlanConfirmDialog::applyStyle() {
    setStyleSheet(QStringLiteral(R"CSS(
        #PlanConfirmPanel {
            background: #18181B; border: 1px solid #3F3F46; border-radius: 14px;
        }
        QLabel { background: transparent; }
        #PlanConfirmHeader {
            color: #F4F4F5; font-family: 'Arimo'; font-size: 20px; font-weight: 700;
        }
        #PlanConfirmBody {
            color: #D4D4D8; font-family: 'Arimo'; font-size: 14px;
        }
        #PlanConfirmSection {
            color: #9F9FA9; font-family: 'Arimo'; font-size: 11px; font-weight: 700;
            letter-spacing: 0.5px;
        }
        #PlanConfirmRow {
            color: #F4F4F5; font-family: 'Arimo'; font-size: 13px;
        }
        #PlanConfirmRoofEdge {
            color: #FF6467; font-family: 'Arimo'; font-size: 13px;
        }
        #PlanConfirmMuted {
            color: #9F9FA9; font-family: 'Arimo'; font-size: 13px;
        }
        #PlanConfirmThumb {
            background: #0b0b0b; border: 1px solid #3F3F46; border-radius: 10px;
            color: #52525C; font-family: 'Arimo'; font-size: 13px;
        }
        #PlanConfirmAdvancedToggle {
            background: transparent; border: none; color: #9F9FA9;
            font-family: 'Arimo'; font-size: 13px; padding: 2px 0;
        }
        #PlanConfirmAdvancedToggle:hover { color: #F4F4F5; }
        #PlanConfirmAdvanced {
            background: #27272A; border: 1px solid #3F3F46; border-radius: 10px;
        }
        #PlanConfirmAdvanced QLabel {
            color: #D4D4D8; font-family: 'Arimo'; font-size: 13px;
        }
        #PlanConfirmAdvanced QSpinBox, #PlanConfirmAdvanced QComboBox {
            background: #18181B; border: 1px solid #3F3F46; border-radius: 8px;
            padding: 4px 10px; color: #F4F4F5; font-family: 'Arimo'; font-size: 13px;
        }
        #PlanConfirmAdvanced QComboBox QAbstractItemView {
            background: #18181B; color: #F4F4F5; border: 1px solid #3F3F46;
            selection-background-color: rgba(0, 188, 125, 0.30);
        }
        #PlanConfirmAdvanced QCheckBox {
            color: #F4F4F5; font-family: 'Arimo'; font-size: 13px;
        }
        #PlanConfirmAdvanced QCheckBox::indicator {
            width: 16px; height: 16px; border: 1px solid #3F3F46; border-radius: 4px;
            background: #18181B;
        }
        #PlanConfirmAdvanced QCheckBox::indicator:checked {
            background: #00BC7D; border-color: #00BC7D;
        }
        #PlanConfirmProgress {
            background: #27272A; border: none; border-radius: 4px;
        }
        #PlanConfirmProgress::chunk { background: #00BC7D; border-radius: 4px; }
        #PlanConfirmStatus {
            color: #D4D4D8; font-family: 'Arimo'; font-size: 13px;
        }
        QPushButton#PlanConfirmPrimary {
            background: #00BC7D; border: none; border-radius: 10px; padding: 0 22px;
            min-height: 40px; color: #FFFFFF; font-family: 'Arimo'; font-size: 14px;
            font-weight: 700;
        }
        QPushButton#PlanConfirmPrimary:hover { background: #00A86D; }
        QPushButton#PlanConfirmPrimary:disabled { background: #3F3F46; color: #9F9FA9; }
        QPushButton#PlanConfirmSecondary {
            background: #27272A; border: 1px solid #3F3F46; border-radius: 10px;
            padding: 0 18px; min-height: 40px; color: #F4F4F5;
            font-family: 'Arimo'; font-size: 14px; font-weight: 600;
        }
        QPushButton#PlanConfirmSecondary:hover { background: #3F3F46; }
        QPushButton#PlanConfirmLink {
            background: transparent; border: none; color: #9F9FA9;
            font-family: 'Arimo'; font-size: 13px; text-decoration: underline;
        }
        QPushButton#PlanConfirmLink:hover { color: #F4F4F5; }
    )CSS"));
}

void SatellitePlanConfirmDialog::devSetAdvancedOpen(bool open) {
    advanced_toggle_->setChecked(open);
}

PrefetchRequest SatellitePlanConfirmDialog::currentRequest() const {
    PrefetchRequest request = base_request_;
    request.radius_m = radius_spin_->value();
    request.max_zoom = max_zoom_spin_->value();
    request.max_age_years = max_age_spin_->value();
    request.clarity = clarity_check_->isChecked();
    request.wayback_release = wayback_combo_->currentData().toString();
    return request;
}

void SatellitePlanConfirmDialog::refreshEstimate() {
    const PrefetchRequest request = currentRequest();
    estimate_label_->setText(
        QStringLiteral("~%1 tiles · %2 radius · up to z%3 · flown within "
                       "%4 y")
            .arg(SitePrefetcher::estimateTileCount(request))
            .arg(units::formatLength(request.radius_m, 0))
            .arg(request.max_zoom)
            .arg(request.max_age_years));
}

void SatellitePlanConfirmDialog::setDownloading(bool downloading) {
    advanced_toggle_->setEnabled(!downloading);
    advanced_host_->setEnabled(!downloading);
    primary_button_->setEnabled(!downloading);
    progress_->setVisible(downloading || phase_ == Phase::Done);
    status_label_->setVisible(true);
    cancel_button_->setText(downloading ? QStringLiteral("Stop")
                                        : QStringLiteral("Cancel"));
}

void SatellitePlanConfirmDialog::onPrimaryClicked() {
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
    prefetcher_->start(currentRequest());
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
        primary_button_->setText(QStringLiteral("Done"));
        cancel_button_->setVisible(false);
        primary_button_->setFocus();
        return;
    }
    phase_ = Phase::Failed;
    setDownloading(false);
    progress_->setVisible(false);
    status_label_->setText(result.message);
    primary_button_->setText(QStringLiteral("Retry Download"));
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
