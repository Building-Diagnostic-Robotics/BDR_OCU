/**
 * @file mission_metadata_dialog.cpp
 * @brief Implementation of the "New Scan Information" modal.
 *
 * Visual reference: Figma node 194:152. All colors and dimensions below are
 * lifted directly from the Figma design context — keep them in sync if the
 * design changes. Note that this dialog uses a fixed dark palette regardless
 * of the global dark/light toggle: the design ships dark-only.
 */

#include "components/mission_metadata_dialog.hpp"

#include "settings_constants.hpp"
#include "units_system.hpp"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShowEvent>
#include <QSize>
#include <QString>
#include <QStyle>
#include <QVBoxLayout>

namespace f2c_cpp {

namespace {

constexpr int kDialogFixedWidth = 580;
constexpr int kBuildingSlugMaxLen = 64;
constexpr int kBuildingInputMaxLen = 80;
constexpr int kOperatorInputMaxLen = 80;

/**
 * Helper to build a 16-icon + label pair for a field header. Returns the
 * QWidget that should be added to the parent layout.
 */
QWidget* makeFieldLabel(const QString& iconResource,
                        const QString& text,
                        QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setObjectName("FieldLabelRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* icon = new QLabel(row);
    icon->setObjectName("FieldLabelIcon");
    icon->setFixedSize(16, 16);
    icon->setPixmap(QIcon(iconResource).pixmap(16, 16));
    layout->addWidget(icon, 0, Qt::AlignVCenter);

    auto* label = new QLabel(text, row);
    label->setObjectName("FieldLabelText");
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    return row;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

MissionMetadataDialog::MissionMetadataDialog(QWidget* parent) : QDialog(parent) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowModality(Qt::ApplicationModal);
    setFixedWidth(kDialogFixedWidth);

    buildUi();
    applyStyle();
    loadDefaults();
    refreshUnitToggleVisuals();
    refreshInfoBanner();
    refreshProceedEnabled();
}

void MissionMetadataDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (edit_building_) {
        edit_building_->setFocus();
        // Move cursor to end so an operator with a pre-filled name can
        // start typing additions without selecting first.
        edit_building_->setCursorPosition(edit_building_->text().length());
    }
}

// ---------------------------------------------------------------------------
// Public accessors
// ---------------------------------------------------------------------------

QString MissionMetadataDialog::buildingName() const {
    return edit_building_ ? edit_building_->text().trimmed() : QString();
}

QString MissionMetadataDialog::buildingSlug() const {
    return slugify(buildingName());
}

QString MissionMetadataDialog::operatorName() const {
    return edit_operator_ ? edit_operator_->text().trimmed() : QString();
}

QString MissionMetadataDialog::slugify(const QString& raw) {
    QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    // Replace any run of disallowed characters with a single underscore.
    static const QRegularExpression kDisallowed(QStringLiteral("[^A-Za-z0-9._-]+"));
    trimmed.replace(kDisallowed, QStringLiteral("_"));
    // Strip leading/trailing separators so we never produce names like
    // "_Acme_HQ_" or ".Acme" (the latter would be hidden on most FSes).
    while (!trimmed.isEmpty() &&
           (trimmed.front() == QLatin1Char('_') || trimmed.front() == QLatin1Char('.'))) {
        trimmed.remove(0, 1);
    }
    while (!trimmed.isEmpty() &&
           (trimmed.back() == QLatin1Char('_') || trimmed.back() == QLatin1Char('.'))) {
        trimmed.chop(1);
    }
    if (trimmed.length() > kBuildingSlugMaxLen) {
        trimmed.truncate(kBuildingSlugMaxLen);
        // Re-strip in case truncation landed on a separator.
        while (!trimmed.isEmpty() &&
               (trimmed.back() == QLatin1Char('_') || trimmed.back() == QLatin1Char('.'))) {
            trimmed.chop(1);
        }
    }
    return trimmed;
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void MissionMetadataDialog::buildUi() {
    auto* container = new QWidget(this);
    container->setObjectName("MetadataContainer");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(container);

    auto* outer = new QVBoxLayout(container);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ── Header (81 px high, dark grey, bottom border) ───────────────────────
    auto* header = new QWidget(container);
    header->setObjectName("MetadataHeader");
    header->setFixedHeight(81);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(24, 16, 24, 17);
    headerLayout->setSpacing(12);

    auto* headerBadge = new QLabel(header);
    headerBadge->setObjectName("HeaderBadge");
    headerBadge->setFixedSize(40, 40);
    headerBadge->setAlignment(Qt::AlignCenter);
    headerBadge->setPixmap(QIcon(QStringLiteral(":/assets/dialog/location_pin.svg"))
                               .pixmap(24, 24));
    headerLayout->addWidget(headerBadge, 0, Qt::AlignVCenter);

    auto* titleColumn = new QWidget(header);
    auto* titleLayout = new QVBoxLayout(titleColumn);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(0);

    lbl_header_title_ = new QLabel(tr("New Scan Information"), titleColumn);
    lbl_header_title_->setObjectName("HeaderTitle");
    titleLayout->addWidget(lbl_header_title_);

    lbl_header_subtitle_ = new QLabel(tr("Enter details before starting the scan"),
                                      titleColumn);
    lbl_header_subtitle_->setObjectName("HeaderSubtitle");
    titleLayout->addWidget(lbl_header_subtitle_);

    headerLayout->addWidget(titleColumn, 1, Qt::AlignVCenter);

    btn_close_ = new QPushButton(header);
    btn_close_->setObjectName("HeaderCloseButton");
    btn_close_->setCursor(Qt::PointingHandCursor);
    btn_close_->setFlat(true);
    btn_close_->setFixedSize(20, 20);
    btn_close_->setIcon(QIcon(QStringLiteral(":/assets/dialog/close.svg")));
    btn_close_->setIconSize(QSize(20, 20));
    btn_close_->setToolTip(tr("Close"));
    connect(btn_close_, &QPushButton::clicked, this,
            &MissionMetadataDialog::onCloseClicked);
    headerLayout->addWidget(btn_close_, 0, Qt::AlignVCenter);

    outer->addWidget(header);

    // ── Body (24 px padding, 20 px gap between groups) ──────────────────────
    auto* body = new QWidget(container);
    body->setObjectName("MetadataBody");
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(24, 24, 24, 24);
    bodyLayout->setSpacing(20);

    // Building Name field group
    auto* buildingGroup = new QWidget(body);
    auto* buildingLayout = new QVBoxLayout(buildingGroup);
    buildingLayout->setContentsMargins(0, 0, 0, 0);
    buildingLayout->setSpacing(8);

    buildingLayout->addWidget(makeFieldLabel(
        QStringLiteral(":/assets/dialog/location_pin.svg"),
        tr("Building Name"),
        buildingGroup));

    edit_building_ = new QLineEdit(buildingGroup);
    edit_building_->setObjectName("FieldInput");
    edit_building_->setPlaceholderText(tr("Enter building or site name"));
    edit_building_->setMaxLength(kBuildingInputMaxLen);
    edit_building_->setFixedHeight(46);
    connect(edit_building_, &QLineEdit::textChanged, this,
            &MissionMetadataDialog::onAnyTextChanged);
    buildingLayout->addWidget(edit_building_);

    lbl_slug_preview_ = new QLabel(buildingGroup);
    lbl_slug_preview_->setObjectName("SlugPreview");
    lbl_slug_preview_->setWordWrap(true);
    lbl_slug_preview_->setVisible(false);
    buildingLayout->addWidget(lbl_slug_preview_);

    bodyLayout->addWidget(buildingGroup);

    // Operator Name field group
    auto* operatorGroup = new QWidget(body);
    auto* operatorLayout = new QVBoxLayout(operatorGroup);
    operatorLayout->setContentsMargins(0, 0, 0, 0);
    operatorLayout->setSpacing(8);

    operatorLayout->addWidget(makeFieldLabel(
        QStringLiteral(":/assets/dialog/user.svg"),
        tr("Operator Name"),
        operatorGroup));

    edit_operator_ = new QLineEdit(operatorGroup);
    edit_operator_->setObjectName("FieldInput");
    edit_operator_->setPlaceholderText(tr("Enter operator name"));
    edit_operator_->setMaxLength(kOperatorInputMaxLen);
    edit_operator_->setFixedHeight(46);
    connect(edit_operator_, &QLineEdit::textChanged, this,
            &MissionMetadataDialog::onAnyTextChanged);
    operatorLayout->addWidget(edit_operator_);

    bodyLayout->addWidget(operatorGroup);

    // Unit System group (segmented toggle)
    auto* unitGroup = new QWidget(body);
    auto* unitLayout = new QVBoxLayout(unitGroup);
    unitLayout->setContentsMargins(0, 0, 0, 0);
    unitLayout->setSpacing(12);

    unitLayout->addWidget(makeFieldLabel(
        QStringLiteral(":/assets/dialog/ruler.svg"),
        tr("Unit System"),
        unitGroup));

    auto* segmented = new QWidget(unitGroup);
    segmented->setObjectName("UnitSegmented");
    segmented->setFixedHeight(64);
    auto* segLayout = new QHBoxLayout(segmented);
    segLayout->setContentsMargins(4, 4, 4, 4);
    segLayout->setSpacing(0);

    auto buildSegmentButton = [](const QString& primary,
                                  const QString& secondary,
                                  const QString& objectName,
                                  QWidget* parent) {
        auto* btn = new QPushButton(parent);
        btn->setObjectName(objectName);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setCheckable(true);
        btn->setFlat(true);
        btn->setFixedHeight(56);

        auto* col = new QVBoxLayout(btn);
        col->setContentsMargins(0, 8, 0, 8);
        col->setSpacing(2);

        auto* primaryLbl = new QLabel(primary, btn);
        primaryLbl->setObjectName("UnitPrimary");
        primaryLbl->setAlignment(Qt::AlignCenter);
        col->addWidget(primaryLbl);

        auto* secondaryLbl = new QLabel(secondary, btn);
        secondaryLbl->setObjectName("UnitSecondary");
        secondaryLbl->setAlignment(Qt::AlignCenter);
        col->addWidget(secondaryLbl);

        return btn;
    };

    btn_unit_metric_ = buildSegmentButton(tr("Metric"), tr("meters (m)"),
                                          QStringLiteral("UnitButtonMetric"),
                                          segmented);
    btn_unit_ansi_ = buildSegmentButton(tr("ANSI"), tr("feet (ft)"),
                                        QStringLiteral("UnitButtonAnsi"),
                                        segmented);
    connect(btn_unit_metric_, &QPushButton::clicked, this,
            &MissionMetadataDialog::onMetricClicked);
    connect(btn_unit_ansi_, &QPushButton::clicked, this,
            &MissionMetadataDialog::onAnsiClicked);

    segLayout->addWidget(btn_unit_metric_, 1);
    segLayout->addWidget(btn_unit_ansi_, 1);

    unitLayout->addWidget(segmented);

    bodyLayout->addWidget(unitGroup);

    // Info banner — copy flips with selection.
    lbl_info_banner_ = new QLabel(body);
    lbl_info_banner_->setObjectName("InfoBanner");
    lbl_info_banner_->setWordWrap(true);
    lbl_info_banner_->setTextFormat(Qt::RichText);
    bodyLayout->addWidget(lbl_info_banner_);

    bodyLayout->addStretch(1);

    outer->addWidget(body);

    // ── Footer (75 px high, dark grey, top border) ──────────────────────────
    auto* footer = new QWidget(container);
    footer->setObjectName("MetadataFooter");
    footer->setFixedHeight(75);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(24, 17, 24, 16);
    footerLayout->setSpacing(12);

    btn_cancel_ = new QPushButton(tr("Cancel"), footer);
    btn_cancel_->setObjectName("CancelButton");
    btn_cancel_->setCursor(Qt::PointingHandCursor);
    btn_cancel_->setFixedHeight(42);
    btn_cancel_->setMinimumWidth(96);
    connect(btn_cancel_, &QPushButton::clicked, this,
            &MissionMetadataDialog::onCancelClicked);
    footerLayout->addWidget(btn_cancel_, 0, Qt::AlignLeft | Qt::AlignVCenter);

    footerLayout->addStretch(1);

    btn_proceed_ = new QPushButton(tr("Proceed to Scan"), footer);
    btn_proceed_->setObjectName("ProceedButton");
    btn_proceed_->setCursor(Qt::PointingHandCursor);
    btn_proceed_->setFixedHeight(40);
    btn_proceed_->setMinimumWidth(160);
    btn_proceed_->setEnabled(false);
    connect(btn_proceed_, &QPushButton::clicked, this,
            &MissionMetadataDialog::onProceedClicked);
    footerLayout->addWidget(btn_proceed_, 0, Qt::AlignRight | Qt::AlignVCenter);

    outer->addWidget(footer);
}

void MissionMetadataDialog::applyStyle() {
    setStyleSheet(R"(
        #MetadataContainer {
            background-color: #18181b;
            border: 1px solid #3f3f47;
            border-radius: 10px;
        }
        #MetadataHeader {
            background-color: #27272a;
            border-bottom: 1px solid #3f3f47;
            border-top-left-radius: 10px;
            border-top-right-radius: 10px;
        }
        #HeaderBadge {
            background-color: rgba(0, 188, 125, 0.20);
            border-radius: 10px;
        }
        QLabel#HeaderTitle {
            color: #ffffff;
            font-size: 20px;
            font-weight: 700;
        }
        QLabel#HeaderSubtitle {
            color: #9f9fa9;
            font-size: 14px;
        }
        QPushButton#HeaderCloseButton {
            background: transparent;
            border: none;
            padding: 0;
        }
        QPushButton#HeaderCloseButton:hover {
            background-color: rgba(255, 255, 255, 0.04);
            border-radius: 4px;
        }
        #MetadataBody {
            background-color: #18181b;
        }
        QLabel#FieldLabelText {
            color: #d4d4d8;
            font-size: 14px;
            font-weight: 700;
        }
        QLineEdit#FieldInput {
            background-color: #27272a;
            border: 1px solid #3f3f47;
            border-radius: 10px;
            padding: 10px 16px;
            color: #f4f4f5;
            font-size: 16px;
            selection-background-color: rgba(0, 188, 125, 0.30);
        }
        QLineEdit#FieldInput:focus {
            border: 1px solid #00bc7d;
        }
        QLineEdit#FieldInput::placeholder {
            color: #52525c;
        }
        QLabel#SlugPreview {
            color: #71717b;
            font-size: 12px;
            padding-left: 4px;
        }
        #UnitSegmented {
            background-color: #27272a;
            border-radius: 10px;
        }
        QPushButton#UnitButtonMetric, QPushButton#UnitButtonAnsi {
            background: transparent;
            border: none;
            padding: 0;
            border-radius: 8px;
        }
        QPushButton#UnitButtonMetric:checked, QPushButton#UnitButtonAnsi:checked {
            background-color: #18181b;
        }
        QLabel#UnitPrimary {
            color: #9f9fa9;
            font-size: 14px;
        }
        QLabel#UnitSecondary {
            color: #71717b;
            font-size: 12px;
        }
        QPushButton#UnitButtonMetric:checked QLabel#UnitPrimary,
        QPushButton#UnitButtonAnsi:checked QLabel#UnitPrimary {
            color: #00bc7d;
        }
        #InfoBanner {
            background-color: rgba(43, 127, 255, 0.10);
            border: 1px solid rgba(43, 127, 255, 0.20);
            border-radius: 10px;
            color: #51a2ff;
            font-size: 12px;
            padding: 12px 17px;
        }
        #MetadataFooter {
            background-color: #27272a;
            border-top: 1px solid #3f3f47;
            border-bottom-left-radius: 10px;
            border-bottom-right-radius: 10px;
        }
        QPushButton#CancelButton {
            background-color: #3f3f47;
            border: 1px solid #52525c;
            border-radius: 10px;
            color: #ffffff;
            font-size: 14px;
            padding: 0 18px;
        }
        QPushButton#CancelButton:hover {
            background-color: #4a4a52;
        }
        QPushButton#ProceedButton {
            background-color: #00bc7d;
            border: none;
            border-radius: 10px;
            color: #ffffff;
            font-size: 14px;
            font-weight: 700;
            padding: 0 22px;
        }
        QPushButton#ProceedButton:hover {
            background-color: #00a86d;
        }
        QPushButton#ProceedButton:disabled {
            background-color: #3f3f47;
            color: #71717b;
        }
    )");
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

void MissionMetadataDialog::loadDefaults() {
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    if (edit_building_) {
        edit_building_->setText(settings.value(kSettingsBuildingNameKey).toString());
    }
    if (edit_operator_) {
        edit_operator_->setText(settings.value(kSettingsOperatorNameKey).toString());
    }
    selected_units_ = units::fromString(
        settings.value(kSettingsUnitsKey, units::toString(Units::Metric)).toString());
}

void MissionMetadataDialog::refreshUnitToggleVisuals() {
    if (btn_unit_metric_) {
        btn_unit_metric_->setChecked(selected_units_ == Units::Metric);
    }
    if (btn_unit_ansi_) {
        btn_unit_ansi_->setChecked(selected_units_ == Units::Ansi);
    }
    // QPushButton's :checked QLabel descendant pseudo-state isn't always
    // re-evaluated on setChecked alone — kick the labels by re-polishing.
    if (btn_unit_metric_) {
        btn_unit_metric_->style()->unpolish(btn_unit_metric_);
        btn_unit_metric_->style()->polish(btn_unit_metric_);
    }
    if (btn_unit_ansi_) {
        btn_unit_ansi_->style()->unpolish(btn_unit_ansi_);
        btn_unit_ansi_->style()->polish(btn_unit_ansi_);
    }
    refreshInfoBanner();
}

void MissionMetadataDialog::refreshInfoBanner() {
    if (!lbl_info_banner_) return;
    const QString unitWord = selected_units_ == Units::Metric
                                 ? tr("meters")
                                 : tr("feet");
    lbl_info_banner_->setText(
        tr("All distance measurements will be displayed in <b>%1</b> "
           "throughout the scan process.")
            .arg(unitWord));
}

void MissionMetadataDialog::refreshProceedEnabled() {
    const bool nameOk = !buildingSlug().isEmpty();
    const bool operatorOk = !operatorName().isEmpty();
    if (btn_proceed_) {
        btn_proceed_->setEnabled(nameOk && operatorOk);
    }
    if (lbl_slug_preview_) {
        const QString slug = buildingSlug();
        const QString raw = buildingName();
        if (raw.isEmpty()) {
            lbl_slug_preview_->setVisible(false);
        } else if (slug.isEmpty()) {
            lbl_slug_preview_->setVisible(true);
            lbl_slug_preview_->setText(
                tr("Building name must contain at least one letter or number."));
            lbl_slug_preview_->setStyleSheet(QStringLiteral("color: #ff6b6b;"));
        } else {
            lbl_slug_preview_->setVisible(true);
            lbl_slug_preview_->setText(tr("Will be saved as: %1").arg(slug));
            lbl_slug_preview_->setStyleSheet(QString());
        }
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void MissionMetadataDialog::onMetricClicked() {
    selected_units_ = Units::Metric;
    refreshUnitToggleVisuals();
}

void MissionMetadataDialog::onAnsiClicked() {
    selected_units_ = Units::Ansi;
    refreshUnitToggleVisuals();
}

void MissionMetadataDialog::onAnyTextChanged() {
    refreshProceedEnabled();
}

void MissionMetadataDialog::onProceedClicked() {
    if (!btn_proceed_ || !btn_proceed_->isEnabled()) {
        return;
    }
    {
        QSettings settings(kSettingsOrgName, kSettingsAppName);
        settings.setValue(kSettingsBuildingNameKey, buildingName());
        settings.setValue(kSettingsOperatorNameKey, operatorName());
        // setUnits() also writes to QSettings, but we set it explicitly here
        // so the persisted form is always written even if the value didn't
        // change (defensive; QSettings is cheap).
        settings.setValue(kSettingsUnitsKey, units::toString(selected_units_));
    }
    UnitsProvider::instance()->setUnits(selected_units_);
    accept();
}

void MissionMetadataDialog::onCancelClicked() {
    reject();
}

void MissionMetadataDialog::onCloseClicked() {
    reject();
}

}  // namespace f2c_cpp
