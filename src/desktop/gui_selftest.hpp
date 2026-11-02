// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>

namespace nesva::ui {

class MainWindow;

/// Drives the real main window through every page and writes one screenshot per
/// page into \p outputDirectory. Used by scripts and CI to prove that the GUI
/// actually renders, including on offscreen platforms.
///
/// \return 0 when the window was visible and every page produced a screenshot.
int runGuiSelfTest(MainWindow& window, const QString& outputDirectory);

} // namespace nesva::ui
