#pragma once

#include <QObject>
#include <QPointer>

class QAbstractScrollArea;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QEvent;

namespace f2c_cpp {

// Auto-hiding, faded vertical scroll bar attached to any QAbstractScrollArea
// (QScrollArea, QListWidget, QPlainTextEdit, QTextEdit, etc.).
//
// Behavior:
//   - Bar is fully transparent at rest (opacity 0).
//   - Mouse Enter on the area / viewport / bar triggers a 300ms fade-in.
//   - Mouse Leave (when cursor exits the area entirely) triggers a 300ms
//     fade-out.
//   - Pressed thumb is amber (#F59E0B); resting/hover thumb is gray
//     (#71717A).
//   - 8px wide, 4px radius, 20px min thumb height. Horizontal bar hidden.
//
// Theme: light/dark via setDarkMode(). Track-hover color follows theme.
class AutoHideScrollBar : public QObject {
    Q_OBJECT

public:
    // Convenience installer. Creates a helper parented to `area`. The helper
    // self-manages and is destroyed with the area.
    static void install(QAbstractScrollArea* area, bool dark_mode = true);

    explicit AutoHideScrollBar(QAbstractScrollArea* area);
    void setDarkMode(bool dark_mode);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void applyStyleSheet();
    void fade(double target);

    QPointer<QAbstractScrollArea> area_;
    QGraphicsOpacityEffect* opacity_effect_ = nullptr;
    QPropertyAnimation* animation_ = nullptr;
    bool dark_mode_ = true;
};

}  // namespace f2c_cpp
