// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QDialog>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QTextBrowser;
class QVBoxLayout;
class QHBoxLayout;
class QWidget;

namespace nesva::ui {

class AppContext;

/// Guided rclone config session.
///
/// The left side is the walkthrough (what to answer, in order); the right side
/// is a live session of "rclone config" that the user finishes by typing,
/// including the Apple ID password and the two-factor code that only a human can
/// supply. Nesva never learns those secrets: they go straight into rclone's own
/// configuration.
///
/// rclone's questions are line based, so a transcript plus an input line is a
/// faithful terminal and needs no PTY or terminal emulator on any platform.
class RcloneConfigDialog : public QDialog {
    Q_OBJECT

public:
    explicit RcloneConfigDialog(AppContext& context, const QString& remoteName,
                                QWidget* parent = nullptr);

    /// True when rclone finished successfully, i.e. the remote was written.
    bool configured() const { return configured_; }

private:
    void startSession();
    void readOutput();
    void sendLine(const QString& text);
    void offerChoices(const QString& prompt);
    void finishSession(int exitCode, bool crashed);

    AppContext& context_;
    QString remoteName_;
    QString executable_;

    QTextBrowser* guide_ = nullptr;
    QPlainTextEdit* transcript_ = nullptr;
    QLineEdit* input_ = nullptr;
    QWidget* choiceBar_ = nullptr;
    QHBoxLayout* choiceLayout_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QPushButton* restartButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    QProcess* process_ = nullptr;
    QString pendingOutput_;
    bool configured_ = false;
    bool closing_ = false;
};

} // namespace nesva::ui
