// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>
#include <QtWidgets/QDialog>

#include "nesva/tools/tool_installer.hpp"

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace nesva::ui {

class AppContext;

/// Shows exactly what installing an external tool (Rclone or Restic) will do -
/// the download URL, the checksum, the unpack command, the target path and, for
/// a system-wide install, the elevated command - and only then offers to
/// perform it.
///
/// Nothing happens until the user presses Install, and the plan is rendered
/// from the same data the installer executes, so the preview cannot drift from
/// the real commands.
class ToolInstallDialog : public QDialog {
    Q_OBJECT

public:
    explicit ToolInstallDialog(AppContext& context,
                               tools::ToolId tool = tools::ToolId::Rclone,
                               QWidget* parent = nullptr);

    /// Absolute path of the binary that was installed, empty when nothing was.
    QString installedPath() const { return installedPath_; }

private:
    void requestPlan();
    void renderPlan();
    void showPlaceholder(const QString& message);
    void performInstall();
    void appendLog(const QString& line);
    void finishWithError(const QString& message);

    AppContext& context_;
    tools::ToolId tool_ = tools::ToolId::Rclone;
    QComboBox* scopeCombo_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPlainTextEdit* planView_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* installButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;

    tools::ToolInstallPlan plan_;
    bool planReady_ = false;
    bool busy_ = false;
    QString installedPath_;
};

} // namespace nesva::ui
