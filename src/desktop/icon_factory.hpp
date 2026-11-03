// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtGui/QColor>
#include <QtGui/QIcon>

namespace nesva::ui {

/// Navigation glyphs, drawn programmatically so that the icon set is
/// theme-aware, vector-crisp and free of external asset dependencies.
enum class Glyph {
    Dashboard,
    Jobs,
    Storage,
    Archive,
    Snapshots,
    Restore,
    Activity,
    Mail,
    Settings,
};

/// Builds an icon for the glyph in the given stroke colour.
QIcon makeGlyphIcon(Glyph glyph, const QColor& color, int pixelSize = 24);

/// Application/window icon.
QIcon applicationIcon();

} // namespace nesva::ui
