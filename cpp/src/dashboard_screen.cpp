#include "dashboard_screen.hpp"
#include "settings_constants.hpp"

#include <QColor>
#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include "components/tilt_calibration_dialog.hpp"
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QVariant>
#include <QVBoxLayout>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QSvgRenderer>
#else
#include <QtSvg/QSvgRenderer>
#endif

namespace f2c_cpp {

namespace {

QPixmap loadSvgPixmap(const QString& resourcePath, int w, int h,
                      const QString& strokeColor = QString()) {
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }
    QByteArray data = f.readAll();
    f.close();

    if (!strokeColor.isEmpty()) {
        int strokeStart = data.indexOf("stroke=\"");
        if (strokeStart >= 0) {
            int valueStart = strokeStart + 8;
            int valueEnd = data.indexOf('"', valueStart);
            if (valueEnd > valueStart) {
                QByteArray newColor = strokeColor.toUtf8();
                if (!newColor.startsWith('#')) {
                    newColor.prepend('#');
                }
                data = data.left(valueStart) + newColor + data.mid(valueEnd);
            }
        }
    }

    QSvgRenderer renderer(data);
    if (!renderer.isValid()) {
        return QPixmap();
    }
    QPixmap pix(w, h);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    renderer.render(&painter);
    return pix;
}

void applyDropShadow(QWidget* widget, int blurRadius, int yOffset, int alpha) {
    if (!widget) {
        return;
    }
    auto* effect = new QGraphicsDropShadowEffect(widget);
    effect->setBlurRadius(blurRadius);
    effect->setOffset(0, yOffset);
    effect->setColor(QColor(0, 0, 0, alpha));
    widget->setGraphicsEffect(effect);
}

QWidget* makeStatusCard(QWidget* parent,
                       const QString& objectName,
                       const QString& iconBgColor,
                       const QString& iconResourcePath,
                       const QString& iconColor,
                       const QString& labelText,
                       QLabel*& outValue) {
    auto* card = new QWidget(parent);
    card->setObjectName(objectName);
    card->setFixedHeight(94);
    auto* cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(24, 24, 24, 14);
    cardLayout->setSpacing(24);

    auto* iconBox = new QLabel(card);
    iconBox->setObjectName(objectName + "Icon");
    iconBox->setAlignment(Qt::AlignCenter);
    iconBox->setFixedSize(56, 56);
    iconBox->setStyleSheet(QString("background: %1; border-radius: 10px;").arg(iconBgColor));
    QPixmap iconPix = loadSvgPixmap(iconResourcePath, 32, 32, iconColor);
    if (!iconPix.isNull()) {
        iconBox->setPixmap(iconPix);
    }
    cardLayout->addWidget(iconBox, 0, Qt::AlignTop);

    auto* textCol = new QWidget(card);
    auto* textLayout = new QVBoxLayout(textCol);
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(4);
    textLayout->setAlignment(Qt::AlignTop);

    auto* label = new QLabel(labelText, textCol);
    label->setObjectName(objectName + "Label");
    label->setStyleSheet("font-size: 14px; line-height: 20px; color: #4A5565;");
    textLayout->addWidget(label);

    outValue = new QLabel(textCol);
    outValue->setObjectName(objectName + "Value");
    textLayout->addWidget(outValue);

    cardLayout->addWidget(textCol, 1, Qt::AlignTop);
    return card;
}

QPushButton* makeActionButton(QWidget* parent,
                              const QString& objectName,
                              const QString& borderColor,
                              const QString& titleColor,
                              const QString& iconResourcePath,
                              const QString& title,
                              const QString& description) {
    auto* btn = new QPushButton(parent);
    btn->setObjectName(objectName);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFlat(true);  // so native style doesn't paint over icon/title/desc
    btn->setFixedHeight(168);
    const QString selector = QString("#%1").arg(objectName);
    btn->setStyleSheet(QString(
        "%1 {"
        "  background: transparent;"
        "  border: 2px solid %2;"
        "  border-radius: 10px;"
        "  text-align: left;"
        "}"
        "%1:hover:enabled { background: rgba(0,0,0,0.03); }"
        "%1:focus { outline: none; }"
        ).arg(selector, borderColor));
    auto* layout = new QVBoxLayout(btn);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    layout->setAlignment(Qt::AlignCenter);

    auto* icon = new QLabel(btn);
    icon->setObjectName(objectName + "Icon");
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedSize(40, 40);
    icon->setScaledContents(true);
    QPixmap iconPix = loadSvgPixmap(iconResourcePath, 40, 40, titleColor);
    if (!iconPix.isNull()) {
        icon->setPixmap(iconPix);
    }
    layout->addWidget(icon, 0, Qt::AlignCenter);

    auto* titleLbl = new QLabel(title, btn);
    titleLbl->setObjectName(objectName + "Title");
    titleLbl->setStyleSheet(QString(
        "font-family: 'Arimo'; font-weight: 700; font-size: 18px; line-height: 28px; color: %1;")
        .arg(titleColor));
    titleLbl->setAlignment(Qt::AlignCenter);
    layout->addWidget(titleLbl, 0, Qt::AlignCenter);

    auto* descLbl = new QLabel(description, btn);
    descLbl->setObjectName(objectName + "Desc");
    descLbl->setStyleSheet(
        "font-family: 'Arimo'; font-size: 14px; line-height: 20px; color: #4A5565;");
    descLbl->setAlignment(Qt::AlignCenter);
    descLbl->setWordWrap(true);
    layout->addWidget(descLbl, 0, Qt::AlignCenter);

    return btn;
}

}  // namespace

DashboardScreen::DashboardScreen(QWidget* parent)
    : QWidget(parent) {
    setObjectName("DashboardRoot");
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addSpacing(45);

    // ── Header ─────────────────────────────────────────────────────────────
    header_ = new QWidget(this);
    header_->setObjectName("DashboardHeader");
    header_->setFixedHeight(127);
    auto* headerLayout = new QVBoxLayout(header_);
    headerLayout->setContentsMargins(40, 24, 40, 24);
    headerLayout->setSpacing(0);

    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(0);

    auto* titleCol = new QVBoxLayout();
    titleCol->setContentsMargins(0, 0, 0, 0);
    titleCol->setSpacing(4);
    lbl_title_ = new QLabel("Roofus Dashboard", header_);
    lbl_title_->setObjectName("DashboardTitle");
    titleCol->addWidget(lbl_title_);
    lbl_subtitle_ = new QLabel("Coverage Planning Suite", header_);
    lbl_subtitle_->setObjectName("DashboardSubtitle");
    titleCol->addWidget(lbl_subtitle_);
    headerRow->addLayout(titleCol, 1);
    headerRow->addStretch(1);

    btn_logout_ = new QPushButton(header_);
    btn_logout_->setObjectName("DashboardLogoutButton");
    btn_logout_->setFixedHeight(40);
    btn_logout_->setCursor(Qt::PointingHandCursor);
    QPixmap logoutPix = loadSvgPixmap(":/assets/dashboard/logout.svg", 20, 20);
    if (!logoutPix.isNull()) {
        btn_logout_->setIcon(QIcon(logoutPix));
        btn_logout_->setIconSize(QSize(20, 20));
    }
    btn_logout_->setText(" Logout");
    connect(btn_logout_, &QPushButton::clicked, this, &DashboardScreen::onLogoutClicked);
    headerRow->addWidget(btn_logout_, 0, Qt::AlignRight | Qt::AlignVCenter);

    headerLayout->addLayout(headerRow);
    root->addWidget(header_);

    // ── Content ────────────────────────────────────────────────────────────
    auto* content = new QWidget(this);
    content->setObjectName("DashboardContent");
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(32, 37, 32, 32);
    contentLayout->setSpacing(47);

    // Status cards row
    auto* cardsRow = new QHBoxLayout();
    cardsRow->setContentsMargins(0, 0, 0, 0);
    cardsRow->setSpacing(24);

    card_status_ = makeStatusCard(content, "DashboardCardStatus",
        "#D0FAE5", ":/assets/dashboard/heartbeat.svg", "#009966", "System Status", lbl_status_value_);
    lbl_status_value_->setText("READY");
    lbl_status_value_->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: #009966;");
    cardsRow->addWidget(card_status_, 1);

    card_scans_ = makeStatusCard(content, "DashboardCardScans",
        "#DBEAFE", ":/assets/dashboard/location_pin.svg", "#155DFC", "Total Scans", lbl_scans_value_);
    lbl_scans_value_->setText("1");
    lbl_scans_value_->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: #1E2939;");
    cardsRow->addWidget(card_scans_, 1);

    card_cameras_ = makeStatusCard(content, "DashboardCardCameras",
        "#F3E8FF", ":/assets/dashboard/battery.svg", "#9810FA", "Battery", lbl_cameras_value_);
    lbl_cameras_value_->setText("87%");
    lbl_cameras_value_->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: #9810FA;");
    cardsRow->addWidget(card_cameras_, 1);

    card_calibration_ = makeStatusCard(content, "DashboardCardCalibration",
        "#FEF3C6", ":/assets/dashboard/settings.svg", "#E17100", "Next Calibration", lbl_calibration_value_);
    lbl_calibration_value_->setText("2 scans");
    lbl_calibration_value_->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: #E17100;");
    cardsRow->addWidget(card_calibration_, 1);

    contentLayout->addLayout(cardsRow);

    // Quick Actions
    auto* actionsCard = new QWidget(this);
    actionsCard->setObjectName("DashboardActionsCard");
    auto* actionsLayout = new QVBoxLayout(actionsCard);
    actionsLayout->setContentsMargins(32, 32, 32, 32);
    actionsLayout->setSpacing(24);

    auto* actionsTitle = new QLabel("Quick Actions", actionsCard);
    actionsTitle->setObjectName("DashboardActionsTitle");
    actionsTitle->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: #1E2939;");
    actionsLayout->addWidget(actionsTitle);

    auto* actionsRow = new QHBoxLayout();
    actionsRow->setContentsMargins(0, 0, 0, 0);
    actionsRow->setSpacing(24);

    btn_start_scan_ = makeActionButton(actionsCard, "DashboardBtnStartScan", "#009966", "#009966",
        ":/assets/dashboard/location_pin.svg", "Start New Scan", "Begin a new coverage mapping session");
    connect(btn_start_scan_, &QPushButton::clicked, this, &DashboardScreen::onStartNewScanClicked);
    actionsRow->addWidget(btn_start_scan_, 1);

    btn_run_diagnostics_ = makeActionButton(actionsCard, "DashboardBtnDiagnostics", "#155DFC", "#155DFC",
        ":/assets/dashboard/heartbeat.svg", "Run Diagnostics", "Check system health and sensors");
    connect(btn_run_diagnostics_, &QPushButton::clicked, this, &DashboardScreen::onRunDiagnosticsClicked);
    actionsRow->addWidget(btn_run_diagnostics_, 1);

    btn_view_recordings_ = makeActionButton(actionsCard, "DashboardBtnRecordings", "#9810FA", "#9810FA",
        ":/assets/dashboard/camera.svg", "View Recordings", "Access previous scan data");
    connect(btn_view_recordings_, &QPushButton::clicked, this, &DashboardScreen::onViewRecordingsClicked);
    actionsRow->addWidget(btn_view_recordings_, 1);

    btn_calibrate_tilt_ = makeActionButton(actionsCard, "DashboardBtnCalibrateTilt", "#E17100", "#E17100",
        ":/assets/dashboard/settings.svg", "Calibrate Tilt", "Align LiDAR mount for odometry and maps");
    connect(btn_calibrate_tilt_, &QPushButton::clicked, this, &DashboardScreen::onCalibrateTiltRequested);
    actionsRow->addWidget(btn_calibrate_tilt_, 1);

    actionsLayout->addLayout(actionsRow);
    contentLayout->addWidget(actionsCard);

    // System Information
    auto* infoCard = new QWidget(this);
    infoCard->setObjectName("DashboardInfoCard");
    auto* infoLayout = new QVBoxLayout(infoCard);
    infoLayout->setContentsMargins(24, 24, 24, 24);
    infoLayout->setSpacing(8);

    auto* infoTitle = new QLabel("System Information", infoCard);
    infoTitle->setObjectName("DashboardInfoTitle");
    infoTitle->setStyleSheet(
        "font-family: 'Arimo'; font-weight: 700; font-size: 18px; line-height: 27px; color: #1C398E;");
    infoLayout->addWidget(infoTitle);

    auto* infoRow = new QHBoxLayout();
    infoRow->setContentsMargins(0, 0, 0, 0);
    infoRow->setSpacing(32);

    auto addInfoPair = [&infoRow](QWidget* parent, const QString& labelText,
                                  QLabel*& outValue) -> QWidget* {
        auto* block = new QWidget(parent);
        auto* v = new QVBoxLayout(block);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        auto* lbl = new QLabel(labelText, block);
        lbl->setStyleSheet(
            "font-family: 'Arimo'; font-weight: 700; font-size: 14px; line-height: 20px; color: #1447E6;");
        outValue = new QLabel(block);
        outValue->setStyleSheet(
            "font-family: 'Arimo'; font-size: 14px; line-height: 20px; color: #155DFC;");
        v->addWidget(lbl);
        v->addWidget(outValue);
        infoRow->addWidget(block);
        return block;
    };

    addInfoPair(infoCard, "Robot ID", lbl_robot_id_value_);
    addInfoPair(infoCard, "Firmware", lbl_firmware_value_);
    addInfoPair(infoCard, "Last Calibration", lbl_calibration_value_info_);
    addInfoPair(infoCard, "Uptime", lbl_battery_value_);

    lbl_robot_id_value_->setText("—");
    lbl_firmware_value_->setText("v2.3.1");
    lbl_calibration_value_info_->setText("Scan 0");
    lbl_battery_value_->setText("3h 24m");

    infoLayout->addLayout(infoRow);
    contentLayout->addWidget(infoCard);

    contentLayout->addStretch(1);
    root->addWidget(content, 1);

    applyDropShadow(header_, 8, 4, 25);
    applyDropShadow(card_status_, 6, 2, 25);
    applyDropShadow(card_scans_, 6, 2, 25);
    applyDropShadow(card_cameras_, 6, 2, 25);
    applyDropShadow(card_calibration_, 6, 2, 25);
    applyDropShadow(actionsCard, 6, 2, 25);

    applyStyle();
}

void DashboardScreen::setRobotId(const QString& robotId) {
    robot_id_ = robotId.trimmed();
    if (lbl_robot_id_value_) {
        lbl_robot_id_value_->setText(robot_id_.isEmpty() ? "—" : robot_id_.toUpper());
    }
}

void DashboardScreen::setDarkMode(bool dark_mode) {
    if (dark_mode_ == dark_mode) {
        return;
    }
    dark_mode_ = dark_mode;
    applyStyle();
}

void DashboardScreen::onLogoutClicked() {
    emit logoutRequested();
}

void DashboardScreen::onStartNewScanClicked() {
    emit startNewScanRequested();
}

void DashboardScreen::onRunDiagnosticsClicked() {
    emit runDiagnosticsRequested();
}

void DashboardScreen::onViewRecordingsClicked() {
    emit viewRecordingsRequested();
}

void DashboardScreen::onCalibrateTiltRequested() {
    TiltCalibrationDialog dlg(robotHostFromSettings(), this);
    dlg.exec();
}

QString DashboardScreen::robotHostFromSettings() const {
    QSettings settings(kSettingsOrgName, kSettingsAppName);
    const QString from_settings = settings.value("robot_ip", "").toString().trimmed();
    return from_settings.isEmpty() ? QStringLiteral("192.168.168.101") : from_settings;
}

void DashboardScreen::applyStyle() {
    setProperty("theme", QVariant(dark_mode_ ? QStringLiteral("dark") : QStringLiteral("light")));
    setStyleSheet(R"(
        #DashboardRoot {
            background-color: #FAFAFA;
            font-family: "Arimo";
        }
        #DashboardHeader {
            background: #FFFFFF;
            border-bottom: 1px solid #E4E4E7;
        }
        #DashboardTitle {
            font-weight: 700;
            font-size: 30px;
            line-height: 36px;
            color: #18181B;
        }
        #DashboardSubtitle {
            font-size: 16px;
            line-height: 24px;
            color: #71717B;
        }
        #DashboardLogoutButton {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 4px;
            padding: 0 16px;
            min-width: 113px;
            height: 40px;
            color: #71717B;
            font-size: 16px;
            line-height: 24px;
        }
        #DashboardLogoutButton:hover {
            background: #F4F4F5;
        }
        #DashboardLogoutButton:pressed {
            background: #E4E4E7;
        }

        #DashboardContent {
            background-color: #FAFAFA;
        }
        #DashboardCardStatus, #DashboardCardScans, #DashboardCardCameras, #DashboardCardCalibration {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 10px;
        }
        #DashboardActionsCard {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 10px;
        }
        #DashboardInfoCard {
            background: #FFFFFF;
            border: 1px solid #E4E4E7;
            border-radius: 10px;
        }

        /* Dark theme overrides */
        #DashboardRoot[theme="dark"] {
            background-color: #09090B;
        }
        #DashboardRoot[theme="dark"] #DashboardHeader {
            background: #18181B;
            border-bottom: 1px solid #27272A;
        }
        #DashboardRoot[theme="dark"] #DashboardTitle {
            color: #FFFFFF;
        }
        #DashboardRoot[theme="dark"] #DashboardSubtitle {
            color: #9F9FA9;
        }
        #DashboardRoot[theme="dark"] #DashboardLogoutButton {
            background: transparent;
            border: 1px solid #27272A;
            color: #9F9FA9;
        }
        #DashboardRoot[theme="dark"] #DashboardLogoutButton:hover {
            background: #27272A;
        }
        #DashboardRoot[theme="dark"] #DashboardContent {
            background-color: #09090B;
        }
        #DashboardRoot[theme="dark"] #DashboardCardStatus,
        #DashboardRoot[theme="dark"] #DashboardCardScans,
        #DashboardRoot[theme="dark"] #DashboardCardCameras,
        #DashboardRoot[theme="dark"] #DashboardCardCalibration,
        #DashboardRoot[theme="dark"] #DashboardActionsCard {
            background: #18181B;
            border: 1px solid #27272A;
        }
        #DashboardRoot[theme="dark"] #DashboardInfoCard {
            background: #18181B;
            border: 1px solid #27272A;
        }
    )");

    const bool is_dark = dark_mode_;
    const QString accent = "#00BC7D";
    const QString card_label_color = is_dark ? "#9F9FA9" : "#71717B";
    const QString actions_title_color = is_dark ? "#FFFFFF" : "#18181B";
    const QString action_desc_color = "#71717B";
    const QString info_title_color = is_dark ? "#FFFFFF" : "#18181B";
    const QString info_label_color = "#71717B";
    const QString info_value_color = is_dark ? "#FFFFFF" : "#18181B";

    auto setLabelStyle = [](QLabel* label, const QString& style) {
        if (label) {
            label->setStyleSheet(style);
        }
    };

    if (btn_logout_) {
        QPixmap logoutPix = loadSvgPixmap(":/assets/dashboard/logout.svg", 20, 20,
                                          is_dark ? "#9F9FA9" : "#71717B");
        if (!logoutPix.isNull()) {
            btn_logout_->setIcon(QIcon(logoutPix));
            btn_logout_->setIconSize(QSize(20, 20));
        }
    }

    auto setCardLabelStyle = [&](const QString& objectName) {
        if (auto* label = findChild<QLabel*>(objectName)) {
            setLabelStyle(label, QString("font-size: 14px; line-height: 20px; color: %1;")
                                     .arg(card_label_color));
        }
    };

    setCardLabelStyle("DashboardCardStatusLabel");
    setCardLabelStyle("DashboardCardScansLabel");
    setCardLabelStyle("DashboardCardCamerasLabel");
    setCardLabelStyle("DashboardCardCalibrationLabel");

    const QString value_style =
        "font-family: 'Arimo'; font-weight: 700; font-size: 24px; line-height: 32px; color: %1;";
    setLabelStyle(lbl_status_value_, QString(value_style).arg(accent));
    setLabelStyle(lbl_scans_value_, QString(value_style).arg(is_dark ? "#FFFFFF" : "#18181B"));
    setLabelStyle(lbl_cameras_value_, QString(value_style).arg(is_dark ? "#FFFFFF" : "#18181B"));
    setLabelStyle(lbl_calibration_value_, QString(value_style).arg(is_dark ? "#FFFFFF" : "#18181B"));

    if (auto* actionsTitle = findChild<QLabel*>("DashboardActionsTitle")) {
        setLabelStyle(actionsTitle,
                      QString("font-family: 'Arimo'; font-weight: 700; font-size: 24px; "
                              "line-height: 32px; color: %1;")
                          .arg(actions_title_color));
    }

    auto setActionTitleStyle = [&](const QString& objectName, const QString& color) {
        if (auto* label = findChild<QLabel*>(objectName)) {
            setLabelStyle(label,
                          QString("font-family: 'Arimo'; font-weight: 700; font-size: 18px; "
                                  "line-height: 28px; color: %1;")
                              .arg(color));
        }
    };
    auto setActionDescStyle = [&](const QString& objectName) {
        if (auto* label = findChild<QLabel*>(objectName)) {
            setLabelStyle(label,
                          QString("font-family: 'Arimo'; font-size: 14px; line-height: 20px; "
                                  "color: %1;")
                              .arg(action_desc_color));
        }
    };

    setActionTitleStyle("DashboardBtnStartScanTitle", is_dark ? "#FFFFFF" : "#18181B");
    setActionTitleStyle("DashboardBtnDiagnosticsTitle", is_dark ? "#FFFFFF" : "#18181B");
    setActionTitleStyle("DashboardBtnRecordingsTitle", is_dark ? "#FFFFFF" : "#18181B");
    setActionTitleStyle("DashboardBtnCalibrateTiltTitle", is_dark ? "#FFFFFF" : "#18181B");
    setActionDescStyle("DashboardBtnStartScanDesc");
    setActionDescStyle("DashboardBtnDiagnosticsDesc");
    setActionDescStyle("DashboardBtnRecordingsDesc");
    setActionDescStyle("DashboardBtnCalibrateTiltDesc");

    auto* infoTitle = findChild<QLabel*>("DashboardInfoTitle");
    if (infoTitle) {
        setLabelStyle(infoTitle,
                      QString("font-family: 'Arimo'; font-weight: 700; font-size: 18px; "
                              "line-height: 27px; color: %1;")
                          .arg(info_title_color));
    }

    const QString info_value_style =
        "font-family: 'Arimo'; font-size: 14px; line-height: 20px; color: %1;";
    setLabelStyle(lbl_robot_id_value_, QString(info_value_style).arg(info_value_color));
    setLabelStyle(lbl_firmware_value_, QString(info_value_style).arg(info_value_color));
    setLabelStyle(lbl_calibration_value_info_, QString(info_value_style).arg(info_value_color));
    setLabelStyle(lbl_battery_value_, QString(info_value_style).arg(info_value_color));

    if (auto* infoCard = findChild<QWidget*>("DashboardInfoCard")) {
        const auto labels = infoCard->findChildren<QLabel*>();
        for (auto* label : labels) {
            if (!label || label == infoTitle ||
                label == lbl_robot_id_value_ || label == lbl_firmware_value_ ||
                label == lbl_calibration_value_info_ || label == lbl_battery_value_) {
                continue;
            }
            setLabelStyle(label,
                          QString("font-family: 'Arimo'; font-weight: 700; font-size: 14px; "
                                  "line-height: 20px; color: %1;")
                              .arg(info_label_color));
        }
    }

    const QString icon_bg = "rgba(0, 188, 125, 0.1)";
    auto updateCardIcon = [&](const QString& cardName, const QString& resourcePath,
                              const QString& bgColor, const QString& strokeColor) {
        if (auto* icon = findChild<QLabel*>(cardName + "Icon")) {
            icon->setStyleSheet(QString("background: %1; border-radius: 10px;").arg(bgColor));
            QPixmap iconPix = loadSvgPixmap(resourcePath, 32, 32, strokeColor);
            if (!iconPix.isNull()) {
                icon->setPixmap(iconPix);
            }
        }
    };

    updateCardIcon("DashboardCardStatus", ":/assets/dashboard/heartbeat.svg",
                   icon_bg, accent);
    updateCardIcon("DashboardCardScans", ":/assets/dashboard/location_pin.svg",
                   icon_bg, accent);
    updateCardIcon("DashboardCardCameras", ":/assets/dashboard/battery.svg",
                   icon_bg, accent);
    updateCardIcon("DashboardCardCalibration", ":/assets/dashboard/settings.svg",
                   icon_bg, accent);

    const QString action_bg = is_dark ? "#27272A" : "#FFFFFF";
    const QString action_border = is_dark ? "#27272A" : "#E4E4E7";
    const QString action_hover = is_dark ? "#2f2f33" : "#F4F4F5";
    const int action_border_width = 1;

    auto updateActionButtonStyle = [&](QPushButton* button) {
        if (!button) {
            return;
        }
        const QString selector = QString("#%1").arg(button->objectName());
        button->setStyleSheet(QString(
            "%1 {"
            "  background: %2;"
            "  border: %3px solid %4;"
            "  border-radius: 10px;"
            "  text-align: left;"
            "}"
            "%1:hover:enabled { background: %5; }"
            "%1:focus { outline: none; }")
            .arg(selector, action_bg, QString::number(action_border_width),
                 action_border, action_hover));
    };

    updateActionButtonStyle(btn_start_scan_);
    updateActionButtonStyle(btn_run_diagnostics_);
    updateActionButtonStyle(btn_view_recordings_);
    updateActionButtonStyle(btn_calibrate_tilt_);

    auto updateActionIcon = [&](const QString& buttonName, const QString& resourcePath,
                                const QString& strokeColor) {
        if (auto* icon = findChild<QLabel*>(buttonName + "Icon")) {
            QPixmap iconPix = loadSvgPixmap(resourcePath, 40, 40, strokeColor);
            if (!iconPix.isNull()) {
                icon->setPixmap(iconPix);
            }
        }
    };

    updateActionIcon("DashboardBtnStartScan", ":/assets/dashboard/location_pin.svg", accent);
    updateActionIcon("DashboardBtnDiagnostics", ":/assets/dashboard/heartbeat.svg", accent);
    updateActionIcon("DashboardBtnRecordings", ":/assets/dashboard/camera.svg", accent);
    updateActionIcon("DashboardBtnCalibrateTilt", ":/assets/dashboard/settings.svg", "#E17100");
}

}  // namespace f2c_cpp
