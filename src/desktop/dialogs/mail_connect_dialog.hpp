// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

namespace nesva::ui {

class AppContext;

/// Signs a Microsoft 365 account in from the desktop: the Entra application
/// details, the browser sign-in with PKCE over a loopback redirect, and the
/// refresh token into the OS keyring.
///
/// The flow itself lives in mail::connectAccount, so the CLI and this dialog
/// behave identically; nothing here knows about tokens.
class MailConnectDialog : public QDialog {
    Q_OBJECT

public:
    explicit MailConnectDialog(AppContext& context, QWidget* parent = nullptr);

    /// Account row id of the account that was connected, 0 when none was.
    qint64 connectedAccountId() const { return connectedAccountId_; }
    QString connectedAccount() const { return connectedAccount_; }

private:
    void startConnect();
    void appendStatus(const QString& line);
    void setBusy(bool busy);
    void showSignInUrl(const QString& url);

    AppContext& context_;
    QLineEdit* accountEdit_ = nullptr;
    QLineEdit* tenantEdit_ = nullptr;
    QLineEdit* clientEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* urlLabel_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    qint64 connectedAccountId_ = 0;
    QString connectedAccount_;
    bool busy_ = false;
};

} // namespace nesva::ui
