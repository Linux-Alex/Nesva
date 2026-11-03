// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QVector>

#include "page.hpp"
#include "nesva/mail/mail_store.hpp"

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QStandardItemModel;
class QTableView;

namespace nesva::ui {

class AppContext;
class ElidedLabel;

/// Microsoft 365 archival: which account is connected, how much mail it holds,
/// and the one-click path from "account" to a scheduled job whose output can be
/// covered by a Restic backup.
///
/// Connecting an account is an interactive browser sign-in and lives in the CLI
/// (\c nesva-cli mail connect); this page says so instead of pretending a button
/// can do it without credentials.
class MailPage : public Page {
    Q_OBJECT

public:
    explicit MailPage(AppContext& context, QWidget* parent = nullptr);

    void refresh() override;

private:
    void loadAccounts();
    void loadFolders();
    void estimate();
    void syncNow();
    void editJob();
    void backUpWithRestic();
    void updateActionState();
    qint64 currentAccountId() const;
    qint64 findMailJob(qint64 accountId) const;

    AppContext& context_;
    QComboBox* accountCombo_ = nullptr;
    QTableView* view_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    ElidedLabel* statusLabel_ = nullptr;
    ElidedLabel* hintLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* estimateButton_ = nullptr;
    QPushButton* syncButton_ = nullptr;
    QPushButton* jobButton_ = nullptr;
    QPushButton* resticButton_ = nullptr;
    QVector<mail::MailAccount> accounts_;
    bool busy_ = false;
};

} // namespace nesva::ui
