/**
 * @file scan_setup_dialog.cpp
 * @brief Implementation of the Start New Scan mode/plan selector.
 *
 * Visual vocabulary lifted from mission_metadata_dialog.cpp (zinc palette,
 * sourced from ui_theme_constants.hpp) and dashboard_screen.cpp's
 * makeActionButton (2px brand border cards, tinted stroke SVG icons,
 * Arimo 700 18 / 400 14).
 */

#include "components/scan_setup_dialog.hpp"

#include "components/bdr_message_box.hpp"
#include "imagery_reachability_probe.hpp"
#include "ui_theme_constants.hpp"

#include <QDate>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QVBoxLayout>

#include <algorithm>

namespace f2c_cpp {

namespace {

constexpr int kDialogFixedWidth = 580;
constexpr int kPlanRowHeight = 64;
constexpr int kPlanListMaxVisible = 4;
constexpr int kModeCardHeight = 136;
constexpr int kPageChoose = 0;
constexpr int kPageSaved = 1;
constexpr int kPageNew = 2;

/** Reports the current page, so the dialog shrinks when the operator leaves
 *  the plan list. QStackedWidget's own hint is the tallest page. */
class CurrentPageStack : public QStackedWidget {
public:
    using QStackedWidget::QStackedWidget;

    QSize sizeHint() const override {
        if (QWidget* page = currentWidget()) {
            return page->sizeHint();
        }
        return QStackedWidget::sizeHint();
    }
    QSize minimumSizeHint() const override {
        if (QWidget* page = currentWidget()) {
            return page->minimumSizeHint();
        }
        return QStackedWidget::minimumSizeHint();
    }
};

// Brand hues for the two plan modes. These are borders, icon tints and chip
// fills, so they stay vivid in both themes. `brandText` is the variant to use
// when the same hue has to carry a label directly on a surface — neither hue
// is legible as text on white.
constexpr const char* kAccentGreen = "#00BC7D";
constexpr const char* kAccentBlue = "#2B7FFF";

QString brandText(const QString& brand) {
    const UiThemeTokens t = appThemeTokens();
    if (brand == QLatin1String(kAccentBlue)) {
        return t.info;
    }
    if (brand == QLatin1String(kAccentGreen)) {
        return t.accent_text;
    }
    return t.muted;
}

QString chipFill(const QString& brand) {
    if (brand == QLatin1String(kAccentBlue)) {
        return QStringLiteral("rgba(43, 127, 255, 0.15)");
    }
    if (brand == QLatin1String(kAccentGreen)) {
        return QStringLiteral("rgba(0, 188, 125, 0.15)");
    }
    return QStringLiteral("rgba(161, 161, 170, 0.15)");
}

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

ScanSetupDialog::ScanSetupDialog(const QVector<Job>& jobs, QWidget* parent,
                                 ImageryReachabilityProbe* imagery)
    : QDialog(parent) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setModal(true);
    setObjectName("ScanSetupDialog");
    setFixedWidth(kDialogFixedWidth);
    setAttribute(Qt::WA_StyledBackground, true);
    buildUi(jobs);
    applyStyle();
    imagery_ = imagery;
    applyImageryReachable(imagery && imagery->reachable());
    if (imagery) {
        connect(imagery, &ImageryReachabilityProbe::reachableChanged, this,
                &ScanSetupDialog::applyImageryReachable);
    }
}

bool ScanSetupDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        if (auto* button = qobject_cast<QPushButton*>(watched);
            button && button->objectName() == QLatin1String("SetupPlanDelete")) {
            const char* key =
                event->type() == QEvent::Enter ? "iconHot" : "iconRest";
            button->setIcon(QIcon(button->property(key).value<QPixmap>()));
        }
    }
    return QDialog::eventFilter(watched, event);
}

// ---- Imagery reachability gate ---------------------------------------------

void ScanSetupDialog::applyImageryReachable(bool reachable) {
    if (!satellite_card_) {
        return;
    }
    imagery_reachable_ = reachable;
    satellite_card_->setEnabled(reachable);
    satellite_card_->setCursor(reachable ? Qt::PointingHandCursor
                                         : Qt::ForbiddenCursor);
    const UiThemeTokens t = appThemeTokens();
    const QString brand = reachable ? QString::fromLatin1(kAccentBlue)
                                    : t.muted;
    satellite_card_->setStyleSheet(QStringLiteral(
        "#SetupCardSatellite {"
        "  background: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 12px;"
        "}"
        "#SetupCardSatellite:hover:enabled { border-color: %3; }"
        "#SetupCardSatellite:focus { border-color: %3; outline: none; }")
        .arg(t.raised, t.raised_border, QString::fromLatin1(kAccentBlue)));
    satellite_title_->setStyleSheet(QStringLiteral(
        "font-family: 'Arimo'; font-weight: 700; font-size: 16px; "
        "letter-spacing: 1px; color: %1; background: transparent;")
        .arg(reachable ? brandText(QString::fromLatin1(kAccentBlue))
                       : t.muted));
    if (auto* icon = satellite_card_->findChild<QLabel*>(
            QStringLiteral("SetupCardIcon"))) {
        icon->setStyleSheet(QStringLiteral(
            "background-color: %1; border-radius: 28px;")
            .arg(chipFill(brand)));
        icon->setPixmap(tintedSvg(
            QStringLiteral(":/assets/scansetup/satellite.svg"), 28, 28, brand));
    }
    if (satellite_status_) {
        const bool checking = imagery_ && !imagery_->answered();
        satellite_status_->setVisible(!reachable);
        satellite_status_->setText(checking ? QStringLiteral("CHECKING")
                                            : QStringLiteral("OFFLINE"));
    }
    satellite_card_->setToolTip(
        reachable ? QString()
                  : QStringLiteral("Satellite imagery is unreachable. Plan "
                                   "satellite jobs from the office, where "
                                   "the site imagery can be downloaded."));
}

void ScanSetupDialog::buildUi(const QVector<Job>& jobs) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(20);

    // Arrow and X are the same width so the title stays centred when the
    // arrow is blank (first screen, or no saved plans).
    auto* header_row = new QHBoxLayout();
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(8);

    back_button_ = new QPushButton(this);
    back_button_->setObjectName("SetupBack");
    back_button_->setIconSize(QSize(20, 20));
    back_button_->setFixedSize(28, 28);
    back_button_->setCursor(Qt::PointingHandCursor);
    back_button_->setFocusPolicy(Qt::NoFocus);
    connect(back_button_, &QPushButton::clicked, this,
            [this] { showPage(kPageChoose); });
    header_row->addWidget(back_button_, 0, Qt::AlignVCenter);

    auto* title = new QLabel(QStringLiteral("START NEW SCAN"), this);
    title->setObjectName("SetupTitle");
    title->setAlignment(Qt::AlignCenter);
    title_ = title;
    header_row->addWidget(title, 1);

    const UiThemeTokens theme = appThemeTokens();
    auto* close_button = new QPushButton(this);
    close_button->setObjectName("SetupClose");
    close_button->setIcon(QIcon(tintedSvg(
        QStringLiteral(":/assets/dialog/close.svg"), 16, 16, theme.danger)));
    close_button->setIconSize(QSize(16, 16));
    close_button->setFixedSize(28, 28);
    close_button->setCursor(Qt::PointingHandCursor);
    close_button->setFocusPolicy(Qt::NoFocus);
    connect(close_button, &QPushButton::clicked, this, &QDialog::reject);
    header_row->addWidget(close_button, 0, Qt::AlignVCenter);
    root->addLayout(header_row);

    // ---- Saved plans: PLANNED (open) then COMPLETED (collapsed) ----
    // A plan is COMPLETED once its mission finalized with data on disk
    // (SatelliteScreen::markCurrentPlanCompleted). It stays openable — the
    // same roof scanned again — but is tucked away so the list the operator
    // scans on the roof is the work still to do.
    QVector<Job> planned;
    QVector<Job> completed;
    for (const Job& job : jobs) {
        (job.executed() ? completed : planned).append(job);
    }
    std::sort(planned.begin(), planned.end(), [](const Job& a, const Job& b) {
        return a.updated > b.updated;
    });
    std::sort(completed.begin(), completed.end(),
              [](const Job& a, const Job& b) {
                  return a.last_executed_at > b.last_executed_at;
              });

    pages_ = new CurrentPageStack(this);
    root->addWidget(pages_);

    auto* choose = new QWidget(pages_);
    auto* choose_row = new QHBoxLayout(choose);
    choose_row->setContentsMargins(0, 0, 0, 0);
    choose_row->setSpacing(16);
    auto* saved_card = buildModeCard(
        choose, QStringLiteral("SetupCardSaved"),
        QString::fromLatin1(kAccentBlue),
        QStringLiteral(":/assets/dashboard/plan_job.svg"),
        QStringLiteral("SAVED SCAN"));
    connect(saved_card, &QPushButton::clicked, this,
            [this] { showPage(kPageSaved); });
    choose_row->addWidget(saved_card, 1);
    auto* new_card = buildModeCard(
        choose, QStringLiteral("SetupCardNew"),
        QString::fromLatin1(kAccentGreen),
        QStringLiteral(":/assets/scansetup/new.svg"),
        QStringLiteral("NEW SCAN"));
    connect(new_card, &QPushButton::clicked, this,
            [this] { showPage(kPageNew); });
    choose_row->addWidget(new_card, 1);
    pages_->addWidget(choose);

    auto* saved = new QWidget(pages_);
    auto* saved_layout = new QVBoxLayout(saved);
    saved_layout->setContentsMargins(0, 0, 0, 0);
    saved_layout->setSpacing(16);
    planned_.completed = false;
    completed_.completed = true;
    buildPlanSection(planned_, QStringLiteral("SAVED PLANS"), planned, false,
                     saved_layout);
    buildPlanSection(completed_, QStringLiteral("COMPLETED"), completed, true,
                     saved_layout);
    pages_->addWidget(saved);
    refreshSectionChrome();

    auto* fresh = new QWidget(pages_);
    auto* cards_row = new QHBoxLayout(fresh);
    cards_row->setContentsMargins(0, 0, 0, 0);
    cards_row->setSpacing(16);
    auto* measured_card = buildModeCard(
        fresh, QStringLiteral("SetupCardMeasured"),
        QString::fromLatin1(kAccentGreen),
        QStringLiteral(":/assets/scansetup/measured.svg"),
        QStringLiteral("MEASURED ROI SCAN"));
    connect(measured_card, &QPushButton::clicked, this, [this] {
        choice_ = Choice::NewMeasuredPlan;
        accept();
    });
    cards_row->addWidget(measured_card, 1);

    satellite_card_ = buildModeCard(
        fresh, QStringLiteral("SetupCardSatellite"),
        QString::fromLatin1(kAccentBlue),
        QStringLiteral(":/assets/scansetup/satellite.svg"),
        QStringLiteral("SATELLITE ROI SCAN"));
    satellite_title_ =
        satellite_card_->findChild<QLabel*>(QStringLiteral("SetupCardTitle"));
    satellite_status_ = new QLabel(QStringLiteral("CHECKING"), satellite_card_);
    satellite_status_->setObjectName(QStringLiteral("SetupCardStatus"));
    satellite_status_->setAlignment(Qt::AlignCenter);
    if (auto* card_layout = qobject_cast<QVBoxLayout*>(satellite_card_->layout())) {
        card_layout->addWidget(satellite_status_, 0, Qt::AlignCenter);
    }
    connect(satellite_card_, &QPushButton::clicked, this, [this] {
        if (!imagery_reachable_) {
            return;
        }
        choice_ = Choice::NewSatellitePlan;
        accept();
    });
    cards_row->addWidget(satellite_card_, 1);
    pages_->addWidget(fresh);

    showPage(hasSavedPlans() ? kPageChoose : kPageNew);
}

void ScanSetupDialog::buildPlanSection(PlanSection& section,
                                       const QString& title,
                                       const QVector<Job>& jobs,
                                       bool collapsible, QVBoxLayout* root) {
    section.host = new QWidget(this);
    auto* host_layout = new QVBoxLayout(section.host);
    host_layout->setContentsMargins(0, 0, 0, 0);
    host_layout->setSpacing(12);

    if (collapsible) {
        // Disclosure header: the whole row is the toggle, chevron at the
        // right. Same shape as the Save Plan dialog's Advanced row.
        section.toggle = new QPushButton(section.host);
        section.toggle->setObjectName("SetupSectionToggle");
        section.toggle->setCheckable(true);
        section.toggle->setChecked(false);
        section.toggle->setCursor(Qt::PointingHandCursor);
        section.toggle->setFlat(true);
        section.toggle->setFixedHeight(24);
        auto* toggle_layout = new QHBoxLayout(section.toggle);
        toggle_layout->setContentsMargins(0, 0, 0, 0);
        toggle_layout->setSpacing(8);
        section.header = new QLabel(title, section.toggle);
        section.header->setObjectName("SetupSectionLabel");
        toggle_layout->addWidget(section.header, 0, Qt::AlignVCenter);
        auto* chevron = new QLabel(QStringLiteral("▸"), section.toggle);
        chevron->setObjectName("SetupSectionChevron");
        toggle_layout->addWidget(chevron, 0, Qt::AlignVCenter);
        toggle_layout->addStretch(1);
        host_layout->addWidget(section.toggle);
        connect(section.toggle, &QPushButton::toggled, this,
                [this, &section, chevron](bool open) {
                    chevron->setText(open ? QStringLiteral("▾")
                                          : QStringLiteral("▸"));
                    section.body->setVisible(open);
                    adjustSize();
                });
    } else {
        section.header = new QLabel(title, section.host);
        section.header->setObjectName("SetupSectionLabel");
        host_layout->addWidget(section.header);
    }

    auto* list_host = new QWidget(section.host);
    section.rows = new QVBoxLayout(list_host);
    section.rows->setContentsMargins(0, 0, 0, 0);
    section.rows->setSpacing(12);
    for (const Job& job : jobs) {
        section.rows->addWidget(buildPlanRow(job, list_host));
    }
    section.rows->addStretch(1);
    section.count = jobs.size();

    if (jobs.size() > kPlanListMaxVisible) {
        auto* scroll = new QScrollArea(section.host);
        scroll->setObjectName("SetupPlanScroll");
        scroll->setWidget(list_host);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFixedHeight(kPlanListMaxVisible * (kPlanRowHeight + 8));
        // The viewport paints the palette's Base brush (white) unless told
        // not to; the #SetupPlanScroll rule only reaches the frame.
        scroll->viewport()->setAutoFillBackground(false);
        section.body = scroll;
    } else {
        section.body = list_host;
    }
    host_layout->addWidget(section.body);
    if (collapsible) {
        section.body->setVisible(false);
    }
    root->addWidget(section.host);
}

void ScanSetupDialog::devSetCompletedOpen(bool open) {
    if (completed_.toggle) {
        completed_.toggle->setChecked(open);
    }
}

void ScanSetupDialog::refreshSectionChrome() {
    planned_.host->setVisible(planned_.count > 0);
    completed_.host->setVisible(completed_.count > 0);
    completed_.header->setText(
        QStringLiteral("COMPLETED (%1)").arg(completed_.count));
    adjustSize();
}

bool ScanSetupDialog::hasSavedPlans() const {
    return planned_.count + completed_.count > 0;
}

void ScanSetupDialog::showPage(int page) {
    if (!pages_ || !title_) {
        return;
    }
    pages_->setCurrentIndex(page);
    // QStackedLayout sizes to the tallest page. Ignored pages drop out
    // of that, so the dialog shrinks back to the page on screen.
    for (int i = 0; i < pages_->count(); ++i) {
        if (QWidget* page_widget = pages_->widget(i)) {
            page_widget->setSizePolicy(i == page
                ? QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred)
                : QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored));
        }
    }
    if (page == kPageSaved) {
        title_->setText(QStringLiteral("SAVED SCANS"));
    } else if (page == kPageNew) {
        title_->setText(QStringLiteral("NEW SCAN"));
    } else {
        title_->setText(QStringLiteral("START NEW SCAN"));
    }
    if (back_button_) {
        const bool show_back = page != kPageChoose && hasSavedPlans();
        back_button_->setEnabled(show_back);
        back_button_->setIcon(show_back
            ? QIcon(tintedSvg(QStringLiteral(":/assets/satellite/footer_back.svg"),
                              20, 20, QString::fromLatin1(kAccentBlue)))
            : QIcon());
    }
    pages_->updateGeometry();
    recenter();
}

void ScanSetupDialog::recenter() {
    adjustSize();
    QWidget* host = parentWidget();
    if (!host) {
        return;
    }
    move(host->mapToGlobal(QPoint((host->width() - width()) / 2,
                                  (host->height() - height()) / 2)));
}

void ScanSetupDialog::devShowSaved() { showPage(kPageSaved); }

void ScanSetupDialog::devShowNew() { showPage(kPageNew); }

void ScanSetupDialog::onDeletePlanClicked(const Job& job, QWidget* row) {
    const QString name = job.name.isEmpty() ? job.id : job.name;
    const QString detail =
        job.isMeasured()
            ? QStringLiteral("The plan is removed from this laptop.")
            : QStringLiteral("The plan and its cached site imagery are "
                             "removed from this laptop. Re-planning this "
                             "roof needs an internet connection.");
    const int picked = BdrMessageBox::custom(
        this, QStringLiteral("Delete plan"),
        QStringLiteral("Delete \u201c%1\u201d?").arg(name),
        {QStringLiteral("Cancel"), QStringLiteral("Delete")}, 0, detail);
    if (picked != 1) {
        return;
    }
    if (!JobStore().remove(job.id)) {
        BdrMessageBox::warning(
            this, QStringLiteral("Delete plan"),
            QStringLiteral("Could not delete \u201c%1\u201d.").arg(name));
        return;
    }
    PlanSection& section = job.executed() ? completed_ : planned_;
    section.rows->removeWidget(row);
    row->deleteLater();
    section.count = std::max(section.count - 1, 0);
    refreshSectionChrome();
    if (!hasSavedPlans()) {
        showPage(kPageNew);
    }
    emit planDeleted(job.id);
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
        QStringLiteral("background-color: %1; border-radius: 12px;")
            .arg(chipFill(brand)));
    chip->setPixmap(tintedSvg(
        measured ? QStringLiteral(":/assets/scansetup/measured.svg")
                 : QStringLiteral(":/assets/scansetup/satellite.svg"),
        22, 22, brand));
    layout->addWidget(chip, 0, Qt::AlignVCenter);

    auto* text_column = new QVBoxLayout();
    text_column->setSpacing(2);
    auto* name = new QLabel(
        (job.name.isEmpty() ? job.id : job.name).toUpper(), row);
    name->setObjectName("SetupPlanName");
    text_column->addWidget(name);
    QString detail_text =
        job.address.isEmpty()
            ? (measured ? QStringLiteral("MEASURED PLAN")
                        : QStringLiteral("SATELLITE PLAN"))
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

    // Trash: a child button swallows its own press, so clicking it never
    // reaches the row's "open this plan" handler.
    auto* trash = new QPushButton(row);
    trash->setObjectName("SetupPlanDelete");
    trash->setFixedSize(32, 32);
    trash->setCursor(Qt::PointingHandCursor);
    trash->setFlat(true);
    trash->setToolTip(QStringLiteral("Delete plan"));
    trash->setIconSize(QSize(18, 18));
    const UiThemeTokens t = appThemeTokens();
    const QString trash_svg = QStringLiteral(":/assets/scansetup/delete.svg");
    trash->setIcon(QIcon(tintedSvg(trash_svg, 18, 18, t.muted)));
    // Qt styles pick QIcon::Active on focus, not hover, so the red tint is
    // swapped in explicitly on enter/leave.
    trash->setProperty("iconRest", tintedSvg(trash_svg, 18, 18, t.muted));
    trash->setProperty("iconHot", tintedSvg(trash_svg, 18, 18, t.danger));
    trash->installEventFilter(this);
    layout->addWidget(trash, 0, Qt::AlignVCenter);
    connect(trash, &QPushButton::clicked, this,
            [this, job, row] { onDeletePlanClicked(job, row); });

    connect(row, &QPushButton::clicked, this, [this, job] {
        choice_ = Choice::ExistingPlan;
        selected_job_ = job;
        accept();
    });
    return row;
}

QPushButton* ScanSetupDialog::buildModeCard(
    QWidget* parent, const QString& object_name, const QString& brand_color,
    const QString& icon_resource, const QString& title) {
    // Dashboard makeActionButton construction, dark-adapted (hover uses a
    // white wash instead of the light theme's black wash).
    auto* card = new QPushButton(parent);
    card->setObjectName(object_name);
    card->setCursor(Qt::PointingHandCursor);
    card->setFlat(true);
    card->setFocusPolicy(Qt::NoFocus);
    card->setFixedHeight(kModeCardHeight);
    const UiThemeTokens t = appThemeTokens();
    card->setStyleSheet(QStringLiteral(
        "#%1 {"
        "  background: %2;"
        "  border: 1px solid %3;"
        "  border-radius: 12px;"
        "}"
        "#%1:hover:enabled { border-color: %4; }"
        "#%1:focus { border-color: %4; outline: none; }")
        .arg(object_name, t.raised, t.raised_border, brand_color));

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);
    layout->setAlignment(Qt::AlignCenter);

    auto* icon = new QLabel(card);
    icon->setObjectName(QStringLiteral("SetupCardIcon"));
    icon->setFixedSize(56, 56);
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet(QStringLiteral(
        "background-color: %1; border-radius: 28px;").arg(chipFill(brand_color)));
    icon->setPixmap(tintedSvg(icon_resource, 28, 28, brand_color));
    layout->addWidget(icon, 0, Qt::AlignCenter);

    auto* title_label = new QLabel(title, card);
    title_label->setObjectName("SetupCardTitle");
    title_label->setStyleSheet(QStringLiteral(
        "font-family: 'Arimo'; font-weight: 700; font-size: 16px; "
        "letter-spacing: 1px; color: %1; background: transparent;")
        .arg(brandText(brand_color)));
    title_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(title_label, 0, Qt::AlignCenter);
    return card;
}

void ScanSetupDialog::applyStyle() {
    const UiThemeTokens t = appThemeTokens();
    setStyleSheet(QStringLiteral(R"(
        #ScanSetupDialog {
            background-color: %1;
            border: 1px solid %2;
            border-radius: 16px;
        }
        QLabel { background: transparent; }
        #SetupTitle {
            font-family: 'Arimo'; font-weight: 600; font-size: 18px;
            letter-spacing: 1px; color: %3;
        }
        #SetupBack, #SetupBack:disabled {
            background: transparent; border: none; border-radius: 4px;
        }
        #SetupBack:hover:enabled { background-color: %5; }
        #SetupClose {
            background: transparent; border: none; border-radius: 4px;
        }
        #SetupClose:hover { background-color: rgba(239, 68, 68, 0.12); }
        #SetupSectionLabel {
            font-family: 'Arimo'; font-weight: 700; font-size: 11px;
            letter-spacing: 0.5px; color: %4;
        }
        #SetupPlanRow {
            background-color: %6;
            border: 1px solid %8;
            border-radius: 12px;
            text-align: left;
        }
        #SetupPlanRow:hover { background-color: %5; }
        #SetupPlanRow:hover #SetupPlanStatusRun { color: %3; }
        #SetupPlanRow:focus { outline: none; }
        #SetupPlanDelete {
            background: transparent; border: none; border-radius: 6px;
        }
        #SetupPlanDelete:hover { background-color: rgba(239, 68, 68, 0.12); }
        #SetupPlanDelete:focus { outline: none; }
        #SetupSectionToggle {
            background: transparent; border: none; text-align: left;
        }
        #SetupSectionToggle:focus { outline: none; }
        #SetupSectionChevron {
            font-family: 'Arimo'; font-size: 12px; color: %4;
        }
        #SetupPlanScroll, #SetupPlanScroll > QWidget > QWidget { background: transparent; }
        #SetupPlanScroll QScrollBar:vertical {
            background: transparent; width: 6px; margin: 0;
        }
        #SetupPlanScroll QScrollBar::handle:vertical {
            background: %8; border-radius: 3px; min-height: 24px;
        }
        #SetupPlanScroll QScrollBar::handle:vertical:hover { background: %4; }
        #SetupPlanScroll QScrollBar::add-line:vertical,
        #SetupPlanScroll QScrollBar::sub-line:vertical { height: 0; }
        #SetupPlanScroll QScrollBar::add-page:vertical,
        #SetupPlanScroll QScrollBar::sub-page:vertical { background: transparent; }
        #SetupPlanName {
            font-family: 'Arimo'; font-weight: 600; font-size: 14px;
            letter-spacing: 1px; color: %3;
        }
        #SetupPlanDetail {
            font-family: 'Arimo'; font-size: 12px; color: %4;
        }
        #SetupPlanStatusPlanned {
            font-family: 'Arimo'; font-weight: 700; font-size: 10px;
            letter-spacing: 0.5px; color: %9;
        }
        #SetupCardStatus {
            font-family: 'Arimo'; font-weight: 700; font-size: 10px;
            letter-spacing: 1px; color: %4;
        }
        #SetupPlanStatusRun {
            font-family: 'Arimo'; font-weight: 700; font-size: 10px;
            letter-spacing: 0.5px; color: %4;
        }
    )")
                      .arg(t.surface, t.surface_border, t.text, t.muted,
                           t.neutral_hover, t.raised, t.accent_green,
                           t.raised_border, t.accent_text));
}

}  // namespace f2c_cpp
