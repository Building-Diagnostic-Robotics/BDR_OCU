// Tooltip theme: every tooltip carries the theme rule as its own sheet, so a
// catch-all rule on the widget it is shown for cannot restyle it.
#include "tooltip_theme.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QToolTip>
#include <gtest/gtest.h>

using f2c_cpp::applyTooltipTheme;
using f2c_cpp::tooltipStyleSheet;

namespace {

QWidget* visibleTip() {
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->inherits("QTipLabel") && w->isVisible()) return w;
    }
    return nullptr;
}

QWidget* showTip(QWidget* host) {
    QToolTip::showText(QPoint(20, 20), QStringLiteral("2 sections · 1 uploaded"), host);
    QElapsedTimer waited;
    waited.start();
    while (!visibleTip() && waited.elapsed() < 2000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return visibleTip();
}

}  // namespace

TEST(TooltipTheme, SheetUsesThemeTokensAndDiffersByTheme) {
    EXPECT_TRUE(tooltipStyleSheet(false).startsWith("QToolTip {"));
    EXPECT_NE(tooltipStyleSheet(false), tooltipStyleSheet(true));
}

TEST(TooltipTheme, OverridesCatchAllRuleOnTheHostWidget) {
    for (const bool dark : {false, true}) {
        applyTooltipTheme(dark);
        QLabel host(QStringLiteral("host"));
        host.setStyleSheet(QStringLiteral("color: white; background: #444; font-size: 22px;"));
        host.show();
        QWidget* tip = showTip(&host);
        ASSERT_NE(tip, nullptr);
        EXPECT_EQ(tip->styleSheet(), tooltipStyleSheet(dark));
        QToolTip::hideText();
        QApplication::processEvents();
    }
}

TEST(TooltipTheme, AppliesWithoutAnyHostSheetAndFollowsThemeSwitch) {
    QLabel host(QStringLiteral("host"));
    host.show();
    applyTooltipTheme(true);
    QWidget* tip = showTip(&host);
    ASSERT_NE(tip, nullptr);
    EXPECT_EQ(tip->styleSheet(), tooltipStyleSheet(true));
    QToolTip::hideText();
    QApplication::processEvents();
    applyTooltipTheme(false);
    tip = showTip(nullptr);
    ASSERT_NE(tip, nullptr);
    EXPECT_EQ(tip->styleSheet(), tooltipStyleSheet(false));
    QToolTip::hideText();
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
