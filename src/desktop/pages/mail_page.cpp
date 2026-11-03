// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "mail_page.hpp"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDir>
#include <QtCore/QFutureWatcher>
#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableView>

#include <utility>

#include "app_context.hpp"
#include "dialogs/job_editor_dialog.hpp"
#include "dialogs/mail_connect_dialog.hpp"
#include "job_executor.hpp"
#include "nesva/jobs/job_store.hpp"
#include "nesva/mail/mail_store.hpp"
#include "widgets/elided_label.hpp"

namespace nesva::ui {
namespace {

QString humanCount(qint64 value) {
    return QString::number(value);
}

} // namespace

MailPage::MailPage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Mail"),
           QStringLiteral("Microsoft 365 mail is archived as one .eml file per message. The "
                          "mailbox is only read: nothing is moved, deleted or marked as read."),
           parent),
      context_(context) {
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(6);

    accountCombo_ = new QComboBox(this);
    accountCombo_->setObjectName(QStringLiteral("mailAccount"));
    accountCombo_->setMinimumWidth(280);
    toolbar->addWidget(accountCombo_);

    connectButton_ = new QPushButton(QStringLiteral("Connect account..."), this);
    refreshButton_ = new QPushButton(QStringLiteral("Refresh"), this);
    estimateButton_ = new QPushButton(QStringLiteral("Estimate"), this);
    syncButton_ = new QPushButton(QStringLiteral("Sync now"), this);
    jobButton_ = new QPushButton(QStringLiteral("Schedule job..."), this);
    resticButton_ = new QPushButton(QStringLiteral("Back up archive with Restic..."), this);
    toolbar->addWidget(connectButton_);
    toolbar->addWidget(refreshButton_);
    toolbar->addWidget(estimateButton_);
    toolbar->addWidget(syncButton_);
    toolbar->addWidget(jobButton_);
    toolbar->addStretch(1);
    toolbar->addWidget(resticButton_);
    body()->addLayout(toolbar);

    progress_ = new QProgressBar(this);
    progress_->setVisible(false);
    progress_->setRange(0, 0);
    body()->addWidget(progress_);

    statusLabel_ = new ElidedLabel(this);
    statusLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    body()->addWidget(statusLabel_);

    hintLabel_ = new ElidedLabel(this);
    hintLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    body()->addWidget(hintLabel_);

    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({QStringLiteral("Folder"), QStringLiteral("In Microsoft 365"),
                                       QStringLiteral("Archived"), QStringLiteral("Remaining"),
                                       QStringLiteral("Include")});
    view_ = new QTableView(this);
    view_->setObjectName(QStringLiteral("mailFolders"));
    view_->setModel(model_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(false);
    view_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; ++column) {
        view_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    body()->addWidget(view_, 1);

    connect(connectButton_, &QPushButton::clicked, this, &MailPage::connectAccount);
    connect(refreshButton_, &QPushButton::clicked, this, [this] { refresh(); });
    connect(model_, &QStandardItemModel::itemChanged, this, [this](QStandardItem* item) {
        if (loadingFolders_ || item == nullptr || item->column() != 4) {
            return;
        }
        const qint64 folderRowId = item->data(Qt::UserRole).toLongLong();
        if (folderRowId <= 0) {
            return;
        }
        mail::MailStore store(context_.database());
        const bool enabled = item->checkState() == Qt::Checked;
        const Status stored = store.setFolderSyncEnabled(folderRowId, enabled);
        if (!stored.ok()) {
            statusLabel_->setText(QString::fromStdString(stored.error().toDisplayString()));
            return;
        }
        context_.recordEvent(LogLevel::Info, QStringLiteral("mail"),
                             QStringLiteral("%1 folder %2")
                                 .arg(enabled ? QStringLiteral("Included")
                                              : QStringLiteral("Excluded"),
                                      item->text()));
    });
    connect(accountCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        loadFolders();
        updateActionState();
    });
    connect(estimateButton_, &QPushButton::clicked, this, &MailPage::estimate);
    connect(syncButton_, &QPushButton::clicked, this, &MailPage::syncNow);
    connect(jobButton_, &QPushButton::clicked, this, &MailPage::editJob);
    connect(resticButton_, &QPushButton::clicked, this, &MailPage::backUpWithRestic);

    JobExecutor* executor = context_.executor();
    if (executor != nullptr) {
        connect(executor, &JobExecutor::started, this, [this](qint64, const QString& name) {
            busy_ = true;
            progress_->setVisible(true);
            progress_->setRange(0, 0);
            statusLabel_->setText(QStringLiteral("Running %1...").arg(name));
            updateActionState();
        });
        connect(executor, &JobExecutor::progressChanged, this,
                [this](qint64, const jobs::RunProgress& update) {
                    if (update.filesTotal > 0) {
                        progress_->setRange(0, static_cast<int>(update.filesTotal));
                        progress_->setValue(static_cast<int>(update.filesProcessed));
                    }
                    if (!update.message.isEmpty()) {
                        statusLabel_->setText(
                            QStringLiteral("%1 - %2").arg(update.currentFile, update.message));
                    }
                });
        connect(executor, &JobExecutor::finished, this,
                [this](qint64, const jobs::RunReport& report) {
                    busy_ = false;
                    progress_->setVisible(false);
                    statusLabel_->setText(
                        QStringLiteral("%1: %2 archived, %3 unchanged, %4 failed")
                            .arg(report.status)
                            .arg(report.counters.filesTransferred)
                            .arg(report.counters.filesSkipped)
                            .arg(report.counters.filesFailed));
                    loadFolders();
                    updateActionState();
                    // An expired or revoked sign-in is the one failure the user
                    // can fix from here, so offer exactly that.
                    const bool needsSignIn =
                        report.status == QStringLiteral("failed") &&
                        std::any_of(report.errors.cbegin(), report.errors.cend(),
                                    [](const QString& error) {
                                        return error.contains(QStringLiteral("refresh token"),
                                                              Qt::CaseInsensitive) ||
                                               error.contains(QStringLiteral("sign-in"),
                                                              Qt::CaseInsensitive) ||
                                               error.contains(QStringLiteral("credential"),
                                                              Qt::CaseInsensitive);
                                    });
                    if (needsSignIn) {
                        const auto answer = QMessageBox::question(
                            this, QStringLiteral("Microsoft 365"),
                            QStringLiteral("The sign-in for this account is no longer valid. "
                                           "Connect it again now?"),
                            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                        if (answer == QMessageBox::Yes) {
                            connectAccount();
                        }
                    }
                });
    }

    loadAccounts();
}

void MailPage::refresh() {
    loadAccounts();
}

void MailPage::loadAccounts() {
    const qint64 previous = currentAccountId();
    accounts_.clear();
    accountCombo_->blockSignals(true);
    accountCombo_->clear();

    mail::MailStore store(context_.database());
    const Result<QVector<mail::MailAccount>> loaded = store.accounts();
    if (loaded) {
        accounts_ = loaded.value();
    }
    for (const mail::MailAccount& account : accounts_) {
        accountCombo_->addItem(QStringLiteral("%1 (%2)").arg(account.accountIdentifier,
                                                              account.status),
                               static_cast<qulonglong>(account.id));
    }
    accountCombo_->blockSignals(false);

    if (accounts_.isEmpty()) {
        accountCombo_->addItem(QStringLiteral("No account connected"), QVariant());
        hintLabel_->setText(
            QStringLiteral("Connect a mailbox once from a terminal: nesva-cli mail connect "
                           "--account you@example.com --tenant <id> --client <id>. The sign-in "
                           "happens in your browser and the refresh token goes to the system "
                           "keyring, never into the database."));
        model_->removeRows(0, model_->rowCount());
        statusLabel_->clearText();
        updateActionState();
        return;
    }

    hintLabel_->setText(
        QStringLiteral("Sync now runs a real job, so it appears in the history and can be "
                       "scheduled. Back up archive with Restic creates a backup job whose source "
                       "is the mail archive directory, which gives versioned, encrypted copies."));
    if (previous > 0) {
        const int index = accountCombo_->findData(static_cast<qulonglong>(previous));
        if (index >= 0) {
            accountCombo_->setCurrentIndex(index);
        }
    }
    loadFolders();
    updateActionState();
}

void MailPage::connectAccount() {
    MailConnectDialog dialog(context_, this);
    if (dialog.exec() == QDialog::Accepted || dialog.connectedAccountId() > 0) {
        loadAccounts();
        statusLabel_->setText(QStringLiteral("Connected %1.").arg(dialog.connectedAccount()));
    }
}

void MailPage::loadFolders() {
    model_->removeRows(0, model_->rowCount());
    const qint64 accountId = currentAccountId();
    if (accountId <= 0) {
        return;
    }
    mail::MailStore store(context_.database());
    const Result<QVector<mail::MailFolderStats>> stats = store.folderStats(accountId);
    if (!stats) {
        statusLabel_->setText(QString::fromStdString(stats.error().toDisplayString()));
        return;
    }

    qint64 total = 0;
    qint64 archived = 0;
    loadingFolders_ = true;
    for (const mail::MailFolderStats& folder : stats.value()) {
        total += folder.totalItems;
        archived += folder.exportedItems;
        QList<QStandardItem*> row;
        row.append(new QStandardItem(folder.displayName));
        row.append(new QStandardItem(humanCount(folder.totalItems)));
        row.append(new QStandardItem(humanCount(folder.exportedItems)));
        row.append(new QStandardItem(humanCount(folder.remainingItems())));
        auto* include = new QStandardItem();
        include->setCheckable(true);
        include->setCheckState(folder.syncEnabled ? Qt::Checked : Qt::Unchecked);
        include->setData(static_cast<qulonglong>(folder.folderRowId), Qt::UserRole);
        include->setToolTip(QStringLiteral("Include this folder in the archive"));
        row.append(include);
        model_->appendRow(row);
    }
    loadingFolders_ = false;

    if (statusLabel_->text().isEmpty()) {
        statusLabel_->setText(QStringLiteral("%1 messages in %2 folders; %3 archived, %4 to go.")
                                  .arg(total)
                                  .arg(model_->rowCount())
                                  .arg(archived)
                                  .arg(qMax<qint64>(0, total - archived)));
    }
}

qint64 MailPage::currentAccountId() const {
    const QVariant selection = accountCombo_->currentData();
    if (!selection.isValid()) {
        return 0;
    }
    return static_cast<qint64>(selection.toULongLong());
}

qint64 MailPage::findMailJob(qint64 accountId) const {
    jobs::JobStore store(context_.database());
    const Result<QVector<jobs::ArchiveJob>> jobs = store.listJobs();
    if (!jobs) {
        return 0;
    }
    for (const jobs::ArchiveJob& job : jobs.value()) {
        if (job.isMail() && job.mailAccountId == accountId) {
            return job.id;
        }
    }
    return 0;
}

void MailPage::updateActionState() {
    const bool hasAccount = currentAccountId() > 0;
    estimateButton_->setEnabled(hasAccount && !busy_);
    syncButton_->setEnabled(hasAccount && !busy_);
    jobButton_->setEnabled(!busy_);
    resticButton_->setEnabled(!busy_);
}

void MailPage::estimate() {
    const qint64 accountId = currentAccountId();
    if (accountId <= 0) {
        return;
    }
    mail::MailStore store(context_.database());
    const Result<mail::MailAccount> account = store.account(accountId);
    if (!account) {
        statusLabel_->setText(QString::fromStdString(account.error().toDisplayString()));
        return;
    }

    qint64 total = 0;
    qint64 archived = 0;
    const Result<QVector<mail::MailFolderStats>> stats = store.folderStats(accountId);
    if (stats) {
        for (const mail::MailFolderStats& folder : stats.value()) {
            if (!folder.syncEnabled) {
                continue;
            }
            total += folder.totalItems;
            archived += folder.exportedItems;
        }
    }
    const qint64 remaining = qMax<qint64>(0, total - archived);

    QString message = QStringLiteral("%1 messages, %2 archived, %3 remaining.").arg(total).arg(archived).arg(remaining);
    if (remaining > 0 && account.value().hasSpeed()) {
        const double rate = account.value().messagesPerSecond();
        const qint64 seconds = rate > 0 ? static_cast<qint64>(remaining / rate) : -1;
        if (seconds > 0) {
            message += QStringLiteral(" At %1 msg/s that is about %2 minute(s).")
                           .arg(QString::number(rate, 'f', 1))
                           .arg((seconds + 59) / 60);
        }
    } else if (remaining > 0) {
        message += QStringLiteral(" No measured speed yet: the first sync will establish it.");
    }
    statusLabel_->setText(message);
}

void MailPage::syncNow() {
    const qint64 accountId = currentAccountId();
    if (accountId <= 0) {
        return;
    }
    JobExecutor* executor = context_.executor();
    if (executor == nullptr || executor->isRunning()) {
        statusLabel_->setText(QStringLiteral("Another run is in progress."));
        return;
    }

    qint64 jobId = findMailJob(accountId);
    if (jobId == 0) {
        const QString directory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose where the .eml files are archived"));
        if (directory.isEmpty()) {
            return;
        }
        jobs::JobStore store(context_.database());
        jobs::ArchiveJob job;
        for (const mail::MailAccount& account : accounts_) {
            if (account.id == accountId) {
                job.name = QStringLiteral("Mail archive: %1").arg(account.accountIdentifier);
            }
        }
        job.mode = QStringLiteral("archive");
        job.sourceKind = QStringLiteral("mail");
        job.mailAccountId = accountId;
        job.destinationPath = QDir::cleanPath(directory);
        const Result<qint64> created = store.createJob(job);
        if (!created) {
            QMessageBox::critical(this, QStringLiteral("Could not create the mail job"),
                                  QString::fromStdString(created.error().toDisplayString()));
            return;
        }
        jobId = created.value();
        context_.recordEvent(LogLevel::Info, QStringLiteral("jobs"),
                             QStringLiteral("Mail job created: %1").arg(job.name));
    }

    const Status started = executor->startRun(jobId, false);
    if (!started.ok()) {
        statusLabel_->setText(QString::fromStdString(started.error().toDisplayString()));
    }
}

void MailPage::editJob() {
    const qint64 accountId = currentAccountId();
    const qint64 jobId = accountId > 0 ? findMailJob(accountId) : 0;
    jobs::JobStore store(context_.database());
    std::optional<jobs::ArchiveJob> existing;
    if (jobId > 0) {
        const Result<jobs::ArchiveJob> job = store.getJob(jobId);
        if (job) {
            existing = job.value();
        }
    } else {
        jobs::ArchiveJob templateJob;
        templateJob.mode = QStringLiteral("archive");
        templateJob.sourceKind = QStringLiteral("mail");
        templateJob.mailAccountId = accountId;
        for (const mail::MailAccount& account : accounts_) {
            if (account.id == accountId) {
                templateJob.name = QStringLiteral("Mail archive: %1").arg(account.accountIdentifier);
            }
        }
        existing = templateJob;
    }

    JobEditorDialog dialog(context_, existing, this);
    if (dialog.exec() == QDialog::Accepted) {
        loadFolders();
        updateActionState();
    }
}

void MailPage::backUpWithRestic() {
    const qint64 accountId = currentAccountId();
    if (accountId <= 0) {
        return;
    }
    const qint64 jobId = findMailJob(accountId);
    QString archiveDirectory;
    if (jobId > 0) {
        jobs::JobStore store(context_.database());
        const Result<jobs::ArchiveJob> job = store.getJob(jobId);
        if (job) {
            archiveDirectory = job.value().destinationPath;
        }
    }
    if (archiveDirectory.isEmpty()) {
        archiveDirectory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose the mail archive directory to back up"));
        if (archiveDirectory.isEmpty()) {
            return;
        }
    }

    // A Restic backup of the .eml tree: versioned and encrypted, so a deletion
    // or a ransomware encryption in the archive can be undone.
    jobs::ArchiveJob templateJob;
    templateJob.mode = QStringLiteral("backup");
    templateJob.name = QStringLiteral("Mail backup (Restic)");
    templateJob.sources = {{0, QDir::cleanPath(archiveDirectory), QStringLiteral("mail")}};

    JobEditorDialog dialog(context_, templateJob, this);
    if (dialog.exec() == QDialog::Accepted) {
        statusLabel_->setText(QStringLiteral("Backup job saved. Run it from Backup Jobs to take "
                                             "the first snapshot."));
    }
}

} // namespace nesva::ui
