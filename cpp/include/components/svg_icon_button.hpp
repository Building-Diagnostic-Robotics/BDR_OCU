/**
 * @file svg_icon_button.hpp
 * @brief QPushButton with a runtime-tinted SVG icon. Uses Qt's standard
 *        QPushButton::setIcon path — no custom paintEvent override, no
 *        CompositionMode, no QLabel pixmap-on-pixmap.
 *
 * Why this exists:
 *   The first two attempts at this widget (custom paintEvent on QPushButton,
 *   then QLabel::setPixmap) both produced blank icons at rest in this app's
 *   QStyleSheetStyle environment on Qt 5.15.3. setIcon goes through Qt's
 *   standard button-icon machinery and renders identically across themes.
 *
 * Coloring:
 *   The source SVG is rewritten to a target stroke color and re-rendered
 *   into a fresh QPixmap on every theme/hover change. Mirrors the byte
 *   substitution `loadSvgPixmap` in planner_screen.cpp uses (currentColor
 *   replacement + Figma `var(--stroke-N, #fallback)` regex).
 */

#pragma once

#include <QByteArray>
#include <QColor>
#include <QPushButton>
#include <QPixmap>
#include <QString>

class QEvent;

namespace f2c_cpp {

class SvgIconButton : public QPushButton {
    Q_OBJECT

public:
    struct Palette {
        QColor dark_resting;
        QColor dark_hover;
        QColor light_resting;
        QColor light_hover;
    };

    SvgIconButton(const QString& svg_resource_path,
                  const Palette& palette,
                  int side_px,
                  QWidget* parent = nullptr);

    void setDarkMode(bool dark_mode);

protected:
    bool event(QEvent* e) override;

private:
    QPixmap renderTinted(const QColor& color) const;
    void refreshIcon();

    QString svg_text_;          // raw SVG, UTF-8 string for regex/substring ops
    bool svg_loaded_ = false;
    Palette palette_;
    int side_px_;
    bool dark_mode_ = true;
    bool hovered_ = false;
};

}  // namespace f2c_cpp
