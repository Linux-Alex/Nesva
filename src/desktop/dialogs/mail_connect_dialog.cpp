// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "mail_connect_dialog.hpp"

#include <QtCore/QFutureWatcher>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtConcurrent/QtConcurrentRun>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include "app_context.hpp"
#include "nesva/config/settings_store.hpp"
#include "nesva/mail/session.hpp"

namespace nesva::ui {

MailConnectDialog::MailConnectDialog(AppContext& context, QWidget* parent)
    : QDialog(parent), context_(context) {
    setWindowTitle(QStringLiteral("Connect Microsoft 365"));
    setModal(true);
    resize(620, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);

    auto* intro = new QLabel(
        QStringLiteral("Nesva signs in as a public client: your browser opens, you approve, and "
                       "the refresh token goes into the Windows Credential Manager (or the Linux "
                       "keyring). No password and no client secret are stored anywhere."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    accountEdit_ = new QLineEdit(this);
    accountEdit_->setObjectName(QStringLiteral("mailConnectAccount"));
    accountEdit_->setPlaceholderText(QStringLiteral("you@example.com"));
    form->addRow(QStringLiteral("Account"), accountEdit_);

    tenantEdit_ = new QLineEdit(this);
    tenantEdit_->setObjectName(QStringLiteral("mailConnectTenant"));
    tenantEdit_->setPlaceholderText(QStringLiteral("Directory (tenant) ID"));
    form->addRow(QStringLiteral("Tenant ID"), tenantEdit_);

    clientEdit_ = new QLineEdit(this);
    clientEdit_->setObjectName(QStringLiteral("mailConnectClient"));
    clientEdit_->setPlaceholderText(QStringLiteral("Application (client) ID"));
    form->addRow(QStringLiteral("Client ID"), clientEdit_);
    layout->addLayout(form);

    auto* redirect = new QLabel(
        QStringLiteral("Redirect URI to register in Entra under <i>Mobile and desktop "
                       "applications</i>: <b>http://localhost</b>. Nesva uses a random loopback "
                       "port, which Microsoft ignores for that platform. The registration must "
                       "also have <i>Allow public client flows</i> set to Yes."),
        this);
    redirect->setObjectName(QStringLiteral("pageSubtitle"));
    redirect->setWordWrap(true);
    redirect->setTextFormat(Qt::RichText);
    layout->addWidget(redirect);

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    urlLabel_ = new QLabel(this);
    urlLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    urlLabel_->setWordWrap(true);
    urlLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    urlLabel_->setOpenExternalLinks(true);
    urlLabel_->setVisible(false);
    layout->addWidget(urlLabel_);

    layout->addStretch(1);

    auto* buttons = new QHBoxLayout();
    closeButton_ = new QPushButton(QStringLiteral("Close"), this);
    connectButton_ = new QPushButton(QStringLiteral("Connect..."), this);
    connectButton_->setDefault(true);
    buttons->addStretch(1);
    buttons->addWidget(closeButton_);
    buttons->addWidget(connectButton_);
    layout->addLayout(buttons);

    // Prefill from the settings the Settings page writes.
    const auto setting = [this](const char* key) {
        return context_.settings()
            .getString(QString::fromUtf8(key), QString())
            .valueOr(QString());
    };
    tenantEdit_->setText(setting(config::keys::kMailTenantId));
    clientEdit_->setText(setting(config::keys::kMailClientId));

    connect(connectButton_, &QPushButton::clicked, this, [this] { startConnect(); });
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::reject);
}

void MailConnectDialog::setBusy(bool busy) {
    busy_ = busy;
    connectButton_->setEnabled(!busy);
    accountEdit_->setEnabled(!busy);
    tenantEdit_->setEnabled(!busy);
    clientEdit_->setEnabled(!busy);
    closeButton_->setText(busy ? QStringLiteral("Close") : QStringLiteral("Close"));
}

void MailConnectDialog::appendStatus(const QString& line) {
    statusLabel_->setText(line);
}

void MailConnectDialog::showSignInUrl(const QString& url) {
    urlLabel_->setText(QStringLiteral("Sign-in page: <a href=\"%1\">%1</a>").arg(url.toHtmlEscaped()));
    urlLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    urlLabel_->setOpenExternalLinks(true);
    urlLabel_->setVisible(true);
}

void MailConnectDialog::startConnect() {
    if (busy_) {
        return;
    }
    const QString upn = accountEdit_->text().trimmed();
    const QString tenant = tenantEdit_->text().trimmed();
    const QString client = clientEdit_->text().trimmed();
    if (upn.isEmpty() || tenant.isEmpty() || client.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Connect Microsoft 365"),
                             QStringLiteral("Fill in the account, the tenant id and the client id "
                                            "of the app registration."));
        return;
    }

    // Persist the app registration details, so the CLI and a later reconnect
    // use the same values.
    context_.settings().setString(QString::fromUtf8(config::keys::kMailTenantId), tenant);
    context_.settings().setString(QString::fromUtf8(config::keys::kMailClientId), client);

    setBusy(true);
    urlLabel_->setVisible(false);
    appendStatus(QStringLiteral("Opening your browser to sign in..."));

    auto* watcher = new QFutureWatcher<Result<mail::MailConnection>>(this);
    connect(watcher, &QFutureWatcher<Result<mail::MailConnection>>::finished, this,
            [this, watcher] {
                const Result<mail::MailConnection> result = watcher->result();
                watcher->deleteLater();
                setBusy(false);
                if (!result) {
                    appendStatus(QStringLiteral("Sign-in failed: %1")
                                     .arg(QString::fromStdString(
                                         result.error().toDisplayString())));
                    return;
                }
                connectedAccountId_ = result.value().accountId;
                connectedAccount_ = result.value().accountIdentifier;
                appendStatus(QStringLiteral("Connected %1 (%2 folders discovered). The refresh "
                                            "token is stored in the system keyring.")
                                 .arg(result.value().accountIdentifier)
                                 .arg(result.value().folders));
                context_.recordEvent(LogLevel::Info, QStringLiteral("mail"),
                                     QStringLiteral("Connected Microsoft 365 account: %1")
                                         .arg(result.value().accountIdentifier));
                connectButton_->setText(QStringLiteral("Connected"));
                connectButton_->setEnabled(false);
            });

    const QPointer<MailConnectDialog> guard(this);
    watcher->setFuture(QtConcurrent::run(
        [this, guard, upn, tenant, client]() -> Result<mail::MailConnection> {
            return mail::connectAccount(
                context_.database(), upn, tenant, client,
                [guard](const QString& url) {
                    if (guard) {
                        QMetaObject::invokeMethod(
                            guard.data(), [guard, url] {
                                if (guard) {
                                    guard->showSignInUrl(url);
                                    guard->appendStatus(
                                        QStringLiteral("Waiting for the browser sign-in to "
                                                       "complete..."));
                                }
                            },
                            Qt::QueuedConnection);
                    }
                });
        }));
}

} // namespace nesva::ui
