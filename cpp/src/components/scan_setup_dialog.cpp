/**
 * @file scan_setup_dialog.cpp
 * @brief Implementation of the Start New Scan mode/plan selector.
 *
 * Visual vocabulary lifted from mission_metadata_dialog.cpp (dark-only zinc
 * palette: #18181b surface, #27272a inputs, #3f3f47 secondary, #00BC7D
 * accent) and dashboard_screen.cpp's makeActionButton (2px brand border
 * cards, tinted stroke SVG icons, Arimo 700 18 / 400 14).
 */

#include "components/scan_setup_dialog.hpp"

#include "satellite_tile_service.hpp"

#include <QDate>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSvgRenderer>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace f2c_cpp {

namespace {

constexpr int kDialogFixedWidth = 580;
constexpr int kPlanRowHeight = 64;
constexpr int kPlanListMaxVisible = 4;
constexpr int kModeCardHeight = 150;

constexpr const char* kAccentGreen = "#00BC7D";
constexpr const char* kAccentBlue = "#2B7FFF";
constexpr const char* kMuted = "#9F9FA9";
constexpr const char* kMutedBorder = "#3f3f47";

/** Same stroke-retint approach as dashboard_screen.cpp's loadSvgPixmap. */
QPixmap tintedSvg(const QString& resource_path, int w, int h,
                  const QString& stroke_color) {
    QFile file(resource_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }
    QByteArray data = file.readAll();
    file.close();
    if (!stroke_color.isEmpty()) {
        QByteArray needle("stroke=\"");
        int index = data.indexOf(needle);
        while (index >= 0) {
            const int value_start = index + needle.size();
            const int value_end = data.indexOf('"', value_start);
            if (value_end <= value_start) {
                break;
            }
            data = data.left(value_start) + stroke_color.toUtf8() +
                   data.mid(value_end);
            index = data.indexOf(needle, value_start);
        }
    }
    QSvgRenderer renderer(data);
    if (!renderer.isValid()) {
        return QPixmap();
    }
    QPixmap pixmap(w, h);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    return pixmap;
}

}  // namespace

ScanSetupDialog::ScanSetupDialog(const QVector<Job>& jobs, QWidget* parent)
    : QDialog(parent) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setModal(true);
    setObjectName("ScanSetupDialog");
    setFixedWidth(kDialogFixedWidth);
    setAttribute(Qt::WA_StyledBackground, true);
    buildUi(jobs);
    applyStyle();
    startImageryProbe();
}

ScanSetupDialog::~ScanSetupDialog() {
    if (probe_inflight_) {
        // abort() emits finished() synchronously; the slot must not run on
        // a dialog that is mid-destruction.
        probe_inflight_->disconnect(this);
        probe_inflight_->abort();
        probe_inflight_->deleteLater();
        probe_inflight_ = nullptr;
    }
}

// ---- Imagery reachability gate ---------------------------------------------

void ScanSetupDialog::startImageryProbe() {
    probe_nam_ = new QNetworkAccessManager(this);
    probe_timer_ = new QTimer(this);
    probe_timer_->setInterval(kProbeIntervalMs);
    connect(probe_timer_, &QTimer::timeout, this,
            &ScanSetupDialog::onProbeTick);
    applyImageryReachable(false);
    onProbeTick();
    probe_timer_->start();
}

void ScanSetupDialog::onProbeTick() {
    if (probe_inflight_) {
        return;  // the previous probe is still within its timeout
    }
    QNetworkRequest request(TileService::connectivityProbeUrl());
    request.setTransferTimeout(kProbeTimeoutMs);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    probe_inflight_ = probe_nam_->head(request);
    connect(probe_inflight_, &QNetworkReply::finished, this,
            [this, reply = probe_inflight_] { onProbeFinished(reply); });
}

void ScanSetupDialog::onProbeFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (reply == probe_inflight_) {
        probe_inflight_ = nullptr;
    }
    probe_answered_ = true;
    if (reply->error() != QNetworkReply::NoError) {
        probe_successes_ = 0;
        applyImageryReachable(false);
        return;
    }
    probe_successes_ = std::min(probe_successes_ + 1, kProbeSuccessesToEnable);
    applyImageryReachable(probe_successes_ >= kProbeSuccessesToEnable);
}

void ScanSetupDialog::applyImageryReachable(bool reachable) {
    if (!satellite_card_) {
        return;
    }
    imagery_reachable_ = reachable;
    satellite_card_->setEnabled(reachable);
    satellite_card_->setCursor(reachable ? Qt::PointingHandCursor
                                         : Qt::ForbiddenCursor);
    const QString brand = reachable ? QString::fromLatin1(kAccentBlue)
                                    : QString::fromLatin1(kMutedBorder);
    satellite_card_->setStyleSheet(QStringLiteral(
        "#SetupCardSatellite {"
        "  background: transparent;"
        "  border: 2px solid %1;"
        "  border-radius: 10px;"
        "  text-align: left;"
        "}"
        "#SetupCardSatellite:hover:enabled { background: rgba(255,255,255,0.03); }"
        "#SetupCardSatellite:focus { outline: none; }")
        .arg(brand));
    satellite_title_->setStyleSheet(QStringLiteral(
        "font-family: 'Arimo'; font-weight: 700; font-size: 18px; "
        "line-height: 28px; color: %1; background: transparent;")
        .arg(brand));
    satellite_description_->setText(
        reachable ? satellite_description_text_
                  : (probe_answered_
                         ? QStringLiteral("Needs an internet connection —\n"
                                          "plan satellite jobs in the office")
                         : QStringLiteral("Checking imagery connection…")));
    satellite_card_->setToolTip(
        reachable ? QString()
                  : QStringLiteral("Satellite imagery is unreachable. Plan "
                                   "satellite jobs from the office, where "
                                   "the site imagery can be downloaded."));
}

void ScanSetupDialog::buildUi(const QVector<Job>& jobs) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(16);

    // ---- Header: title + subtitle left, close X right ----
    auto* header_row = new QHBoxLayout();
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(8);

    auto* header_text = new QVBoxLayout();
    header_text->setSpacing(2);
    auto* title = new QLabel(QStringLiteral("Start New Scan"), this);
    title->setObjectName("SetupTitle");
    header_text->addWidget(title);
    auto* subtitle = new QLabel(
        QStringLiteral("Choose a saved plan or start a new one"), this);
    subtitle->setObjectName("SetupSubtitle");
    header_text->addWidget(subtitle);
    header_row->addLayout(header_text, 1);

    auto* close_button = new QPushButton(this);
    close_button->setObjectName("SetupClose");
    close_button->setIcon(QIcon(QStringLiteral(":/assets/dialog/close.svg")));
    close_button->setIconSize(QSize(16, 16));
    close_button->setFixedSize(28, 28);
    close_button->setCursor(Qt::PointingHandCursor);
    connect(close_button, &QPushButton::clicked, this, &QDialog::reject);
    header_row->addWidget(close_button, 0, Qt::AlignTop);
    root->addLayout(header_row);

    // ---- Saved plans (unexecuted first, then most recently updated) ----
    QVector<Job> sorted = jobs;
    std::sort(sorted.begin(), sorted.end(), [](const Job& a, const Job& b) {
        if (a.executed() != b.executed()) {
            return !a.executed();
        }
        return a.updated > b.updated;
    });

    if (!sorted.isEmpty()) {
        auto* plans_label = new QLabel(QStringLiteral("SAVED PLANS"), this);
        plans_label->setObjectName("SetupSectionLabel");
        root->addWidget(plans_label);

        auto* list_host = new QWidget(this);
        auto* list_layout = new QVBoxLayout(list_host);
        list_layout->setContentsMargins(0, 0, 0, 0);
        list_layout->setSpacing(8);
        for (const Job& job : sorted) {
            list_layout->addWidget(buildPlanRow(job, list_host));
        }
        list_layout->addStretch(1);

        if (sorted.size() > kPlanListMaxVisible) {
            auto* scroll = new QScrollArea(this);
            scroll->setObjectName("SetupPlanScroll");
            scroll->setWidget(list_host);
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            scroll->setFixedHeight(kPlanListMaxVisible * (kPlanRowHeight + 8));
            root->addWidget(scroll);
        } else {
            root->addWidget(list_host);
        }

        // "or" divider — two hairlines around muted text.
        auto* divider_row = new QHBoxLayout();
        divider_row->setSpacing(12);
        auto make_line = [this]() {
            auto* line = new QFrame(this);
            line->setObjectName("SetupDividerLine");
            line->setFrameShape(QFrame::HLine);
            line->setFixedHeight(1);
            return line;
        };
        divider_row->addWidget(make_line(), 1);
        auto* divider_label =
            new QLabel(QStringLiteral("or start from scratch"), this);
        divider_label->setObjectName("SetupDividerLabel");
        divider_row->addWidget(divider_label, 0);
        divider_row->addWidget(make_line(), 1);
        root->addLayout(divider_row);
    }

    // ---- Mode cards ----
    auto* cards_row = new QHBoxLayout();
    cards_row->setSpacing(16);
    auto* measured_card = buildModeCard(
        this, QStringLiteral("SetupCardMeasured"),
        QString::fromLatin1(kAccentGreen),
        QStringLiteral(":/assets/scansetup/measured.svg"),
        QStringLiteral("Measured ROI Scan"),
        QStringLiteral("Draw the roof from tape\nmeasurements on a grid"));
    connect(measured_card, &QPushButton::clicked, this, [this] {
        choice_ = Choice::NewMeasuredPlan;
        accept();
    });
    cards_row->addWidget(measured_card, 1);

    satellite_description_text_ =
        QStringLiteral("Plan on satellite imagery\nof the site");
    satellite_card_ = buildModeCard(
        this, QStringLiteral("SetupCardSatellite"),
        QString::fromLatin1(kAccentBlue),
        QStringLiteral(":/assets/scansetup/satellite.svg"),
        QStringLiteral("Satellite ROI Scan"), satellite_description_text_);
    satellite_title_ =
        satellite_card_->findChild<QLabel*>(QStringLiteral("SetupCardTitle"));
    satellite_description_ = satellite_card_->findChild<QLabel*>(
        QStringLiteral("SetupCardDescription"));
    connect(satellite_card_, &QPushButton::clicked, this, [this] {
        if (!imagery_reachable_) {
            return;
        }
        choice_ = Choice::NewSatellitePlan;
        accept();
    });
    cards_row->addWidget(satellite_card_, 1);
    root->addLayout(cards_row);

    // ---- Footer ----
    auto* footer_row = new QHBoxLayout();
    footer_row->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
    cancel->setObjectName("SetupCancel");
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setFixedHeight(40);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    footer_row->addWidget(cancel);
    root->addLayout(footer_row);
}

QWidget* ScanSetupDialog::buildPlanRow(const Job& job, QWidget* parent) {
    auto* row = new QPushButton(parent);
    row->setObjectName("SetupPlanRow");
    row->setCursor(Qt::PointingHandCursor);
    row->setFixedHeight(kPlanRowHeight);
    row->setFlat(true);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(12, 12, 16, 12);
    layout->setSpacing(12);

    // Mode icon in a tinted chip — the metadata dialog's icon-chip spec.
    const bool measured = job.isMeasured();
    const QString brand = measured ? QString::fromLatin1(kAccentGreen)
                                   : QString::fromLatin1(kAccentBlue);
    auto* chip = new QLabel(row);
    chip->setObjectName("SetupPlanChip");
    chip->setFixedSize(40, 40);
    chip->setAlignment(Qt::AlignCenter);
    chip->setStyleSheet(
        QStringLiteral("background-color: %1; border-radius: 10px;")
            .arg(measured ? QStringLiteral("rgba(0, 188, 125, 0.20)")
                          : QStringLiteral("rgba(43, 127, 255, 0.10)")));
    chip->setPixmap(tintedSvg(
        measured ? QStringLiteral(":/assets/scansetup/measured.svg")
                 : QStringLiteral(":/assets/scansetup/satellite.svg"),
        22, 22, brand));
    layout->addWidget(chip, 0, Qt::AlignVCenter);

    auto* text_column = new QVBoxLayout();
    text_column->setSpacing(2);
    auto* name = new QLabel(job.name.isEmpty() ? job.id : job.name, row);
    name->setObjectName("SetupPlanName");
    text_column->addWidget(name);
    QString detail_text =
        job.address.isEmpty()
            ? (measured ? QStringLiteral("Measured plan")
                        : QStringLiteral("Satellite plan"))
            : job.address;
    // Imagery provenance travels with the plan (schema 3): a plan saved last
    // month may have been drawn on a flight from years earlier, because World
    // Imagery serves different LODs from different captures. Surface it here
    // so the operator sees it at the moment they pick the plan.
    if (!measured && job.hasImageryProvenance()) {
        const int age = int(job.imagery_captured.daysTo(QDate::currentDate()) /
                            365.25);
        detail_text += QStringLiteral("  ·  imagery %1")
                           .arg(job.imagery_captured.toString(Qt::ISODate));
        if (age >= 3) {
            detail_text += QStringLiteral(" (%1 yr old)").arg(age);
        }
    }
    auto* detail = new QLabel(detail_text, row);
    detail->setObjectName("SetupPlanDetail");
    text_column->addWidget(detail);
    layout->addLayout(text_column, 1);

    auto* status = new QLabel(
        job.executed()
            ? QStringLiteral("LAST RUN %1")
                  .arg(job.last_executed_at.toString(QStringLiteral("MMM d")))
            : QStringLiteral("PLANNED"),
        row);
    status->setObjectName(job.executed() ? "SetupPlanStatusRun"
                                         : "SetupPlanStatusPlanned");
    layout->addWidget(status, 0, Qt::AlignVCenter);

    connect(row, &QPushButton::clicked, this, [this, job] {
        choice_ = Choice::ExistingPlan;
        selected_job_ = job;
        accept();
    });
    return row;
}

QPushButton* ScanSetupDialog::buildModeCard(
    QWidget* parent, const QString& object_name, const QString& brand_color,
    const QString& icon_resource, const QString& title,
    const QString& description) {
    // Dashboard makeActionButton construction, dark-adapted (hover uses a
    // white wash instead of the light theme's black wash).
    auto* card = new QPushButton(parent);
    card->setObjectName(object_name);
    card->setCursor(Qt::PointingHandCursor);
    card->setFlat(true);
    card->setFixedHeight(kModeCardHeight);
    card->setStyleSheet(QStringLiteral(
        "#%1 {"
        "  background: transparent;"
        "  border: 2px solid %2;"
        "  border-radius: 10px;"
        "  text-align: left;"
        "}"
        "#%1:hover:enabled { background: rgba(255,255,255,0.03); }"
        "#%1:focus { outline: none; }")
        .arg(object_name, brand_color));

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(10);
    layout->setAlignment(Qt::AlignCenter);

    auto* icon = new QLabel(card);
    icon->setFixedSize(40, 40);
    icon->setScaledContents(true);
    icon->setPixmap(tintedSvg(icon_resource, 40, 40, brand_color));
    layout->addWidget(icon, 0, Qt::AlignCenter);

    auto* title_label = new QLabel(title, card);
    title_label->setObjectName("SetupCardTitle");
    title_label->setStyleSheet(QStringLiteral(
        "font-family: 'Arimo'; font-weight: 700; font-size: 18px; "
        "line-height: 28px; color: %1; background: transparent;")
        .arg(brand_color));
    title_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(title_label, 0, Qt::AlignCenter);

    auto* description_label = new QLabel(description, card);
    description_label->setObjectName("SetupCardDescription");
    description_label->setStyleSheet(QStringLiteral(
        "font-family: 'Arimo'; font-size: 14px; line-height: 20px; "
        "color: %1; background: transparent;").arg(QLatin1String(kMuted)));
    description_label->setAlignment(Qt::AlignCenter);
    description_label->setWordWrap(true);
    layout->addWidget(description_label, 0, Qt::AlignCenter);

    return card;
}

void ScanSetupDialog::applyStyle() {
    // Zinc palette from mission_metadata_dialog.cpp — dark-only by design,
    // independent of the global theme toggle.
    setStyleSheet(QStringLiteral(R"(
        #ScanSetupDialog {
            background-color: #18181b;
            border: 1px solid #27272a;
            border-radius: 10px;
        }
        QLabel { background: transparent; }
        #SetupTitle {
            font-family: 'Arimo'; font-weight: 700; font-size: 20px;
            color: #FAFAFA;
        }
        #SetupSubtitle {
            font-family: 'Arimo'; font-size: 14px; color: #9F9FA9;
        }
        #SetupClose {
            background: transparent; border: none; border-radius: 4px;
        }
        #SetupClose:hover { background-color: rgba(255, 255, 255, 0.04); }
        #SetupSectionLabel {
            font-family: 'Arimo'; font-weight: 700; font-size: 11px;
            letter-spacing: 0.5px; color: #9F9FA9;
        }
        #SetupPlanRow {
            background-color: #27272a;
            border: 1px solid transparent;
            border-radius: 10px;
            text-align: left;
        }
        #SetupPlanRow:hover { border-color: #00BC7D; }
        #SetupPlanRow:focus { outline: none; }
        #SetupPlanScroll { background: transparent; }
        #SetupPlanName {
            font-family: 'Arimo'; font-weight: 600; font-size: 14px;
            color: #FAFAFA;
        }
        #SetupPlanDetail {
            font-family: 'Arimo'; font-size: 12px; color: #9F9FA9;
        }
        #SetupPlanStatusPlanned {
            font-family: 'Arimo'; font-weight: 700; font-size: 10px;
            letter-spacing: 0.5px; color: #00BC7D;
        }
        #SetupPlanStatusRun {
            font-family: 'Arimo'; font-weight: 700; font-size: 10px;
            letter-spacing: 0.5px; color: #9F9FA9;
        }
        #SetupDividerLine { background-color: #27272a; border: none; }
        #SetupDividerLabel {
            font-family: 'Arimo'; font-size: 12px; color: #9F9FA9;
        }
        #SetupCancel {
            font-family: 'Arimo'; font-weight: 600; font-size: 14px;
            color: #FAFAFA;
            background-color: #3f3f47;
            border: none; border-radius: 10px;
            padding: 0 24px;
        }
        #SetupCancel:hover { background-color: #4a4a52; }
    )"));
}

}  // namespace f2c_cpp
