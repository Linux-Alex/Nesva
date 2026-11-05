// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "rclone_config_dialog.hpp"

#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <QtWidgets/QComboBox>
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

} // namespace

QVector<RcloneProviderProfile> rcloneProviderProfiles() {
    QVector<RcloneProviderProfile> profiles;

    profiles.append({QStringLiteral("iclouddrive"), QStringLiteral("iCloud Drive"),
                     QStringLiteral("iclouddrive"), QStringLiteral("icloud"),
                     QStringLiteral(
                         "<h3>iCloud Drive</h3>"
                         "<p>Apple has no public API, so rclone uses the iCloud web session: it needs the <b>real</b> Apple ID password (an app-specific password does not work) plus a two-factor code.</p>"
                         "<ol>"
                         "<li><b>New remote?</b> &rarr; <code>n</code> &nbsp; <b>name&gt;</b> &rarr; <code>icloud</code></li>"
                         "<li><b>Storage&gt;</b> &rarr; <code>iclouddrive</code></li>"
                         "<li><b>service&gt;</b> &rarr; <code>drive</code> (or <code>photos</code>)</li>"
                         "<li><b>apple_id&gt;</b> &rarr; your Apple ID</li>"
                         "<li><b>Option password</b> offers <code>y</code> / <code>g</code>: answer <code>y</code> <b>first</b>, then type the password at the next prompt</li>"
                         "<li><b>2FA code</b> &rarr; the six digits Apple shows</li>"
                         "<li><b>Edit advanced config?</b> &rarr; <code>n</code>, <b>Keep this remote?</b> &rarr; <code>y</code>, then <code>q</code></li>"
                         "</ol>"
                         "<p>The session lasts weeks, not forever: when it expires Nesva marks the provider as needing re-authentication. Run this wizard again for a fresh code.</p>"
                         )});
    profiles.append({QStringLiteral("drive"), QStringLiteral("Google Drive"),
                     QStringLiteral("drive"), QStringLiteral("gdrive"),
                     QStringLiteral(
                         "<h3>Google Drive</h3>"
                         "<p>Google rate-limits rclone shared client ID, so create your own OAuth client once:</p>"
                         "<ol>"
                         "<li><b>console.cloud.google.com</b> &rarr; new project (any name).</li>"
                         "<li><b>APIs &amp; Services &rarr; Library</b> &rarr; enable <b>Google Drive API</b>.</li>"
                         "<li><b>OAuth consent screen</b> &rarr; External &rarr; add your account under <i>Test users</i>.</li>"
                         "<li><b>Credentials &rarr; Create credentials &rarr; OAuth client ID</b> &rarr; type <b>Desktop app</b> &rarr; copy client ID and secret.</li>"
                         "</ol>"
                         "<p>Then in this session:</p>"
                         "<ol>"
                         "<li><code>n</code> &nbsp; <b>name&gt;</b> &rarr; <code>gdrive</code></li>"
                         "<li><b>Storage&gt;</b> &rarr; <code>drive</code></li>"
                         "<li><b>client_id</b> &rarr; paste it (do not leave it empty)</li>"
                         "<li><b>client_secret</b> &rarr; paste it</li>"
                         "<li><b>scope&gt;</b> &rarr; Enter (default <code>drive</code>)</li>"
                         "<li><b>service_account_file</b> &rarr; Enter; any later <b>client_id</b>/<b>token</b> prompt &rarr; Enter</li>"
                         "<li><b>Edit advanced config?</b> &rarr; <code>n</code></li>"
                         "<li><b>Use web browser to automatically authenticate?</b> &rarr; <code>y</code> &rarr; sign in, allow access</li>"
                         "<li><b>Keep this remote?</b> &rarr; <code>y</code>, then <code>q</code></li>"
                         "</ol>"
                         "<p>15 GB free, shared with Gmail and Photos. Use it as <code>gdrive:</code> or <code>gdrive:Nesva/Archive</code>.</p>"
                         )});
    profiles.append({QStringLiteral("pcloud"), QStringLiteral("pCloud"),
                     QStringLiteral("pcloud"), QStringLiteral("pcloud"),
                     QStringLiteral(
                         "<h3>pCloud</h3>"
                         "<p>Nothing to create in a console: the credentials are your pCloud account.</p>"
                         "<ol>"
                         "<li><code>n</code> &nbsp; <b>name&gt;</b> &rarr; <code>pcloud</code></li>"
                         "<li><b>Storage&gt;</b> &rarr; <code>pcloud</code></li>"
                         "<li><b>client_id</b> &rarr; Enter (empty uses rclone registered app)</li>"
                         "<li><b>client_secret</b> &rarr; Enter</li>"
                         "<li><b>Use web browser to automatically authenticate?</b> &rarr; <code>y</code> &rarr; sign in and allow access</li>"
                         "<li><b>Edit advanced config?</b> &rarr; <code>n</code>, <b>Keep this remote?</b> &rarr; <code>y</code>, then <code>q</code></li>"
                         "</ol>"
                         "<p>About 10 GB free. If asked for a <b>hostname</b>, <code>eapi.pcloud.com</code> keeps the data in the EU.</p>"
                         )});
    profiles.append({QStringLiteral("b2"), QStringLiteral("Backblaze B2"),
                     QStringLiteral("b2"), QStringLiteral("b2"),
                     QStringLiteral(
                         "<h3>Backblaze B2</h3>"
                         "<p>Object storage with a 10 GB free tier and static keys: nothing expires, no browser. The best test target of the four.</p>"
                         "<ol>"
                         "<li><b>backblaze.com</b> &rarr; sign up (free tier needs no card).</li>"
                         "<li><b>Buckets &rarr; Create a Bucket</b> &rarr; unique name, keep it <b>Private</b>.</li>"
                         "<li><b>My Settings &rarr; Application Keys &rarr; Add a New Application Key</b> &rarr; name <code>nesva</code>, allow all buckets &rarr; create.</li>"
                         "<li>Copy <b>keyID</b> and <b>applicationKey</b> now: the second value is shown only once.</li>"
                         "</ol>"
                         "<p>Then in this session:</p>"
                         "<ol>"
                         "<li><code>n</code> &nbsp; <b>name&gt;</b> &rarr; <code>b2</code></li>"
                         "<li><b>Storage&gt;</b> &rarr; <code>b2</code></li>"
                         "<li><b>account</b> &rarr; the <b>keyID</b></li>"
                         "<li><b>key</b> &rarr; the <b>applicationKey</b></li>"
                         "<li><b>hard_delete</b> &rarr; Enter (keep <code>false</code>: Nesva never deletes)</li>"
                         "<li><b>Edit advanced config?</b> &rarr; <code>n</code>, <b>Keep this remote?</b> &rarr; <code>y</code>, then <code>q</code></li>"
                         "</ol>"
                         "<p>Address the remote as <code>b2:your-bucket-name</code>. B2 is also a native Restic backend.</p>"
                         )});
    profiles.append({QStringLiteral("onedrive"), QStringLiteral("Microsoft OneDrive"),
                     QStringLiteral("onedrive"), QStringLiteral("onedrive"),
                     QStringLiteral(
                         "<h3>OneDrive</h3>"
                         "<ol>"
                         "<li><code>n</code> &nbsp; <b>name&gt;</b> &rarr; <code>onedrive</code></li>"
                         "<li><b>Storage&gt;</b> &rarr; <code>onedrive</code></li>"
                         "<li>Choose <b>OneDrive Personal or Business</b>; accept the default client ID and secret with Enter</li>"
                         "<li><b>Use web browser to automatically authenticate?</b> &rarr; <code>y</code> and sign in</li>"
                         "<li>Pick the <b>drive</b> to use (usually <i>root</i>)</li>"
                         "<li><b>Edit advanced config?</b> &rarr; <code>n</code>, <b>Keep this remote?</b> &rarr; <code>y</code>, then <code>q</code></li>"
                         "</ol>"
                         "<p>5 GB free on a personal account.</p>"
                         )});
    profiles.append({QStringLiteral("webdav"), QStringLiteral("WebDAV / NAS"),
                     QStringLiteral("webdav"), QStringLiteral("nas"),
                     QStringLiteral(
                         "<h3>WebDAV (NAS, Nextcloud, Storage Box)</h3>"
                         "<ol>"
                         "<li><code>n</code> &nbsp; <b>name&gt;</b> &rarr; <code>nas</code></li>"
                         "<li><b>Storage&gt;</b> &rarr; <code>webdav</code></li>"
                         "<li><b>url</b> &rarr; the WebDAV endpoint</li>"
                         "<li><b>vendor</b> &rarr; <code>nextcloud</code> / <code>owncloud</code> / <code>other</code></li>"
                         "<li><b>user</b> / <b>pass</b> &rarr; usually an app password</li>"
                         "<li><b>Edit advanced config?</b> &rarr; <code>n</code>, <b>Keep this remote?</b> &rarr; <code>y</code>, then <code>q</code></li>"
                         "</ol>"
                         "<p>No account needed for a local test: run <code>rclone serve webdav /tmp/store --addr 127.0.0.1:8088</code> and use <code>http://127.0.0.1:8088</code> with any user/password.</p>"
                         )});

    return profiles;
}

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

    // Provider selector: changing it swaps the walkthrough on the left and the
    // name/backend suggestions offered at rclone's prompts.
    profiles_ = rcloneProviderProfiles();
    auto* providerRow = new QHBoxLayout();
    providerRow->addWidget(new QLabel(QStringLiteral("Provider:"), this));
    providerCombo_ = new QComboBox(this);
    providerCombo_->setObjectName(QStringLiteral("rcloneProvider"));
    for (const RcloneProviderProfile& entry : profiles_) {
        providerCombo_->addItem(entry.label, entry.key);
    }
    providerRow->addWidget(providerCombo_, 1);
    root->addLayout(providerRow);

    auto* splitter = new QSplitter(Qt::Horizontal, this);

    guide_ = new QTextBrowser(splitter);
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

    // Pressing Enter in the answer line must send ONE line. A QPushButton in a
    // dialog is "auto default" by default, so Enter used to trigger the button
    // as well as returnPressed - every answer was sent twice, which is what made
    // rclone re-ask ("Bad password: no characters in password").
    for (QPushButton* button : {sendButton_, restartButton_, closeButton_}) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }

    connect(sendButton_, &QPushButton::clicked, this, [this] { sendLine(input_->text()); });
    connect(input_, &QLineEdit::returnPressed, this, [this] { sendLine(input_->text()); });
    connect(providerCombo_, &QComboBox::currentIndexChanged, this,
            [this](int index) { applyProfile(index); });
    connect(restartButton_, &QPushButton::clicked, this, [this] {
        if (process_ != nullptr) {
            return;
        }
        transcript_->clear();
        startSession();
    });
    // done() stops the rclone session, whether the dialog was closed with this
    // button, the title-bar X, Escape or by the application shutting down.
    connect(closeButton_, &QPushButton::clicked, this, [this] { reject(); });

    // Default to the profile whose remote name matches what the caller asked
    // for ("icloud", "gdrive", "pcloud", "b2", ...).
    for (int index = 0; index < profiles_.size(); ++index) {
        if (profiles_.at(index).remote.compare(remoteName_, Qt::CaseInsensitive) == 0 ||
            profiles_.at(index).backend.compare(remoteName_, Qt::CaseInsensitive) == 0) {
            providerCombo_->setCurrentIndex(index);
            break;
        }
    }
    applyProfile(providerCombo_->currentIndex());

    startSession();
}

RcloneConfigDialog::~RcloneConfigDialog() {
    stopSession();
}

void RcloneConfigDialog::done(int result) {
    stopSession();
    QDialog::done(result);
}

void RcloneConfigDialog::stopSession() {
    closing_ = true;
    if (process_ == nullptr) {
        return;
    }
    QProcess* process = process_;
    process_ = nullptr;
    // Nothing may be delivered to a dialog that is closing: readOutput() and
    // finishSession() write to widgets that are about to be destroyed. Without
    // this, closing the wizard while rclone was at its first prompt crashed in
    // the QProcess destructor (or left the application unresponsive).
    process->disconnect(this);
    process->kill();
    process->waitForFinished(2000);
    delete process;
}

const RcloneProviderProfile* RcloneConfigDialog::profile() const {
    const int index = providerCombo_ != nullptr ? providerCombo_->currentIndex() : -1;
    if (index < 0 || index >= profiles_.size()) {
        return nullptr;
    }
    return &profiles_.at(index);
}

void RcloneConfigDialog::applyProfile(int index) {
    if (index < 0 || index >= profiles_.size()) {
        return;
    }
    const RcloneProviderProfile& chosen = profiles_.at(index);
    remoteName_ = chosen.remote;
    guide_->setHtml(chosen.html);
    setWindowTitle(QStringLiteral("Configure Rclone - %1").arg(chosen.label));
    if (statusLabel_ != nullptr) {
        statusLabel_->setText(
            QStringLiteral("Follow the %1 steps on the left; the live rclone session is on the "
                           "right. Suggested remote name: %2 (use it as %2:).")
                .arg(chosen.label, chosen.remote));
    }
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
    if (process_ == nullptr || closing_) {
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
    // Remember which option rclone is asking about, so the quick buttons can be
    // the right ones ("Option Storage." -> offer the backend names).
    static const QRegularExpression optionLine(QStringLiteral("^Option ([A-Za-z0-9_]+)\\."));
    for (const QString& line : lines) {
        const QRegularExpressionMatch match = optionLine.match(line.trimmed());
        if (match.hasMatch()) {
            lastOption_ = match.captured(1);
        }
    }
    offerChoices();
}

void RcloneConfigDialog::offerChoices() {
    while (QLayoutItem* item = choiceLayout_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
    QStringList choices;
    static const QRegularExpression choicePrompt(
        QStringLiteral("^([A-Za-z0-9]+(?:/[A-Za-z0-9]+)+)>\\s*$"));
    const QStringList tail = pendingOutput_.trimmed().split(QLatin1Char('\n'));
    const QRegularExpressionMatch match =
        choicePrompt.match(tail.isEmpty() ? QString() : tail.last().trimmed());
    if (match.hasMatch()) {
        choices = match.captured(1).split(QLatin1Char('/'));
    } else if (lastOption_ == QStringLiteral("Storage")) {
        // rclone accepts the backend name instead of the list number.
        for (const RcloneProviderProfile& entry : profiles_) {
            choices.append(entry.backend);
        }
        choices.append(QStringLiteral("local"));
        choices.append(QStringLiteral("webdav"));
    } else if (lastOption_ == QStringLiteral("password")) {
        choices = {QStringLiteral("y"), QStringLiteral("g")};
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
    if (process_ == nullptr || closing_) {
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
    if (closing_) {
        // The dialog is going away; the transcript must not be touched.
        return;
    }
    if (process_ != nullptr) {
        process_->deleteLater();
        process_ = nullptr;
    }
    input_->setEnabled(false);
    sendButton_->setEnabled(false);
    restartButton_->setEnabled(true);
    choiceBar_->setVisible(false);
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