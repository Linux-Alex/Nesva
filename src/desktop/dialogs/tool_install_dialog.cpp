// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "tool_install_dialog.hpp"

#include <QtCore/QFileInfo>
#include <QtCore/QFutureWatcher>
#include <QtCore/QPointer>
#include <QtConcurrent/QtConcurrentRun>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <memory>

#include "app_context.hpp"
#include "nesva/core/external_tool.hpp"
#include "nesva/core/http.hpp"

namespace nesva::ui {

ToolInstallDialog::ToolInstallDialog(AppContext& context, const tools::ToolId tool,
                                     QWidget* parent)
    : QDialog(parent), context_(context), tool_(tool) {
    const QString display = tools::displayName(tool_);
    setWindowTitle(QStringLiteral("Install %1").arg(display));
    setModal(true);
    resize(720, 620);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto* intro = new QLabel(
        tool_ == tools::ToolId::Restic
            ? QStringLiteral("Restic is a separate program that Nesva calls to create and restore "
                             "versioned, encrypted backups. Nesva never bundles it and never pipes "
                             "a download into a shell: the steps below are exactly what will run, "
                             "and nothing is downloaded or executed until you press Install.")
            : QStringLiteral("Rclone is a separate program that Nesva calls for cloud "
                             "destinations. Nesva never bundles it and never pipes a download "
                             "into a shell: the steps below are exactly what will run, and "
                             "nothing is downloaded or executed until you press Install."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    scopeCombo_ = new QComboBox(this);
    scopeCombo_->addItem(QStringLiteral("Only for me (no password, recommended)"),
                         QString::fromLatin1("user"));
    scopeCombo_->addItem(QStringLiteral("All users (%1, asks for your password)")
                             .arg(tools::toolTargetPath(tool_, tools::InstallScope::System)),
                         QString::fromLatin1("system"));
    form->addRow(QStringLiteral("Install for:"), scopeCombo_);
    layout->addLayout(form);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setWordWrap(true);
    summaryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(summaryLabel_);

    planView_ = new QPlainTextEdit(this);
    planView_->setReadOnly(true);
    planView_->setLineWrapMode(QPlainTextEdit::NoWrap);
    planView_->setStyleSheet(
        QStringLiteral("font-family: monospace; font-size: 12px;"));
    planView_->setMinimumHeight(260);
    layout->addWidget(planView_, 1);

    logView_ = new QPlainTextEdit(this);
    logView_->setReadOnly(true);
    logView_->setMaximumHeight(110);
    logView_->setVisible(false);
    layout->addWidget(logView_);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 0);
    progressBar_->setVisible(false);
    layout->addWidget(progressBar_);

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    statusLabel_->setVisible(false);
    layout->addWidget(statusLabel_);

    auto* buttons = new QHBoxLayout();
    installButton_ = new QPushButton(QStringLiteral("Install %1").arg(display), this);
    installButton_->setDefault(true);
    installButton_->setEnabled(false);
    closeButton_ = new QPushButton(QStringLiteral("Close"), this);
    buttons->addStretch(1);
    buttons->addWidget(closeButton_);
    buttons->addWidget(installButton_);
    layout->addLayout(buttons);

    connect(closeButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(installButton_, &QPushButton::clicked, this, [this] { performInstall(); });
    connect(scopeCombo_, &QComboBox::currentIndexChanged, this, [this] {
        if (!busy_) {
            requestPlan();
        }
    });

    requestPlan();
}

void ToolInstallDialog::showPlaceholder(const QString& message) {
    summaryLabel_->setText(message);
    planView_->setPlainText(QString());
    installButton_->setEnabled(false);
    planReady_ = false;
}

void ToolInstallDialog::requestPlan() {
    const bool systemScope = scopeCombo_->currentData().toString() == QStringLiteral("system");
    const tools::InstallScope scope =
        systemScope ? tools::InstallScope::System : tools::InstallScope::User;
    showPlaceholder(QStringLiteral("Reading the current %1 release to work out the exact "
                                   "steps...")
                        .arg(tools::displayName(tool_)));

    auto* watcher = new QFutureWatcher<Result<tools::ToolInstallPlan>>(this);
    connect(watcher, &QFutureWatcher<Result<tools::ToolInstallPlan>>::finished, this,
            [this, watcher] {
                const Result<tools::ToolInstallPlan> plan = watcher->result();
                watcher->deleteLater();
                if (!plan) {
                    summaryLabel_->setText(
                        QStringLiteral("Cannot prepare the installation: %1")
                            .arg(QString::fromStdString(plan.error().toDisplayString())));
                    return;
                }
                plan_ = plan.value();
                planReady_ = true;
                renderPlan();
            });
    const tools::ToolId tool = tool_;
    watcher->setFuture(QtConcurrent::run([tool, scope]() -> Result<tools::ToolInstallPlan> {
        net::QNetworkHttpTransport transport;
        return tools::planToolInstall(tool, scope, transport);
    }));
}

void ToolInstallDialog::renderPlan() {
    summaryLabel_->setText(
        QStringLiteral("%1 %2 will be installed to %3. %4")
            .arg(plan_.toolDisplayName, plan_.version, plan_.targetPath,
                 plan_.requiresElevation
                     ? QStringLiteral("This needs elevated privileges: the password prompt below "
                                      "is the only elevated action.")
                     : QStringLiteral("No elevated privileges are needed.")));
    planView_->setPlainText(tools::describeInstallPlan(plan_));
    installButton_->setEnabled(plan_.automatic);
    if (!plan_.automatic) {
        installButton_->setText(QStringLiteral("Manual installation required"));
    } else {
        installButton_->setText(
            QStringLiteral("Install %1").arg(tools::displayName(tool_)));
    }
}

void ToolInstallDialog::appendLog(const QString& line) {
    // The installer reports from a worker thread through a queued call, so by the
    // time it arrives the dialog may already be gone. The explicit check also
    // stops GCC 13 from warning about a possible null dereference: the inlined
    // call chain loses the constructor initialisation.
    if (logView_ == nullptr) {
        return;
    }
    logView_->setVisible(true);
    logView_->appendPlainText(line);
}

void ToolInstallDialog::performInstall() {
    if (!planReady_ || busy_) {
        return;
    }
    busy_ = true;
    installButton_->setEnabled(false);
    scopeCombo_->setEnabled(false);
    logView_->clear();
    logView_->setVisible(true);
    progressBar_->setVisible(true);
    statusLabel_->setVisible(false);

    const tools::ToolInstallPlan plan = plan_;
    auto* watcher = new QFutureWatcher<Result<tools::InstallOutcome>>(this);
    connect(watcher, &QFutureWatcher<Result<tools::InstallOutcome>>::finished, this,
            [this, watcher] {
                const Result<tools::InstallOutcome> outcome = watcher->result();
                watcher->deleteLater();
                busy_ = false;
                progressBar_->setVisible(false);
                scopeCombo_->setEnabled(true);
                if (!outcome) {
                    finishWithError(QString::fromStdString(outcome.error().toDisplayString()));
                    return;
                }
                installedPath_ = outcome.value().executablePath;
                const char* settingKey = tool_ == tools::ToolId::Restic
                                             ? config::keys::kResticBinaryPath
                                             : config::keys::kRcloneBinaryPath;
                const Status stored = context_.settings().setString(
                    QString::fromUtf8(settingKey), installedPath_);
                if (!stored.ok()) {
                    finishWithError(QStringLiteral("%1 was installed to %2, but Nesva could "
                                                   "not save that path: %3")
                                        .arg(tools::displayName(tool_), installedPath_,
                                             QString::fromStdString(
                                                 stored.error().toDisplayString())));
                    return;
                }
                appendLog(QStringLiteral("SHA-256 %1").arg(outcome.value().sha256));
                const QString token = tools::toToken(tool_);
                statusLabel_->setText(
                    QStringLiteral("Installed %1 %2 at %3 (checksum verified). Nesva now uses "
                                   "that path. To use %1 in a terminal too, add %4 to PATH.")
                        .arg(token, outcome.value().version, installedPath_,
                             QFileInfo(installedPath_).absolutePath()));
                statusLabel_->setVisible(true);
                installButton_->setText(QStringLiteral("Installed"));
                installButton_->setEnabled(false);
                closeButton_->setText(QStringLiteral("Close"));
            });
    const QPointer<ToolInstallDialog> guard(this);
    watcher->setFuture(QtConcurrent::run([plan, guard]() -> Result<tools::InstallOutcome> {
        return tools::executeToolInstall(plan, [guard](const QString& line) {
            // The installer runs in a worker thread; hop back to the GUI thread
            // to touch widgets.
            if (guard) {
                QMetaObject::invokeMethod(
                    guard.data(), [guard, line] {
                        if (guard) {
                            guard->appendLog(line);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    }));
}

void ToolInstallDialog::finishWithError(const QString& message) {
    statusLabel_->setText(message);
    statusLabel_->setVisible(true);
    installButton_->setEnabled(planReady_ && plan_.automatic);
    appendLog(QStringLiteral("Failed: %1").arg(message));
}

} // namespace nesva::ui
