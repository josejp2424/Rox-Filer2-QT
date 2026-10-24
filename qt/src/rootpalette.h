#pragma once
/*
 * ROX-Filer2-QT 1.0.1: privileged-window chrome theme bridge.
 * The stylesheet contains ONLY colours from the effective user's Qt palette.
 * It is evaluated once on launch and never reads the user's Qt config or loads
 * user-installed QStyle plugins in the privileged process.
 */
#include <QApplication>
#include <QPalette>
#include <QColor>
#include <QString>

namespace RoxRootPalette {
inline QString hex(const QColor &color)
{
    return color.name(QColor::HexRgb); // no user-provided CSS or URLs
}
inline void applyChrome(QApplication &app, const QPalette &palette)
{
    const QString window = hex(palette.color(QPalette::Active, QPalette::Window));
    const QString windowText = hex(palette.color(QPalette::Active, QPalette::WindowText));
    const QString button = hex(palette.color(QPalette::Active, QPalette::Button));
    const QString buttonText = hex(palette.color(QPalette::Active, QPalette::ButtonText));
    const QString base = hex(palette.color(QPalette::Active, QPalette::Base));
    const QString text = hex(palette.color(QPalette::Active, QPalette::Text));
    const QString selected = hex(palette.color(QPalette::Active, QPalette::Highlight));
    const QString selectedText = hex(palette.color(QPalette::Active, QPalette::HighlightedText));
    // Qt's platform theme may otherwise repaint these chrome surfaces white
    // while the actual file view already has the correct dark palette.
    const QString css = QStringLiteral(
        "QMenuBar, QToolBar, QStatusBar { background-color: %1; color: %2; }"
        "QMenuBar::item { background-color: transparent; color: %2; }"
        "QMenuBar::item:selected { background-color: %7; color: %8; }"
        "QMenu { background-color: %5; color: %6; }"
        "QMenu::item:selected { background-color: %7; color: %8; }"
        "QToolButton { background-color: %3; color: %4; }"
        "QToolButton:checked, QToolButton:hover { background-color: %7; color: %8; }"
        "QTabBar::tab { background-color: %3; color: %4; }"
        "QTabBar::tab:selected { background-color: %1; color: %2; }"
        "QHeaderView::section { background-color: %3; color: %4; }"
        "QDockWidget::title { background-color: %1; color: %2; }"
        "QToolTip { background-color: %5; color: %6; }"
    ).arg(window, windowText, button, buttonText, base, text, selected, selectedText);
    app.setStyleSheet(css);
}
} // namespace RoxRootPalette
