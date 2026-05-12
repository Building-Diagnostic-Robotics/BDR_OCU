/**
 * @file camera_switch_pill.cpp
 */

#include "components/camera_switch_pill.hpp"

#include <QByteArray>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QTimer>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QSvgRenderer>
#else
#include <QtSvg/QSvgRenderer>
#endif

namespace f2c_cpp {

namespace {

constexpr int kIconSize = 16;
constexpr int kButtonHeight = 36;
constexpr int kButtonRadius = 8;
constexpr int kContainerRadius = 10;
constexpr int kContainerPadding = 4;
constexpr int kContainerSpacing = 8;
constexpr int kDebounceMs = 1500;

const char kColorContainerBg[] = "#27272a";
const char kColorActiveBg[] = "#009966";
const char kColorActiveText[] = "#FFFFFF";
const char kColorInactiveText[] = "#9F9FA9";
const char kColorHoverOverlay[] = "rgba(255,255,255,0.04)";

// Path to the camera glyph in the OCU's resources.qrc.  Reused from the
// dashboard card; tinted at runtime to match the button's text color.
const char kCameraIconResource[] = ":/assets/dashboard/camera.svg";

// Render the camera glyph at `size` px tinted to `hex`.  Mirrors
// SvgIconButton::renderTinted but inlined here so this widget stays
// independent of any other component.
QPixmap renderCameraIcon(int size, const QString& hex) {
    QFile f(QString::fromLatin1(kCameraIconResource));
    if (!f.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }
    QString svg = QString::fromUtf8(f.readAll());
    f.close();
    // The dashboard's Camera_icon.svg uses concrete `stroke="#9810FA"`.
    // Replace that exact value with the requested tint so the glyph
    // matches the button's text color (white on active, grey on
    // inactive).
    svg.replace(QStringLiteral("#9810FA"), hex);
    svg.replace(QStringLiteral("currentColor"), hex);

    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid()) {
        return QPixmap();
    }
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&p);
    return pix;
}

}  // namespace

CameraSwitchPill::CameraSwitchPill(QWidget* parent) : QWidget(parent) {
    buildUi();
    refreshButtonStyling();
}

void CameraSwitchPill::buildUi() {
    setObjectName(QStringLiteral("CameraSwitchPill"));
    // WA_StyledBackground is REQUIRED for a plain QWidget to actually
    // paint its `background:` stylesheet rule.  QPushButton overrides
    // paintEvent to honor stylesheets natively, but plain QWidget does
    // not — without this attribute the `#27272a` container chrome
    // never renders, the inactive button (transparent) shows the
    // video bleeding through, and Qt's dirty-region optimizer leaves
    // repaint residue on the sibling HUD pills (FPS / Speed) sitting
    // on the same QStackedLayout(StackAll) overlay.  Safe here
    // because the QGraphicsEffect offscreen-render path that breaks
    // overlay transparency is gone — we don't apply any QGraphicsEffect
    // anywhere in this widget.
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QString::fromLatin1(
        "QWidget#CameraSwitchPill { background: %1; border-radius: %2px; }")
        .arg(QString::fromLatin1(kColorContainerBg))
        .arg(kContainerRadius));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kContainerPadding, kContainerPadding,
                               kContainerPadding, kContainerPadding);
    layout->setSpacing(kContainerSpacing);

    auto build_button = [this](QPushButton*& btn, const QString& text,
                               const QString& object_name) {
        btn = new QPushButton(this);
        btn->setObjectName(object_name);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        // Use QPushButton's native icon+text path so sizeHint() actually
        // accounts for both — and so the click lands on the button, not
        // on a child QLabel.
        btn->setText(text);
        btn->setIconSize(QSize(kIconSize, kIconSize));
        btn->setFixedHeight(kButtonHeight);
        // Width comes from sizeHint (icon + spacing + text + padding).
        // Fixed policy prevents the buttons from stretching when the
        // container ends up wider than its sizeHint (e.g. parent layout
        // gives extra space).
        btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    };

    build_button(btn_left_, QStringLiteral("Left Camera"),
                 QStringLiteral("CameraSwitchPillLeft"));
    build_button(btn_right_, QStringLiteral("Right Camera"),
                 QStringLiteral("CameraSwitchPillRight"));

    layout->addWidget(btn_left_);
    layout->addWidget(btn_right_);

    connect(btn_left_, &QPushButton::clicked, this,
            &CameraSwitchPill::onLeftClicked);
    connect(btn_right_, &QPushButton::clicked, this,
            &CameraSwitchPill::onRightClicked);

    debounce_timer_ = new QTimer(this);
    debounce_timer_->setSingleShot(true);
    debounce_timer_->setInterval(kDebounceMs);
    connect(debounce_timer_, &QTimer::timeout, this, [this]() {
        // Re-enable both buttons whether or not we got status confirmation.
        // If UDC's pipeline rebuild took longer than kDebounceMs, the
        // operator can issue a second click and we'll redo the dance.
        if (btn_left_) btn_left_->setEnabled(true);
        if (btn_right_) btn_right_->setEnabled(true);
        last_requested_.clear();
    });
}

void CameraSwitchPill::onLeftClicked() {
    requestCamera(QStringLiteral("left"));
}

void CameraSwitchPill::onRightClicked() {
    requestCamera(QStringLiteral("right"));
}

void CameraSwitchPill::requestCamera(const QString& cam) {
    if (cam == active_) {
        // Already active — no-op (Figma does not show a "click active
        // button = re-publish" affordance, and re-publishing would
        // trigger an unnecessary GStreamer rebuild on UDC).
        return;
    }
    last_requested_ = cam;
    if (btn_left_) btn_left_->setEnabled(false);
    if (btn_right_) btn_right_->setEnabled(false);
    if (debounce_timer_) {
        debounce_timer_->start();
    }
    emit cameraRequested(cam);
}

void CameraSwitchPill::setActiveCamera(const QString& cam) {
    if (cam != QStringLiteral("left") && cam != QStringLiteral("right")) {
        return;
    }
    if (active_ == cam) {
        // Already showing this camera as active — but the operator may
        // still have queued a debounce for THIS state (e.g. they hit
        // Right while we were already on Right but mid-debounce from a
        // L->R->L sequence).  Clear the lock if so.
        if (last_requested_ == cam) {
            if (debounce_timer_) debounce_timer_->stop();
            if (btn_left_) btn_left_->setEnabled(true);
            if (btn_right_) btn_right_->setEnabled(true);
            last_requested_.clear();
        }
        return;
    }
    active_ = cam;
    refreshButtonStyling();
    // If the confirmation matches our pending request, drop the
    // debounce early so the operator can switch back quickly.
    if (last_requested_ == cam) {
        if (debounce_timer_) debounce_timer_->stop();
        if (btn_left_) btn_left_->setEnabled(true);
        if (btn_right_) btn_right_->setEnabled(true);
        last_requested_.clear();
    }
}

void CameraSwitchPill::refreshButtonStyling() {
    auto apply = [](QPushButton* btn, bool is_active) {
        if (!btn) return;
        const QString text_color = is_active
            ? QString::fromLatin1(kColorActiveText)
            : QString::fromLatin1(kColorInactiveText);
        if (is_active) {
            btn->setStyleSheet(QString::fromLatin1(
                "QPushButton {"
                "  background: %1;"
                "  color: %2;"
                "  border: none;"
                "  border-radius: %3px;"
                "  padding: 0px 16px;"
                "  font-family: 'Arimo';"
                "  font-size: 14px;"
                "  font-weight: 400;"
                "  text-align: center;"
                "}"
                "QPushButton:disabled {"
                "  background: %1;"
                "  color: %2;"
                "}")
                .arg(QString::fromLatin1(kColorActiveBg))
                .arg(text_color)
                .arg(kButtonRadius));
        } else {
            btn->setStyleSheet(QString::fromLatin1(
                "QPushButton {"
                "  background: transparent;"
                "  color: %1;"
                "  border: none;"
                "  border-radius: %2px;"
                "  padding: 0px 16px;"
                "  font-family: 'Arimo';"
                "  font-size: 14px;"
                "  font-weight: 400;"
                "  text-align: center;"
                "}"
                "QPushButton:hover { background: %3; }"
                "QPushButton:disabled {"
                "  background: transparent;"
                "  color: %1;"
                "}")
                .arg(text_color)
                .arg(kButtonRadius)
                .arg(QString::fromLatin1(kColorHoverOverlay)));
        }
        btn->setIcon(QIcon(renderCameraIcon(kIconSize, text_color)));
    };

    const bool left_active = (active_ == QStringLiteral("left"));
    apply(btn_left_, left_active);
    apply(btn_right_, !left_active);
}

}  // namespace f2c_cpp
