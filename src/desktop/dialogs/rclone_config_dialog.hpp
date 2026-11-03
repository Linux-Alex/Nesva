// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QDialog>

class QComboBox;
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

/// A supported rclone backend with the walkthrough for connecting it.
struct RcloneProviderProfile {
    QString key;      ///< stable id used by the combo box
    QString label;    ///< shown in the UI
    QString backend;  ///< value to type at rclone's "Storage>" prompt
    QString remote;   ///< suggested remote name ("drive", "b2", ...)
    QString html;     ///< the tutorial shown on the left
};

QVector<RcloneProviderProfile> rcloneProviderProfiles();

/// Guided rclone config session.
///
/// The left side is the walkthrough for the chosen provider; the right side is a
/// live "rclone config" session the user finishes by typing, including the
/// secrets and two-factor codes that only a human can supply. Nesva never learns
/// them: they go straight into rclone's own configuration.
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
    void offerChoices();
    void finishSession(int exitCode, bool crashed);
    void applyProfile(int index);
    const RcloneProviderProfile* profile() const;

    AppContext& context_;
    QVector<RcloneProviderProfile> profiles_;
    QString remoteName_;
    QString executable_;
    QString lastOption_;

    QComboBox* providerCombo_ = nullptr;
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
    QString pendingOutput_; ///< text seen since the last answer, used to spot the prompt
    bool configured_ = false;
    bool closing_ = false;
};

} // namespace nesva::ui
