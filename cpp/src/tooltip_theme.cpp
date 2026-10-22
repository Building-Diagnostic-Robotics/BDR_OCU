#include "tooltip_theme.hpp"

#include "ui_theme_constants.hpp"

#include <QApplication>
#include <QEvent>
#include <QPalette>
#include <QToolTip>

namespace f2c_cpp {

namespace {

constexpr int kTooltipPadding = 8;   // px around the text
constexpr int kTooltipFontPx = 14;

/// Every tooltip is a QTipLabel parented nowhere; Qt applies the style
/// sheets of the widget it was shown for. A catch-all rule there (a bare
/// `color: white; font-size: 22px` on a label) restyles the tooltip, so each
/// one gets this rule as its own sheet, which outranks anything inherited.
/// A new label is caught at Polish. When the widget it is shown for has a
/// style sheet, QToolTip::showText then sets the label's sheet to "/* */"
/// itself, right before sizing it; that arrives as StyleChange. Replacing
/// the sheet at both points means the size is always computed with ours.
class TooltipSheetFilter : public QObject {
public:
    using QObject::QObject;
    QString sheet;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        const QEvent::Type type = event->type();
        if ((type == QEvent::Polish || type == QEvent::StyleChange) &&
            !sheet.isEmpty() && watched->inherits("QTipLabel")) {
            auto* tip = static_cast<QWidget*>(watched);
            if (tip->styleSheet() != sheet) tip->setStyleSheet(sheet);
        }
        return QObject::eventFilter(watched, event);
    }
};

TooltipSheetFilter* sheetFilter() {
    static TooltipSheetFilter* filter = [] {
        auto* f = new TooltipSheetFilter(qApp);
        qApp->installEventFilter(f);
        return f;
    }();
    return filter;
}

}  // namespace

QString tooltipStyleSheet(bool dark_mode) {
    const UiThemeTokens t = uiThemeTokens(dark_mode);
    return QStringLiteral(
               "QToolTip { background-color: %1; color: %2; border: 1px solid %3; "
               "padding: %4px; font-size: %5px; }")
        .arg(t.surface, t.text, t.raised_border)
        .arg(kTooltipPadding)
        .arg(kTooltipFontPx);
}

void applyTooltipTheme(bool dark_mode) {
    // Palette and font too, for anything drawn before the sheet lands.
    const UiThemeTokens t = uiThemeTokens(dark_mode);
    QPalette palette = QToolTip::palette();
    for (const QPalette::ColorGroup group :
         {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        palette.setColor(group, QPalette::ToolTipBase, QColor(t.surface));
        palette.setColor(group, QPalette::ToolTipText, QColor(t.text));
    }
    QToolTip::setPalette(palette);
    QFont font = QToolTip::font();
    font.setPixelSize(kTooltipFontPx);
    QToolTip::setFont(font);
    if (qApp) sheetFilter()->sheet = tooltipStyleSheet(dark_mode);
}

}  // namespace f2c_cpp
