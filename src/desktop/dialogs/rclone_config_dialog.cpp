// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "rclone_config_dialog.hpp"

#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtGui/QTextCursor>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QVBoxLayout>

#include "app_context.hpp"
#include "nesva/config/settings_store.hpp"

namespace nesva::ui {
namespace {

/// rclone prints coloured menus; a transcript does not need the escape codes.
QString stripAnsi(const QString& text) {
    static const QRegularExpression ansi(QStringLiteral("\\x1B\\[[0-9;]*[A-Za-z]"));
    QString cleaned = text;
    cleaned.remove(ansi);
    return cleaned;
}

QString guideHtml(const QString& remoteName) {
    return QStringLiteral(
        "<h3>Create the remote in rclone</h3>"
        "<p>This is rclone's own configuration; the password and the two-factor code go straight "
        "into rclone and are never stored by Nesva.</p>"
        "<ol>"
        "<li><b>New remote?</b> &rarr; <code>n</code></li>"
        "<li><b>name&gt;</b> &rarr; <code>%1</code> (the name you will use as <code>%1:</code>)</li>"
        "<li><b>Storage&gt;</b> &rarr; type <code>iclouddrive</code> and press Enter "
        "(for the family photos you can pick <code>photos</code> later in another remote)</li>"
        "<li><b>service&gt;</b> &rarr; <code>drive</code> for iCloud Drive</li>"
        "<li><b>apple_id&gt;</b> &rarr; the Apple ID of the account that owns the storage</li>"
        "<li><b>password&gt;</b> &rarr; the Apple ID password (not an app-specific password: the "
        "web API Nesva uses requires the real one plus 2FA)</li>"
        "<li><b>2FA code</b> &rarr; the six digits Apple shows on your trusted device</li>"
        "<li><b>Edit advanced config?</b> &rarr; <code>n</code></li>"
        "<li><b>Keep this remote?</b> &rarr; <code>y</code></li>"
        "<li><b>Quit config</b> &rarr; <code>q</code></li>"
        "</ol>"
        "<p>When it finishes, press <b>Check</b> in the dialog and the run continues.</p>"
        "<h3>If it fails with 401 / &quot;unauthorised&quot;</h3>"
        "<p>Apple invalidates the saved session when the password changes, when two-factor is "
        "reset, or simply after a few weeks. Run this wizard again (or <code>rclone config "
        "update %1</code>) and enter a fresh 2FA code. Nesva marks the storage provider as "
        "needing re-authentication and refuses to start a run until it is fixed, instead of "
        "failing file by file.</p>")
        .arg(remoteName.toHtmlEscaped());
}

} // namespace

RcloneConfigDialog::RcloneConfigDialog(AppContext& context, const QString& remoteName,
                                       QWidget* parent)
    : QDialog(parent), context_(context), remoteName_(remoteName) {
    setWindowTitle(QStringLiteral("Configure Rclone (%1)").arg(remoteName));
    setModal(true);
    resize(980, 620);

    executable_ = context_.settings()
                      .getString(QString::fromUtf8(config::keys::kRcloneBinaryPath),
                                 QStringLiteral("rclone"))
                      .valueOr(QStringLiteral("rclone"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    guide_ = new QTextBrowser(splitter);
    guide_->setHtml(guideHtml(remoteName_));
    guide_->setOpenExternalLinks(true);
    guide_->setMinimumWidth(320);
    splitter->addWidget(guide_);

    auto* sessionPanel = new QWidget(splitter);
    auto* sessionLayout = new QVBoxLayout(sessionPanel);
    sessionLayout->setContentsMargins(8, 0, 0, 0);
    sessionLayout->setSpacing(8);

    transcript_ = new QPlainTextEdit(sessionPanel);
    transcript_->setReadOnly(true);
    transcript_->setLineWrapMode(QPlainTextEdit::NoWrap);
    transcript_->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px;"));
    sessionLayout->addWidget(transcript_, 1);

    choiceBar_ = new QWidget(sessionPanel);
    choiceLayout_ = new QHBoxLayout(choiceBar_);
    choiceLayout_->setContentsMargins(0, 0, 0, 0);
    choiceLayout_->setSpacing(6);
    choiceLayout_->addStretch(1);
    sessionLayout->addWidget(choiceBar_);

    auto* inputRow = new QHBoxLayout();
    input_ = new QLineEdit(sessionPanel);
    input_->setObjectName(QStringLiteral("rcloneInput"));
    input_->setPlaceholderText(QStringLiteral("Type the answer rclone is waiting for, then Enter"));
    sendButton_ = new QPushButton(QStringLiteral("Send"), sessionPanel);
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(sendButton_);
    sessionLayout->addLayout(inputRow);

    splitter->addWidget(sessionPanel);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    statusLabel_->setWordWrap(true);
    root->addWidget(statusLabel_);

    auto* buttons = new QHBoxLayout();
    restartButton_ = new QPushButton(QStringLiteral("Restart session"), this);
    closeButton_ = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(restartButton_);
    buttons->addStretch(1);
    buttons->addWidget(closeButton_);
    root->addLayout(buttons);

    connect(sendButton_, &QPushButton::clicked, this, [this] { sendLine(input_->text()); });
    connect(input_, &QLineEdit::returnPressed, this, [this] { sendLine(input_->text()); });
    connect(restartButton_, &QPushButton::clicked, this, [this] {
        if (process_ != nullptr) {
            return;
        }
        transcript_->clear();
        startSession();
    });
    connect(closeButton_, &QPushButton::clicked, this, [this] {
        closing_ = true;
        if (process_ != nullptr) {
            process_->kill();
            process_->waitForFinished(2000);
        }
        reject();
    });

    startSession();
}

void RcloneConfigDialog::startSession() {
    const QString resolved = QStandardPaths::findExecutable(executable_);
    if (resolved.isEmpty()) {
        statusLabel_->setText(
            QStringLiteral("rclone was not found (%1). Install it from Settings -> External "
                           "tools first.").arg(executable_));
        input_->setEnabled(false);
        sendButton_->setEnabled(false);
        return;
    }

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::readyRead, this, [this] { readOutput(); });
    connect(process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        readOutput();
        finishSession(exitCode, status == QProcess::CrashExit);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            statusLabel_->setText(QStringLiteral("rclone could not be started."));
            finishSession(-1, true);
        }
    });

    process_->start(resolved, {QStringLiteral("config")});
    statusLabel_->setText(QStringLiteral("Running 'rclone config'. Follow the steps on the left; "
                                        "answer here on the right."));
    input_->setEnabled(true);
    sendButton_->setEnabled(true);
    restartButton_->setEnabled(false);
    input_->setFocus();
}

void RcloneConfigDialog::readOutput() {
    if (process_ == nullptr) {
        return;
    }
    const QString chunk = stripAnsi(QString::fromLocal8Bit(process_->readAll()));
    if (chunk.isEmpty()) {
        return;
    }
    pendingOutput_ += chunk;
    transcript_->moveCursor(QTextCursor::End);
    transcript_->insertPlainText(chunk);
    transcript_->verticalScrollBar()->setValue(transcript_->verticalScrollBar()->maximum());

    // The last meaningful line is the question the user has to answer.
    QStringList lines = pendingOutput_.split(QLatin1Char('\n'));
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
        lines.removeLast();
    }
    if (!lines.isEmpty()) {
        offerChoices(lines.last().trimmed());
    }
}

void RcloneConfigDialog::offerChoices(const QString& prompt) {
    while (QLayoutItem* item = choiceLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    QStringList choices;
    static const QRegularExpression choicePrompt(
        QStringLiteral("^([A-Za-z0-9]+(?:/[A-Za-z0-9]+)+)>\\s*$"));
    const QRegularExpressionMatch match = choicePrompt.match(prompt);
    if (match.hasMatch()) {
        choices = match.captured(1).split(QLatin1Char('/'));
    }
    for (const QString& choice : choices) {
        auto* button = new QPushButton(choice, choiceBar_);
        button->setMinimumWidth(44);
        connect(button, &QPushButton::clicked, this, [this, choice] { sendLine(choice); });
        choiceLayout_->addWidget(button);
    }
    choiceLayout_->addStretch(1);
    choiceBar_->setVisible(!choices.isEmpty());
}

void RcloneConfigDialog::sendLine(const QString& text) {
    if (process_ == nullptr) {
        return;
    }
    process_->write(text.toUtf8() + '\n');
    transcript_->moveCursor(QTextCursor::End);
    transcript_->insertPlainText(text + QLatin1Char('\n'));
    pendingOutput_.clear();
    input_->clear();
    input_->setFocus();
}

void RcloneConfigDialog::finishSession(int exitCode, bool crashed) {
    if (process_ != nullptr) {
        process_->deleteLater();
        process_ = nullptr;
    }
    input_->setEnabled(false);
    sendButton_->setEnabled(false);
    restartButton_->setEnabled(true);
    choiceBar_->setVisible(false);

    if (closing_) {
        return;
    }
    if (crashed || exitCode != 0) {
        statusLabel_->setText(
            QStringLiteral("rclone config ended without saving (exit code %1). Nothing was "
                           "changed; restart the session to try again.").arg(exitCode));
        return;
    }
    configured_ = true;
    statusLabel_->setText(QStringLiteral("Remote '%1' is configured. Close this dialog and press "
                                         "Refresh on the Storage page.").arg(remoteName_));
}

} // namespace nesva::ui
