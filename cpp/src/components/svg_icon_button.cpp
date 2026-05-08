/**
 * @file svg_icon_button.cpp
 */

#include "components/svg_icon_button.hpp"

#include <QEvent>
#include <QFile>
#include <QIcon>
#include <QPainter>
#include <QRegularExpression>
#include <QSize>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QSvgRenderer>
#else
#include <QtSvg/QSvgRenderer>
#endif

namespace f2c_cpp {

SvgIconButton::SvgIconButton(const QString& svg_resource_path,
                             const Palette& palette,
                             int side_px,
                             QWidget* parent)
    : QPushButton(parent),
      palette_(palette),
      side_px_(side_px) {
    setFlat(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(side_px_, side_px_);
    setIconSize(QSize(side_px_, side_px_));
    // Background-only stylesheet — leaves Qt's icon paint path entirely
    // alone. Earlier attempts that added `border: none; padding: 0;`
    // appear to have interacted badly with QStyleSheetStyle's button paint
    // path on Qt 5.15.3. Just kill the background; let the rest go through
    // Qt's standard QStyle.
    setStyleSheet(QStringLiteral("QPushButton { background: transparent; }"));

    QFile f(svg_resource_path);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray data = f.readAll();
        f.close();
        svg_text_ = QString::fromUtf8(data);
        svg_loaded_ = !svg_text_.isEmpty();
    }
    refreshIcon();
}

QPixmap SvgIconButton::renderTinted(const QColor& color) const {
    if (!svg_loaded_) {
        return QPixmap();
    }
    // Two substitutions, mirroring planner_screen.cpp::loadSvgPixmap (the
    // pattern already used everywhere else in this app):
    //   1) Lucide source SVGs use `stroke="currentColor"` — swap the
    //      keyword for the concrete target hex.
    //   2) The codebase's existing icon set uses Figma's
    //      `stroke="var(--stroke-N, #fallback)"` — swap the entire var()
    //      call for the target hex.
    const QString hex = color.name(QColor::HexRgb);
    QString svg = svg_text_;
    svg.replace(QStringLiteral("currentColor"), hex);
    static const QRegularExpression kFigmaVarColorPattern(
        QStringLiteral(R"(var\(--(?:fill|stroke)-\d+,\s*(#[0-9A-Fa-f]{3,8})\s*\))"));
    svg.replace(kFigmaVarColorPattern, hex);

    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid()) {
        return QPixmap();
    }
    QPixmap pix(side_px_, side_px_);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&p);
    return pix;
}

void SvgIconButton::refreshIcon() {
    if (!svg_loaded_) {
        return;
    }
    const QColor target = dark_mode_
        ? (hovered_ ? palette_.dark_hover : palette_.dark_resting)
        : (hovered_ ? palette_.light_hover : palette_.light_resting);
    QPixmap pix = renderTinted(target);
    if (pix.isNull()) {
        return;
    }
    setIcon(QIcon(pix));
}

void SvgIconButton::setDarkMode(bool dark_mode) {
    if (dark_mode_ == dark_mode) {
        return;
    }
    dark_mode_ = dark_mode;
    refreshIcon();
}

bool SvgIconButton::event(QEvent* e) {
    switch (e->type()) {
        case QEvent::Enter:
            if (!hovered_) {
                hovered_ = true;
                refreshIcon();
            }
            break;
        case QEvent::Leave:
            if (hovered_) {
                hovered_ = false;
                refreshIcon();
            }
            break;
        default:
            break;
    }
    return QPushButton::event(e);
}

}  // namespace f2c_cpp
