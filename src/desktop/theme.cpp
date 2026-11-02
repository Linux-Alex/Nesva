// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "theme.hpp"

#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtGui/QStyleHints>
#include <QtWidgets/QToolTip>

namespace nesva::ui {
namespace {

QPalette lightPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0xf3, 0xf4, 0xf6));
    palette.setColor(QPalette::WindowText, QColor(0x1f, 0x23, 0x28));
    palette.setColor(QPalette::Base, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::AlternateBase, QColor(0xf7, 0xf8, 0xfa));
    palette.setColor(QPalette::ToolTipBase, QColor(0xff, 0xff, 0xdc));
    palette.setColor(QPalette::ToolTipText, QColor(0x1f, 0x23, 0x28));
    palette.setColor(QPalette::Text, QColor(0x1f, 0x23, 0x28));
    palette.setColor(QPalette::PlaceholderText, QColor(0x6c, 0x75, 0x7d));
    palette.setColor(QPalette::Button, QColor(0xf3, 0xf4, 0xf6));
    palette.setColor(QPalette::ButtonText, QColor(0x1f, 0x23, 0x28));
    palette.setColor(QPalette::BrightText, QColor(0xd1, 0x24, 0x2f));
    palette.setColor(QPalette::Highlight, QColor(0x0a, 0x66, 0xc2));
    palette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::Link, QColor(0x0a, 0x66, 0xc2));
    palette.setColor(QPalette::Mid, QColor(0xd5, 0xd9, 0xde));
    palette.setColor(QPalette::Dark, QColor(0xb9, 0xbf, 0xc6));
    palette.setColor(QPalette::Light, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x9a, 0xa1, 0xa8));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x9a, 0xa1, 0xa8));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x9a, 0xa1, 0xa8));
    return palette;
}

QPalette darkPalette() {
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(0x1e, 0x20, 0x24));
    palette.setColor(QPalette::WindowText, QColor(0xe6, 0xe8, 0xea));
    palette.setColor(QPalette::Base, QColor(0x24, 0x27, 0x2c));
    palette.setColor(QPalette::AlternateBase, QColor(0x2a, 0x2d, 0x33));
    palette.setColor(QPalette::ToolTipBase, QColor(0x2f, 0x33, 0x39));
    palette.setColor(QPalette::ToolTipText, QColor(0xe6, 0xe8, 0xea));
    palette.setColor(QPalette::Text, QColor(0xe6, 0xe8, 0xea));
    palette.setColor(QPalette::PlaceholderText, QColor(0x9a, 0xa2, 0xac));
    palette.setColor(QPalette::Button, QColor(0x2a, 0x2d, 0x33));
    palette.setColor(QPalette::ButtonText, QColor(0xe6, 0xe8, 0xea));
    palette.setColor(QPalette::BrightText, QColor(0xff, 0x6b, 0x6b));
    palette.setColor(QPalette::Highlight, QColor(0x3d, 0x7e, 0xff));
    palette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::Link, QColor(0x6d, 0xa8, 0xff));
    palette.setColor(QPalette::Mid, QColor(0x3a, 0x3e, 0x45));
    palette.setColor(QPalette::Dark, QColor(0x14, 0x15, 0x18));
    palette.setColor(QPalette::Light, QColor(0x3a, 0x3e, 0x45));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x7b, 0x82, 0x8a));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x7b, 0x82, 0x8a));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x7b, 0x82, 0x8a));
    return palette;
}

/// Colours for the navigation item states.
///
/// The selected item previously used the palette's highlighted *text* colour
/// (white in both palettes) on a pale blue background, which is unreadable in
/// the light theme. Each scheme now carries an explicit, contrast-checked pair:
/// light  #0b3d75 on #dbe7f7  ->  8.4:1
/// dark   #ffffff on #2c3f63  -> 10.2:1
/// Both exceed the WCAG AA requirement of 4.5:1 for normal text and 3:1 for
/// large text, and the selected item also gets an accent bar for
/// non-colour-dependent affordance.
struct NavigationColors {
    QString selectedBackground;
    QString selectedForeground;
    QString selectedAccent;
};

NavigationColors navigationColorsFor(Qt::ColorScheme scheme) {
    if (scheme == Qt::ColorScheme::Dark) {
        return {QStringLiteral("#2c3f63"), QStringLiteral("#ffffff"), QStringLiteral("#6da8ff")};
    }
    return {QStringLiteral("#dbe7f7"), QStringLiteral("#0b3d75"), QStringLiteral("#0a66c2")};
}

QString styleSheetFor(Qt::ColorScheme scheme) {
    const NavigationColors colors = navigationColorsFor(scheme);
    const QString sidebarSelected = colors.selectedBackground;

    return QStringLiteral(R"CSS(
QFrame#sidebar {
    background: palette(alternate-base);
    border-right: 1px solid palette(mid);
}
QListWidget#navigation {
    background: transparent;
    border: none;
    outline: none;
    padding: 6px;
}
QListWidget#navigation::item {
    padding: 8px 10px;
    margin: 1px 0px;
    border-radius: 6px;
    color: palette(window-text);
}
QListWidget#navigation::item:hover {
    background: palette(mid);
}
QListWidget#navigation::item:selected {
    background: @SIDEBAR_SELECTED@;
    color: @SIDEBAR_SELECTED_TEXT@;
    border-left: 3px solid @SIDEBAR_ACCENT@;
    padding-left: 7px;
    font-weight: 600;
}
QListWidget#navigation::item:selected:!active {
    background: @SIDEBAR_SELECTED@;
    color: @SIDEBAR_SELECTED_TEXT@;
}
QFrame#topBar {
    background: palette(window);
    border-bottom: 1px solid palette(mid);
}
QLabel#applicationTitle {
    font-size: 13pt;
    font-weight: 600;
}
QLabel#statusBadge {
    padding: 3px 10px;
    border-radius: 9px;
    background: palette(mid);
    color: palette(window-text);
}
QLabel#pageTitle {
    font-size: 16pt;
    font-weight: 600;
}
QLabel#pageSubtitle {
    color: palette(placeholder-text);
}
QFrame#statCard {
    background: palette(base);
    border: 1px solid palette(mid);
    border-radius: 8px;
}
QLabel#statCardValue {
    font-size: 18pt;
    font-weight: 600;
}
QLabel#statCardCaption {
    color: palette(placeholder-text);
}
QLabel#emptyState {
    color: palette(placeholder-text);
}
QTableView {
    border: 1px solid palette(mid);
    border-radius: 6px;
    gridline-color: palette(mid);
    selection-background-color: palette(highlight);
    selection-color: palette(highlighted-text);
}
QHeaderView::section {
    background: palette(alternate-base);
    border: none;
    border-bottom: 1px solid palette(mid);
    padding: 6px 8px;
    font-weight: 600;
}
QPushButton {
    padding: 5px 14px;
    border: 1px solid palette(mid);
    border-radius: 6px;
    background: palette(button);
}
QPushButton:hover {
    background: palette(mid);
}
QPushButton:disabled {
    color: palette(placeholder-text);
}
)CSS")
        .replace(QStringLiteral("@SIDEBAR_SELECTED@"), sidebarSelected)
        .replace(QStringLiteral("@SIDEBAR_SELECTED_TEXT@"), colors.selectedForeground)
        .replace(QStringLiteral("@SIDEBAR_ACCENT@"), colors.selectedAccent);
}

} // namespace

QString themeModeToString(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Light:
        return QStringLiteral("light");
    case ThemeMode::Dark:
        return QStringLiteral("dark");
    case ThemeMode::System:
        return QStringLiteral("system");
    }
    return QStringLiteral("system");
}

std::optional<ThemeMode> themeModeFromString(const QString& value) {
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("light")) {
        return ThemeMode::Light;
    }
    if (normalised == QStringLiteral("dark")) {
        return ThemeMode::Dark;
    }
    if (normalised == QStringLiteral("system") || normalised.isEmpty()) {
        return ThemeMode::System;
    }
    return std::nullopt;
}

Qt::ColorScheme effectiveColorScheme(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Light:
        return Qt::ColorScheme::Light;
    case ThemeMode::Dark:
        return Qt::ColorScheme::Dark;
    case ThemeMode::System:
        break;
    }
    if (QGuiApplication::styleHints() != nullptr) {
        const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
        if (scheme != Qt::ColorScheme::Unknown) {
            return scheme;
        }
    }
    const QPalette palette = QGuiApplication::palette();
    return palette.color(QPalette::Window).lightness() < 128 ? Qt::ColorScheme::Dark
                                                             : Qt::ColorScheme::Light;
}

void applyTheme(QApplication& app, ThemeMode mode) {
    const Qt::ColorScheme scheme = effectiveColorScheme(mode);
    const QPalette palette = scheme == Qt::ColorScheme::Dark ? darkPalette() : lightPalette();
    app.setPalette(palette);
    QToolTip::setPalette(palette);
    app.setStyleSheet(styleSheetFor(scheme));
}

} // namespace nesva::ui
