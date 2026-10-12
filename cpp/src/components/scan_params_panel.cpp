#include "components/scan_params_panel.hpp"

#include "svg_tint.hpp"
#include "ui_theme_constants.hpp"
#include "units_system.hpp"

#include <QButtonGroup>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace f2c_cpp {
namespace {

constexpr int kCardMinHeight = 60;
constexpr int kSpeedButtonHeight = 68;
constexpr int kCardIconPx = 18;
constexpr int kSpeedIconPx = 30;

constexpr int kSliderW = 208;
constexpr int kSliderH = 272;
constexpr double kTrackTop = 16.0;
constexpr double kTrackHeight = 240.0;
constexpr double kTrackX = 80.0;
constexpr double kTrackW = 6.0;
constexpr int kThumbPx = 28;
constexpr int kMajorTickW = 16;
constexpr int kMajorTickH = 3;
constexpr int kMinorTickW = 6;
constexpr int kMinorTickH = 2;
constexpr qreal kLockedOpacity = 0.55;

const char* kSpeedIcon[3] = {
    ":/assets/scansetup/speed_slow.svg",
    ":/assets/scansetup/speed_medium.svg",
    ":/assets/scansetup/speed_fast.svg",
};

const char* kPresetName[3] = {"DETAILED", "STANDARD", "QUICK"};

double speedForIndex(int index) {
    if (index <= 0) {
        return ScanParams::kSpeedMinMps;
    }
    if (index >= 2) {
        return ScanParams::kSpeedMaxMps;
    }
    return ScanParams::kSpeedDefaultMps;
}

int indexForSpeed(double mps) {
    const double snapped = ScanParams::snapSpeed(mps);
    if (snapped < 0.45) {
        return 0;
    }
    if (snapped > 0.55) {
        return 2;
    }
    return 1;
}

QString laneNumber(int half_feet) {
    if (UnitsProvider::instance()->isMetric()) {
        const QString full = units::formatLength(
            ScanParams::metersFromHalfFeet(half_feet), 2);
        return full.section(QLatin1Char(' '), 0, 0);
    }
    if (half_feet % 2 == 0) {
        return QString::number(half_feet / 2);
    }
    return QString::number(half_feet * 0.5, 'f', 1);
}

QColor minorTickColor(bool dark) {
    // Spec pair. `faint` is the same as `muted` in light mode, so a theme
    // token cannot make the half-foot ticks quieter there.
    return QColor(dark ? QStringLiteral("#52525B") : QStringLiteral("#A1A1AA"));
}

QString labelStyle(const QString& color, int px, int weight, double spacing) {
    return QStringLiteral(
               "background: transparent; font-family: 'Arimo'; "
               "font-size: %1px; font-weight: %2; letter-spacing: %3px; "
               "color: %4;")
        .arg(px)
        .arg(weight)
        .arg(spacing, 0, 'f', 1)
        .arg(color);
}

void ignoreMouse(QWidget* widget) {
    widget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

double yForHalf(int half) {
    const double t =
        (ScanParams::kWidthHalfFeetMax - half) /
        double(ScanParams::kWidthHalfFeetMax - ScanParams::kWidthHalfFeetMin);
    return kTrackTop + t * kTrackHeight;
}

int halfForY(double y) {
    double t = (y - kTrackTop) / kTrackHeight;
    t = std::min(1.0, std::max(0.0, t));
    const double half = ScanParams::kWidthHalfFeetMax -
                        t * (ScanParams::kWidthHalfFeetMax -
                             ScanParams::kWidthHalfFeetMin);
    int snapped = static_cast<int>(std::lround(half));
    if (snapped < ScanParams::kWidthHalfFeetMin) {
        snapped = ScanParams::kWidthHalfFeetMin;
    }
    if (snapped > ScanParams::kWidthHalfFeetMax) {
        snapped = ScanParams::kWidthHalfFeetMax;
    }
    return snapped;
}

class RadioMark : public QWidget {
public:
    explicit RadioMark(QWidget* parent) : QWidget(parent) {
        setFixedSize(20, 20);
        ignoreMouse(this);
    }

    void setOn(bool on, const QColor& ring, const QColor& dot) {
        on_ = on;
        ring_ = ring;
        dot_ = dot;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(ring_, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(1, 1, 18, 18));
        if (on_) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(dot_);
            painter.drawEllipse(QRectF(5, 5, 10, 10));
        }
    }

private:
    bool on_ = false;
    QColor ring_ = Qt::gray;
    QColor dot_ = Qt::transparent;
};

class ChevronMark : public QWidget {
public:
    explicit ChevronMark(QWidget* parent) : QWidget(parent) {
        setFixedSize(14, 14);
        ignoreMouse(this);
    }

    void setOpen(bool open, const QColor& color) {
        open_ = open;
        color_ = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(color_, 1.6, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
        if (open_) {
            painter.drawLine(QPointF(2, 4.5), QPointF(7, 9.5));
            painter.drawLine(QPointF(7, 9.5), QPointF(12, 4.5));
        } else {
            painter.drawLine(QPointF(4.5, 2), QPointF(9.5, 7));
            painter.drawLine(QPointF(9.5, 7), QPointF(4.5, 12));
        }
    }

private:
    bool open_ = false;
    QColor color_ = Qt::gray;
};

class WidthSlider : public QWidget {
public:
    explicit WidthSlider(QWidget* parent) : QWidget(parent) {
        setFixedSize(kSliderW, kSliderH);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QStringLiteral("Lane width"));
    }

    void setHalfFeet(int half) {
        half_ = std::min(ScanParams::kWidthHalfFeetMax,
                         std::max(ScanParams::kWidthHalfFeetMin, half));
        update();
    }

    void setEditable(bool editable) {
        editable_ = editable;
        setCursor(editable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    void setColors(const QColor& muted, const QColor& minor,
                   const QColor& accent, const QColor& track,
                   const QColor& text) {
        muted_ = muted;
        minor_ = minor;
        accent_ = accent;
        track_ = track;
        text_ = text;
        update();
    }

    std::function<void(int)> on_half_changed;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const double thumb_y = yForHalf(half_);

        QFont label_font(QStringLiteral("Arimo"));
        label_font.setPixelSize(12);
        painter.setFont(label_font);
        painter.setPen(muted_);
        for (int half = ScanParams::kWidthHalfFeetMin;
             half <= ScanParams::kWidthHalfFeetMax; ++half) {
            const double y = yForHalf(half);
            const bool major = (half % 4) == 2;
            const int tick_w = major ? kMajorTickW : kMinorTickW;
            const int tick_h = major ? kMajorTickH : kMinorTickH;
            const double tick_x = (kTrackX - 4.0) - tick_w;
            painter.fillRect(QRectF(tick_x, y - tick_h / 2.0, tick_w, tick_h),
                             major ? muted_ : minor_);
            if (major) {
                painter.drawText(
                    QRectF(0, y - 8, 56, 16),
                    Qt::AlignRight | Qt::AlignVCenter,
                    laneNumber(half) + units::lengthUnitSuffix());
            }
        }

        painter.setPen(Qt::NoPen);
        const QRectF track(kTrackX, kTrackTop, kTrackW, kTrackHeight);
        painter.setBrush(track_);
        painter.drawRoundedRect(track, 3, 3);
        const double fill_h = (kTrackTop + kTrackHeight) - thumb_y;
        if (fill_h > 0.5) {
            painter.setBrush(accent_);
            painter.drawRoundedRect(
                QRectF(kTrackX, thumb_y, kTrackW, fill_h), 3, 3);
        }

        const QRectF thumb(kTrackX + kTrackW / 2.0 - kThumbPx / 2.0,
                           thumb_y - kThumbPx / 2.0, kThumbPx, kThumbPx);
        painter.setBrush(QColor(0, 0, 0, 90));
        painter.drawEllipse(thumb.translated(0, 1).adjusted(1.5, 1.5, -1.5, -1.5));
        painter.setPen(QPen(accent_, 3));
        painter.setBrush(Qt::white);
        painter.drawEllipse(thumb.adjusted(1.5, 1.5, -1.5, -1.5));

        const QString number = laneNumber(half_);
        const QString unit = units::lengthUnitSuffix().trimmed();
        QFont value_font(QStringLiteral("Arimo"));
        value_font.setPixelSize(28);
        value_font.setWeight(QFont::Bold);
        QFont unit_font(QStringLiteral("Arimo"));
        unit_font.setPixelSize(14);
        const int value_h = QFontMetrics(value_font).height();
        int text_y = static_cast<int>(std::lround(thumb_y - value_h / 2.0));
        text_y = std::min(height() - value_h, std::max(0, text_y));
        painter.setFont(value_font);
        painter.setPen(text_);
        const int number_w =
            QFontMetrics(value_font).horizontalAdvance(number);
        painter.drawText(QRect(108, text_y, number_w + 2, value_h),
                         Qt::AlignLeft | Qt::AlignVCenter, number);
        painter.setFont(unit_font);
        painter.setPen(muted_);
        painter.drawText(QRect(108 + number_w + 4, text_y, 36, value_h),
                         Qt::AlignLeft | Qt::AlignVCenter, unit);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (!editable_ || event->button() != Qt::LeftButton) {
            return;
        }
        applyY(event->pos().y());
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!editable_ || !(event->buttons() & Qt::LeftButton)) {
            return;
        }
        applyY(event->pos().y());
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (!editable_) {
            QWidget::keyPressEvent(event);
            return;
        }
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Right) {
            commit(half_ + 1);
        } else if (event->key() == Qt::Key_Down ||
                   event->key() == Qt::Key_Left) {
            commit(half_ - 1);
        } else {
            QWidget::keyPressEvent(event);
        }
    }

private:
    void applyY(int y) { commit(halfForY(y)); }

    void commit(int half) {
        half = std::min(ScanParams::kWidthHalfFeetMax,
                        std::max(ScanParams::kWidthHalfFeetMin, half));
        if (half == half_) {
            return;
        }
        half_ = half;
        update();
        if (on_half_changed) {
            on_half_changed(half_);
        }
    }

    int half_ = 6;
    bool editable_ = true;
    QColor muted_ = QColor(QStringLiteral("#9F9FA9"));
    QColor minor_ = QColor(QStringLiteral("#52525B"));
    QColor accent_ = QColor(QStringLiteral("#00BC7D"));
    QColor track_ = QColor(QStringLiteral("#3f3f47"));
    QColor text_ = QColor(QStringLiteral("#F3F4F6"));
};

QString cardStyle(const UiThemeTokens& tokens, bool dark) {
    QColor wash(tokens.accent_green);
    wash.setAlphaF(dark ? 0.14 : 0.12);
    const QString selected = wash.name(QColor::HexArgb);
    // No :hover. A touchscreen leaves hover on the last widget touched,
    // and that grey fill read as a second selected card.
    return QStringLiteral(
               "QPushButton { background: %1; border: 1px solid %2; "
               "border-radius: 10px; text-align: left; }"
               "QPushButton:checked { background: %3; border: 1px solid %4; }"
               "QPushButton:disabled { background: %1; border: 1px solid %2; }"
               "QPushButton:checked:disabled { background: %3; "
               "border: 1px solid %4; }")
        .arg(tokens.raised, tokens.raised_border, selected, tokens.accent_green);
}

QString speedStyle(const UiThemeTokens& tokens, bool dark) {
    QColor wash(tokens.accent_green);
    wash.setAlphaF(dark ? 0.22 : 0.20);
    const QString selected = wash.name(QColor::HexArgb);
    return QStringLiteral(
               "QPushButton { background: %1; border: 1px solid %2; "
               "border-radius: 8px; }"
               "QPushButton:checked { background: %3; border: 1px solid %4; }"
               "QPushButton:disabled { background: %1; border: 1px solid %2; }"
               "QPushButton:checked:disabled { background: %3; "
               "border: 1px solid %4; }")
        .arg(tokens.surface, tokens.raised_border, selected, tokens.accent_green);
}

struct PresetCard {
    QPushButton* button = nullptr;
    QLabel* name = nullptr;
    QLabel* lanes = nullptr;
    QLabel* icon = nullptr;
    QLabel* speed = nullptr;
    RadioMark* radio = nullptr;
};

}  // namespace

class ScanParamsPanel::PanelPrivate {
public:
    PresetCard cards[3];
    QWidget* custom_frame = nullptr;
    QPushButton* custom_button = nullptr;
    QLabel* custom_name = nullptr;
    QLabel* custom_summary = nullptr;
    ChevronMark* chevron = nullptr;
    QWidget* custom_body = nullptr;
    QPushButton* speed_buttons[3] = {};
    QLabel* speed_icons[3] = {};
    QLabel* speed_labels[3] = {};
    QLabel* blurb = nullptr;
    QWidget* cards_host = nullptr;
    QLabel* section_labels[2] = {};
    WidthSlider* slider = nullptr;
    QButtonGroup* presets = nullptr;
    QButtonGroup* speeds = nullptr;
    QPixmap card_icon[3][2];
    QPixmap speed_icon[3][2];
    QString text;
    QString muted;
    QString accent_text;
};

ScanParamsPanel::ScanParamsPanel(QWidget* parent) : QWidget(parent) {
    ui_ = new PanelPrivate;
    dark_mode_ = appDarkMode();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("Scan Parameters"), this);
    title->setObjectName(QStringLiteral("SatRoiTitle"));
    layout->addWidget(title);
    ui_->blurb = new QLabel(
        QStringLiteral("Pick how detailed the scan should be."), this);
    ui_->blurb->setObjectName(QStringLiteral("SatRoiBlurb"));
    ui_->blurb->setWordWrap(true);
    layout->addWidget(ui_->blurb);

    auto* cards = new QWidget(this);
    ui_->cards_host = cards;
    auto* card_layout = new QVBoxLayout(cards);
    card_layout->setContentsMargins(0, 0, 0, 0);
    card_layout->setSpacing(8);
    layout->addWidget(cards);

    ui_->presets = new QButtonGroup(this);
    ui_->presets->setExclusive(true);
    ui_->speeds = new QButtonGroup(this);
    ui_->speeds->setExclusive(true);

    const ScanPreset named[3] = {ScanPreset::Detailed, ScanPreset::Standard,
                                 ScanPreset::Quick};
    for (int i = 0; i < 3; ++i) {
        auto* button = new QPushButton(cards);
        button->setCheckable(true);
        button->setAutoExclusive(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setMinimumHeight(kCardMinHeight);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        button->setFocusPolicy(Qt::NoFocus);
        auto* row = new QHBoxLayout(button);
        row->setContentsMargins(14, 10, 14, 10);
        row->setSpacing(12);

        auto* text_col = new QVBoxLayout;
        text_col->setContentsMargins(0, 0, 0, 0);
        text_col->setSpacing(4);
        auto* name = new QLabel(QString::fromLatin1(kPresetName[i]), button);
        ignoreMouse(name);
        text_col->addWidget(name);
        auto* line = new QHBoxLayout;
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(6);
        auto* lanes = new QLabel(button);
        auto* icon = new QLabel(button);
        icon->setFixedSize(kCardIconPx, kCardIconPx);
        auto* speed = new QLabel(button);
        ignoreMouse(lanes);
        ignoreMouse(icon);
        ignoreMouse(speed);
        line->addWidget(lanes);
        line->addWidget(icon);
        line->addWidget(speed);
        line->addStretch(1);
        text_col->addLayout(line);
        row->addLayout(text_col, 1);

        auto* radio = new RadioMark(button);
        row->addWidget(radio, 0, Qt::AlignVCenter);

        ui_->cards[i] = {button, name, lanes, icon, speed, radio};
        ui_->presets->addButton(button, static_cast<int>(named[i]));
        card_layout->addWidget(button);
    }

    ui_->custom_frame = new QWidget(cards);
    ui_->custom_frame->setObjectName(QStringLiteral("SatScanCustom"));
    ui_->custom_frame->setAttribute(Qt::WA_StyledBackground, true);
    auto* custom_layout = new QVBoxLayout(ui_->custom_frame);
    custom_layout->setContentsMargins(0, 0, 0, 0);
    custom_layout->setSpacing(0);

    ui_->custom_button = new QPushButton(ui_->custom_frame);
    ui_->custom_button->setCheckable(true);
    ui_->custom_button->setAutoExclusive(false);
    ui_->custom_button->setCursor(Qt::PointingHandCursor);
    ui_->custom_button->setMinimumHeight(kCardMinHeight);
    ui_->custom_button->setFocusPolicy(Qt::NoFocus);
    auto* custom_row = new QHBoxLayout(ui_->custom_button);
    custom_row->setContentsMargins(14, 10, 14, 10);
    custom_row->setSpacing(12);
    auto* custom_text = new QVBoxLayout;
    custom_text->setSpacing(4);
    ui_->custom_name = new QLabel(QStringLiteral("CUSTOM"), ui_->custom_button);
    ui_->custom_summary = new QLabel(ui_->custom_button);
    ui_->custom_summary->setWordWrap(true);
    ignoreMouse(ui_->custom_name);
    ignoreMouse(ui_->custom_summary);
    custom_text->addWidget(ui_->custom_name);
    custom_text->addWidget(ui_->custom_summary);
    custom_row->addLayout(custom_text, 1);
    ui_->chevron = new ChevronMark(ui_->custom_button);
    custom_row->addWidget(ui_->chevron, 0, Qt::AlignVCenter);
    ui_->presets->addButton(ui_->custom_button,
                            static_cast<int>(ScanPreset::Custom));
    custom_layout->addWidget(ui_->custom_button);

    ui_->custom_body = new QWidget(ui_->custom_frame);
    auto* body = new QVBoxLayout(ui_->custom_body);
    body->setContentsMargins(12, 2, 12, 14);
    body->setSpacing(16);

    auto* speed_block = new QWidget(ui_->custom_body);
    auto* speed_col = new QVBoxLayout(speed_block);
    speed_col->setContentsMargins(0, 0, 0, 0);
    speed_col->setSpacing(8);
    ui_->section_labels[0] =
        new QLabel(QStringLiteral("ROBOT SPEED"), speed_block);
    speed_col->addWidget(ui_->section_labels[0]);
    auto* speed_row = new QHBoxLayout;
    speed_row->setSpacing(6);
    const char* speed_names[3] = {"Slow", "Medium", "Fast"};
    for (int i = 0; i < 3; ++i) {
        auto* button = new QPushButton(speed_block);
        button->setCheckable(true);
        button->setAutoExclusive(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedHeight(kSpeedButtonHeight);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAccessibleName(
            QStringLiteral("Robot speed %1").arg(QLatin1String(speed_names[i])));
        auto* col = new QVBoxLayout(button);
        col->setContentsMargins(4, 4, 4, 4);
        col->setSpacing(4);
        auto* icon = new QLabel(button);
        icon->setFixedSize(kSpeedIconPx, kSpeedIconPx);
        icon->setAlignment(Qt::AlignCenter);
        auto* label =
            new QLabel(QString::fromLatin1(speed_names[i]), button);
        label->setAlignment(Qt::AlignCenter);
        ignoreMouse(icon);
        ignoreMouse(label);
        col->addStretch(1);
        col->addWidget(icon, 0, Qt::AlignHCenter);
        col->addWidget(label);
        col->addStretch(1);
        ui_->speed_buttons[i] = button;
        ui_->speed_icons[i] = icon;
        ui_->speed_labels[i] = label;
        ui_->speeds->addButton(button, i);
        speed_row->addWidget(button, 1);
    }
    speed_col->addLayout(speed_row);
    body->addWidget(speed_block);

    auto* width_block = new QWidget(ui_->custom_body);
    auto* width_col = new QVBoxLayout(width_block);
    width_col->setContentsMargins(0, 0, 0, 0);
    width_col->setSpacing(4);
    ui_->section_labels[1] =
        new QLabel(QStringLiteral("LANE WIDTH"), width_block);
    width_col->addWidget(ui_->section_labels[1]);
    ui_->slider = new WidthSlider(width_block);
    width_col->addWidget(ui_->slider, 0, Qt::AlignLeft);
    body->addWidget(width_block);
    custom_layout->addWidget(ui_->custom_body);
    card_layout->addWidget(ui_->custom_frame);

    connect(ui_->presets, &QButtonGroup::idClicked, this,
            [this](int id) { selectPreset(static_cast<ScanPreset>(id)); });
    connect(ui_->speeds, &QButtonGroup::idClicked, this,
            [this](int id) { setCustomSpeed(speedForIndex(id)); });
    ui_->slider->on_half_changed = [this](int half) { setCustomHalfFeet(half); };

    connect(UnitsProvider::instance(), &UnitsProvider::unitsChanged, this,
            [this] { refresh(); });

    restyle();
}

ScanParamsPanel::~ScanParamsPanel() { delete ui_; }

QString ScanParamsPanel::laneWidthText(double meters) {
    if (UnitsProvider::instance()->isMetric()) {
        return units::formatLength(ScanParams::snapWidth(meters), 2);
    }
    return laneNumber(ScanParams::halfFeetFromMeters(meters)) +
           units::lengthUnitSuffix();
}

void ScanParamsPanel::setParams(const ScanParams& params) {
    params_ = params.resolved();
    if (params_.preset == ScanPreset::Custom) {
        custom_width_m_ = params_.coverage_width_m;
        custom_speed_mps_ = params_.scan_speed_mps;
        custom_set_ = true;
    }
    refresh();
}

void ScanParamsPanel::resetCustomMemory() { custom_set_ = false; }

void ScanParamsPanel::setEditable(bool editable) {
    editable_ = editable;
    const QString tip = editable
                            ? QString()
                            : QStringLiteral(
                                  "Locked while a mission is running.");
    setToolTip(tip);
    for (const PresetCard& card : ui_->cards) {
        card.button->setEnabled(editable);
        card.button->setCursor(editable ? Qt::PointingHandCursor
                                        : Qt::ArrowCursor);
    }
    ui_->custom_button->setEnabled(editable);
    ui_->custom_button->setCursor(editable ? Qt::PointingHandCursor
                                           : Qt::ArrowCursor);
    for (QPushButton* button : ui_->speed_buttons) {
        button->setEnabled(editable);
        button->setCursor(editable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    ui_->slider->setEditable(editable);
    ui_->blurb->setText(
        editable ? QStringLiteral("Pick how detailed the scan should be.")
                 : QStringLiteral("Locked while a mission is running."));
    // Only the cards fade. The title and the lock notice stay at full
    // contrast, since the notice is the operator's only explanation.
    // The card refresh runs on map edits, so only touch the effect when
    // the lock actually changes. Rebuilding it every time flickers.
    QWidget* host = ui_->cards_host;
    if (editable) {
        if (host->graphicsEffect()) {
            host->setGraphicsEffect(nullptr);
        }
    } else if (!host->graphicsEffect()) {
        auto* dim = new QGraphicsOpacityEffect(host);
        dim->setOpacity(kLockedOpacity);
        host->setGraphicsEffect(dim);
    }
}

void ScanParamsPanel::setDarkMode(bool dark_mode) {
    dark_mode_ = dark_mode;
    restyle();
}

void ScanParamsPanel::selectPreset(ScanPreset preset) {
    if (applying_) {
        return;
    }
    if (!editable_) {
        refresh();
        return;
    }
    ScanParams next;
    if (preset == ScanPreset::Custom) {
        if (!custom_set_) {
            custom_width_m_ = params_.coverage_width_m;
            custom_speed_mps_ = params_.scan_speed_mps;
            custom_set_ = true;
        }
        next.preset = ScanPreset::Custom;
        next.coverage_width_m = custom_width_m_;
        next.scan_speed_mps = custom_speed_mps_;
        next = next.resolved();
    } else {
        next = ScanParams::fromPreset(preset);
    }
    if (next.preset == params_.preset &&
        next.coverage_width_m == params_.coverage_width_m &&
        next.scan_speed_mps == params_.scan_speed_mps) {
        return;
    }
    params_ = next;
    refresh();
    emit paramsChanged(params_);
}

void ScanParamsPanel::setCustomSpeed(double mps) {
    if (!editable_ || applying_) {
        return;
    }
    const double snapped = ScanParams::snapSpeed(mps);
    if (params_.preset == ScanPreset::Custom &&
        params_.scan_speed_mps == snapped) {
        return;
    }
    custom_speed_mps_ = snapped;
    custom_set_ = true;
    params_.preset = ScanPreset::Custom;
    params_.coverage_width_m = custom_width_m_;
    params_.scan_speed_mps = snapped;
    params_ = params_.resolved();
    refresh();
    emit paramsChanged(params_);
}

void ScanParamsPanel::setCustomHalfFeet(int half_feet) {
    if (!editable_ || applying_) {
        return;
    }
    const double width = ScanParams::metersFromHalfFeet(half_feet);
    if (params_.preset == ScanPreset::Custom &&
        params_.coverage_width_m == width) {
        return;
    }
    custom_width_m_ = width;
    custom_set_ = true;
    params_.preset = ScanPreset::Custom;
    params_.coverage_width_m = width;
    params_.scan_speed_mps = custom_speed_mps_;
    params_ = params_.resolved();
    refresh();
    emit paramsChanged(params_);
}

void ScanParamsPanel::restyle() {
    const UiThemeTokens tokens = uiThemeTokens(dark_mode_);
    ui_->text = tokens.text;
    ui_->muted = tokens.muted;
    ui_->accent_text = tokens.accent_text;

    const QString cards = cardStyle(tokens, dark_mode_);
    for (const PresetCard& card : ui_->cards) {
        card.button->setStyleSheet(cards);
    }
    ui_->custom_button->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; }"
        "QPushButton:disabled { background: transparent; border: none; }"));
    const QString speeds = speedStyle(tokens, dark_mode_);
    for (QPushButton* button : ui_->speed_buttons) {
        button->setStyleSheet(speeds);
    }
    for (QLabel* label : ui_->section_labels) {
        label->setStyleSheet(labelStyle(tokens.muted, 11, 700, 0.5));
    }

    for (int i = 0; i < 3; ++i) {
        ui_->card_icon[i][0] =
            loadTintedSvg(QString::fromLatin1(kSpeedIcon[i]), kCardIconPx,
                          kCardIconPx, tokens.muted);
        ui_->card_icon[i][1] =
            loadTintedSvg(QString::fromLatin1(kSpeedIcon[i]), kCardIconPx,
                          kCardIconPx, tokens.accent_text);
        ui_->speed_icon[i][0] =
            loadTintedSvg(QString::fromLatin1(kSpeedIcon[i]), kSpeedIconPx,
                          kSpeedIconPx, tokens.muted);
        ui_->speed_icon[i][1] =
            loadTintedSvg(QString::fromLatin1(kSpeedIcon[i]), kSpeedIconPx,
                          kSpeedIconPx, tokens.accent_text);
    }
    ui_->slider->setColors(QColor(tokens.muted), minorTickColor(dark_mode_),
                           QColor(tokens.accent_green),
                           QColor(tokens.raised_border), QColor(tokens.text));
    refresh();
}

void ScanParamsPanel::refresh() {
    applying_ = true;
    const QSignalBlocker block_presets(ui_->presets);
    const QSignalBlocker block_speeds(ui_->speeds);

    const bool custom = params_.preset == ScanPreset::Custom;
    const ScanPreset named[3] = {ScanPreset::Detailed, ScanPreset::Standard,
                                 ScanPreset::Quick};
    for (int i = 0; i < 3; ++i) {
        const ScanParams spec = ScanParams::fromPreset(named[i]);
        const bool on = params_.preset == named[i];
        PresetCard& card = ui_->cards[i];
        card.button->setChecked(on);
        card.name->setStyleSheet(
            labelStyle(on ? ui_->accent_text : ui_->text, 14, 700, 1.0));
        card.lanes->setText(laneWidthText(spec.coverage_width_m) +
                            QStringLiteral(" lanes ·"));
        card.lanes->setStyleSheet(labelStyle(ui_->muted, 12, 400, 0.0));
        card.speed->setText(ScanParams::speedTitle(spec.scan_speed_mps));
        card.speed->setStyleSheet(labelStyle(ui_->muted, 12, 400, 0.0));
        card.icon->setPixmap(ui_->card_icon[i][on ? 1 : 0]);
        const UiThemeTokens tokens = uiThemeTokens(dark_mode_);
        card.radio->setOn(
            on, QColor(on ? tokens.accent_green : tokens.raised_border),
            QColor(tokens.accent_green));
        card.button->setAccessibleName(
            QStringLiteral("%1, %2 lanes, %3")
                .arg(card.name->text(), laneWidthText(spec.coverage_width_m),
                     card.speed->text()));
    }

    ui_->custom_button->setChecked(custom);
    ui_->custom_name->setStyleSheet(
        labelStyle(custom ? ui_->accent_text : ui_->text, 14, 700, 1.0));
    ui_->custom_summary->setStyleSheet(labelStyle(ui_->muted, 12, 400, 0.0));
    if (custom) {
        ui_->custom_summary->setText(
            laneWidthText(params_.coverage_width_m) +
            QStringLiteral(" lanes · ") +
            ScanParams::speedTitle(params_.scan_speed_mps));
    } else {
        ui_->custom_summary->setText(
            QStringLiteral("Set your own width and speed"));
    }
    const UiThemeTokens tokens = uiThemeTokens(dark_mode_);
    ui_->chevron->setOpen(custom, QColor(tokens.muted));
    ui_->custom_body->setVisible(custom);

    QColor frame_wash(tokens.accent_green);
    frame_wash.setAlphaF(dark_mode_ ? 0.14 : 0.12);
    ui_->custom_frame->setStyleSheet(
        QStringLiteral("#SatScanCustom { background: %1; border: 1px solid %2; "
                       "border-radius: 10px; }")
            .arg(custom ? frame_wash.name(QColor::HexArgb) : tokens.raised,
                 custom ? tokens.accent_green : tokens.raised_border));

    const int speed_index = indexForSpeed(
        custom ? params_.scan_speed_mps : custom_speed_mps_);
    for (int i = 0; i < 3; ++i) {
        const bool on = i == speed_index;
        ui_->speed_buttons[i]->setChecked(on);
        ui_->speed_icons[i]->setPixmap(ui_->speed_icon[i][on ? 1 : 0]);
        ui_->speed_labels[i]->setStyleSheet(
            labelStyle(on ? ui_->text : ui_->muted, 12, 600, 0.0));
    }
    const double shown_width =
        custom ? params_.coverage_width_m : custom_width_m_;
    ui_->slider->setHalfFeet(ScanParams::halfFeetFromMeters(shown_width));

    applying_ = false;
}

}  // namespace f2c_cpp
