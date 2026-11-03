// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "snapshots_page.hpp"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDir>
#include <QtCore/QFuture>
#include <QtCore/QMetaObject>
#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableView>

#include <utility>

#include "app_context.hpp"
#include "widgets/elided_label.hpp"
#include "dialogs/snapshot_browser_dialog.hpp"
#include "dialogs/snapshot_restore_dialog.hpp"
#include "nesva/backup/restic.hpp"
#include "nesva/config/settings_store.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/backup_runner.hpp"
#include "nesva/jobs/job_store.hpp"

namespace nesva::ui {
namespace {

QString resticPath(AppContext& context) {
    return context.settings()
        .getString(QString::fromUtf8(config::keys::kResticBinaryPath), QStringLiteral("restic"))
        .valueOr(QStringLiteral("restic"));
}

} // namespace

SnapshotsPage::SnapshotsPage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Snapshots"),
           QStringLiteral("Versioned, encrypted Restic snapshots. Restore, prune and verify them "
                          "here; the password stays in your system keyring."),
           parent),
      context_(context) {
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(6);

    jobCombo_ = new QComboBox(this);
    jobCombo_->setObjectName(QStringLiteral("snapshotJob"));
    jobCombo_->setMinimumWidth(220);
    toolbar->addWidget(jobCombo_);

    refreshButton_ = new QPushButton(QStringLiteral("Refresh from repository"), this);
    browseButton_ = new QPushButton(QStringLiteral("Browse files..."), this);
    restoreButton_ = new QPushButton(QStringLiteral("Restore snapshot..."), this);
    toolbar->addWidget(refreshButton_);
    toolbar->addWidget(browseButton_);
    toolbar->addWidget(restoreButton_);
    toolbar->addStretch(1);

    retentionPreviewButton_ = new QPushButton(QStringLiteral("Retention preview"), this);
    retentionApplyButton_ = new QPushButton(QStringLiteral("Apply retention..."), this);
    checkButton_ = new QPushButton(QStringLiteral("Check repository"), this);
    unlockButton_ = new QPushButton(QStringLiteral("Unlock"), this);
    toolbar->addWidget(retentionPreviewButton_);
    toolbar->addWidget(retentionApplyButton_);
    toolbar->addWidget(checkButton_);
    toolbar->addWidget(unlockButton_);
    body()->addLayout(toolbar);

    // Fixed single line: repository messages are long and must not reflow the
    // snapshot table underneath them.
    statusLabel_ = new ElidedLabel(this);
    statusLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    body()->addWidget(statusLabel_);

    busyBar_ = new QProgressBar(this);
    busyBar_->setVisible(false);
    busyBar_->setRange(0, 0);
    body()->addWidget(busyBar_);

    emptyState_ = new QLabel(this);
    emptyState_->setObjectName(QStringLiteral("emptyState"));
    emptyState_->setWordWrap(true);
    body()->addWidget(emptyState_, 1);

    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({QStringLiteral("Snapshot"), QStringLiteral("Created"),
                                       QStringLiteral("Host"), QStringLiteral("Files"),
                                       QStringLiteral("Bytes")});
    view_ = new QTableView(this);
    view_->setModel(model_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    view_->setVisible(false);
    body()->addWidget(view_, 1);

    connect(jobCombo_, &QComboBox::currentIndexChanged, this, [this](int) { loadSnapshots(); });
    connect(refreshButton_, &QPushButton::clicked, this, &SnapshotsPage::refreshFromRepository);
    connect(browseButton_, &QPushButton::clicked, this, &SnapshotsPage::browseSelected);
    connect(restoreButton_, &QPushButton::clicked, this, &SnapshotsPage::restoreSelected);
    connect(retentionPreviewButton_, &QPushButton::clicked, this, &SnapshotsPage::previewRetention);
    connect(retentionApplyButton_, &QPushButton::clicked, this, &SnapshotsPage::applyRetention);
    connect(checkButton_, &QPushButton::clicked, this, &SnapshotsPage::runCheck);
    connect(unlockButton_, &QPushButton::clicked, this, &SnapshotsPage::unlockRepository);
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { updateActionState(); });

    updateActionState();
}

void SnapshotsPage::refresh() {
    reloadJobs();
    loadSnapshots();
}

void SnapshotsPage::reloadJobs() {
    const qint64 selected = jobCombo_->currentData().toLongLong();
    jobCombo_->blockSignals(true);
    jobCombo_->clear();

    jobs::JobStore store(context_.database());
    Result<QVector<jobs::ArchiveJob>> jobs = store.listJobs();
    if (jobs) {
        for (const jobs::ArchiveJob& job : jobs.value()) {
            if (job.isBackup()) {
                jobCombo_->addItem(job.name, QVariant(job.id));
            }
        }
    }
    const int index = jobCombo_->findData(QVariant(selected));
    if (index >= 0) {
        jobCombo_->setCurrentIndex(index);
    }
    jobCombo_->blockSignals(false);
}

qint64 SnapshotsPage::selectedJobId() const {
    return jobCombo_->currentData().toLongLong();
}

std::optional<jobs::ArchiveJob> SnapshotsPage::selectedJob() const {
    const qint64 id = selectedJobId();
    if (id <= 0) {
        return std::nullopt;
    }
    jobs::JobStore store(context_.database());
    Result<jobs::ArchiveJob> job = store.getJob(id);
    if (!job) {
        return std::nullopt;
    }
    return job.value();
}

QString SnapshotsPage::selectedSnapshotId() const {
    const QModelIndexList rows = view_->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return {};
    }
    return model_->item(rows.first().row(), 0)->data(Qt::UserRole).toString();
}

void SnapshotsPage::loadSnapshots() {
    model_->removeRows(0, model_->rowCount());
    const std::optional<jobs::ArchiveJob> job = selectedJob();
    if (!job.has_value()) {
        view_->setVisible(false);
        emptyState_->setVisible(true);
        emptyState_->setText(QStringLiteral(
            "No backup jobs yet. Create one on the Backup Jobs page (mode: Backup) with a Restic "
            "repository location."));
        statusLabel_->clear();
        updateActionState();
        return;
    }

    jobs::JobStore store(context_.database());
    Result<QVector<jobs::JobStore::SnapshotRecord>> snapshots = store.listSnapshots(job->id, 500);
    if (!snapshots) {
        statusLabel_->setText(QString::fromStdString(snapshots.error().toDisplayString()));
        updateActionState();
        return;
    }

    for (const jobs::JobStore::SnapshotRecord& record : snapshots.value()) {
        QList<QStandardItem*> row;
        auto* idItem = new QStandardItem(record.snapshotId.left(8));
        idItem->setData(record.snapshotId, Qt::UserRole);
        row.append(idItem);
        row.append(new QStandardItem(
            QDateTime::fromString(record.createdAt, Qt::ISODateWithMs)
                .toLocalTime()
                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        row.append(new QStandardItem(record.hostname));
        row.append(new QStandardItem(record.filesProcessed >= 0
                                         ? QString::number(record.filesProcessed)
                                         : QStringLiteral("-")));
        row.append(new QStandardItem(record.sizeBytes >= 0
                                         ? QString::number(record.sizeBytes)
                                         : QStringLiteral("-")));
        model_->appendRow(row);
    }

    const bool hasSnapshots = model_->rowCount() > 0;
    view_->setVisible(hasSnapshots);
    emptyState_->setVisible(!hasSnapshots);
    if (!hasSnapshots) {
        emptyState_->setText(QStringLiteral(
            "No snapshots recorded for '%1' yet. Run the job, or use \"Refresh from repository\" "
            "to read snapshots that already exist in %2.")
                                 .arg(job->name, job->destinationPath));
    }
    statusLabel_->setText(QStringLiteral("%1 - %2 snapshot(s) recorded")
                              .arg(job->name)
                              .arg(model_->rowCount()));
    updateActionState();
}

void SnapshotsPage::setBusy(bool busy, const QString& message) {
    busy_ = busy;
    busyBar_->setVisible(busy);
    if (!message.isEmpty()) {
        statusLabel_->setText(message);
    }
    updateActionState();
}

void SnapshotsPage::updateActionState() {
    const bool hasJob = selectedJobId() > 0;
    const bool hasSnapshot = !selectedSnapshotId().isEmpty();
    refreshButton_->setEnabled(hasJob && !busy_);
    browseButton_->setEnabled(hasJob && hasSnapshot && !busy_);
    restoreButton_->setEnabled(hasJob && hasSnapshot && !busy_);
    retentionPreviewButton_->setEnabled(hasJob && !busy_);
    retentionApplyButton_->setEnabled(hasJob && !busy_);
    checkButton_->setEnabled(hasJob && !busy_);
    unlockButton_->setEnabled(hasJob && !busy_);
}

void SnapshotsPage::refreshFromRepository() {
    const std::optional<jobs::ArchiveJob> job = selectedJob();
    if (!job.has_value() || busy_) {
        return;
    }
    const jobs::ArchiveJob selectedJobValue = job.value();
    const QString databasePath = Paths::databasePath();
    const QString resticExecutable = resticPath(context_);
    const jobs::PasswordOptions passwordOptions =
        jobs::passwordOptionsFromSettings(context_.database());

    setBusy(true, QStringLiteral("Reading snapshots from %1 ...").arg(selectedJobValue.destinationPath));
    auto* watcher = new QFutureWatcher<int>(this);
    watcher->setFuture(QtConcurrent::run([selectedJobValue, databasePath, resticExecutable,
                                          passwordOptions]() {
        const Result<QByteArray> password =
            jobs::loadRepositoryPassword(selectedJobValue.destinationPath, passwordOptions);
        if (!password) {
            return -1;
        }
        const Result<QString> passwordFile = backup::createPasswordFile(password.value());
        if (!passwordFile) {
            return -1;
        }
        backup::ResticRepository repository(resticExecutable, selectedJobValue.destinationPath,
                                            passwordFile.value());
        const Result<QVector<backup::SnapshotInfo>> snapshots = repository.snapshots();
        QFile::remove(passwordFile.value());
        if (!snapshots) {
            return -1;
        }

        db::Database database;
        const Status opened = database.open(databasePath);
        if (!opened) {
            return -1;
        }
        jobs::JobStore store(database);
        int count = 0;
        for (const backup::SnapshotInfo& snapshot : snapshots.value()) {
            jobs::JobStore::SnapshotRecord record;
            record.snapshotId = snapshot.id;
            record.repositoryPath = selectedJobValue.destinationPath;
            record.createdAt = snapshot.time.isValid()
                                   ? snapshot.time.toUTC().toString(Qt::ISODateWithMs)
                                   : QString();
            record.sizeBytes = snapshot.totalBytes;
            record.filesProcessed = snapshot.filesProcessed;
            record.hostname = snapshot.hostname;
            if (store.recordSnapshot(selectedJobValue.id, 0, record).ok()) {
                ++count;
            }
        }
        return count;
    }));
    connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher] {
        const int count = watcher->result();
        watcher->deleteLater();
        setBusy(false, count >= 0
                           ? QStringLiteral("Read %1 snapshot(s) from the repository.").arg(count)
                           : QStringLiteral(
                                 "Could not read the repository. Check the password (keyring) and "
                                 "that Restic is installed."));
        loadSnapshots();
    });
}

void SnapshotsPage::browseSelected() {
    const std::optional<jobs::ArchiveJob> job = selectedJob();
    const QString snapshotId = selectedSnapshotId();
    if (!job.has_value() || snapshotId.isEmpty()) {
        return;
    }
    SnapshotBrowserDialog dialog(context_, job.value(), snapshotId, this);
    dialog.exec();
}

void SnapshotsPage::restoreSelected() {
    const std::optional<jobs::ArchiveJob> job = selectedJob();
    const QString snapshotId = selectedSnapshotId();
    if (!job.has_value() || snapshotId.isEmpty()) {
        return;
    }
    SnapshotRestoreDialog dialog(context_, job.value(), snapshotId, this);
    dialog.exec();
}

void SnapshotsPage::previewRetention() {
    const std::optional<jobs::ArchiveJob> job = selectedJob();
    if (!job.has_value() || busy_) {
        return;
    }
    const jobs::ArchiveJob selectedJobValue = job.value();
    const QString resticExecutable = resticPath(context_);
    const jobs::PasswordOptions passwordOptions =
        jobs::passwordOptionsFromSettings(context_.database());

    setBusy(true, QStringLiteral("Comparing the retention policy with the repository (dry run)..."));
    auto* watcher = new QFutureWatcher<RetentionOutcome>(this);
    watcher->setFuture(QtConcurrent::run([selectedJobValue, resticExecutable, passwordOptions]() {
        RetentionOutcome outcome;
        const Result<QByteArray> password =
            jobs::loadRepositoryPassword(selectedJobValue.destinationPath, passwordOptions);
        if (!password) {
            outcome.summary = QString::fromStdString(password.error().toDisplayString());
            return outcome;
        }
        const Result<QString> passwordFile = backup::createPasswordFile(password.value());
        if (!passwordFile) {
            outcome.summary = QString::fromStdString(passwordFile.error().toDisplayString());
            return outcome;
        }
        backup::ResticRepository repository(resticExecutable, selectedJobValue.destinationPath,
                                            passwordFile.value());
        const Result<QVector<backup::ForgetGroup>> groups =
            repository.forget(selectedJobValue.retention, true, false);
        QFile::remove(passwordFile.value());
        if (!groups) {
            outcome.summary = QString::fromStdString(groups.error().toDisplayString());
            return outcome;
        }
        outcome.ok = true;
        for (const backup::ForgetGroup& group : groups.value()) {
            outcome.kept += static_cast<int>(group.keptIds.size());
            outcome.removed += static_cast<int>(group.removedIds.size());
            outcome.removedIds.append(group.removedIds);
        }
        outcome.summary = QStringLiteral(
            "Dry run: %1 snapshot(s) would be kept, %2 would be removed. Nothing was deleted.")
                              .arg(outcome.kept)
                              .arg(outcome.removed);
        return outcome;
    }));
    connect(watcher, &QFutureWatcher<RetentionOutcome>::finished, this, [this, watcher] {
        const RetentionOutcome outcome = watcher->result();
        watcher->deleteLater();
        setBusy(false, outcome.summary);
        if (!outcome.ok) {
            QMessageBox::warning(this, QStringLiteral("Retention preview"), outcome.summary);
        }
    });
}

void SnapshotsPage::applyRetention() {
    const std::optional<jobs::ArchiveJob> job = selectedJob();
    if (!job.has_value() || busy_) {
        return;
    }
    if (job->retention.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("Retention"),
            QStringLiteral("This job has no retention policy configured, so nothing would be "
                           "deleted. Edit the job to set keep-last/daily/weekly/monthly/yearly."));
        return;
    }

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Apply retention?"),
        QStringLiteral("Apply the retention policy of '%1' and delete the snapshots it does not "
                       "keep?\n\nThis cannot be undone. Data packs remain until you prune.")
            .arg(job->name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    const jobs::ArchiveJob selectedJobValue = job.value();
    const QString resticExecutable = resticPath(context_);
    const jobs::PasswordOptions passwordOptions =
        jobs::passwordOptionsFromSettings(context_.database());
    const auto prune = QMessageBox::question(
        this, QStringLiteral("Prune too?"),
        QStringLiteral("Also run 'restic prune' to reclaim disk space? It rewrites the "
                       "repository and takes longer."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    setBusy(true, QStringLiteral("Applying retention..."));
    auto* watcher = new QFutureWatcher<RetentionOutcome>(this);
    watcher->setFuture(QtConcurrent::run([selectedJobValue, resticExecutable, passwordOptions,
                                          prune]() {
        RetentionOutcome outcome;
        outcome.applied = true;
        const Result<QByteArray> password =
            jobs::loadRepositoryPassword(selectedJobValue.destinationPath, passwordOptions);
        if (!password) {
            outcome.summary = QString::fromStdString(password.error().toDisplayString());
            return outcome;
        }
        const Result<QString> passwordFile = backup::createPasswordFile(password.value());
        if (!passwordFile) {
            outcome.summary = QString::fromStdString(passwordFile.error().toDisplayString());
            return outcome;
        }
        backup::ResticRepository repository(resticExecutable, selectedJobValue.destinationPath,
                                            passwordFile.value());
        const Result<QVector<backup::ForgetGroup>> groups =
            repository.forget(selectedJobValue.retention, false, prune == QMessageBox::Yes);
        QFile::remove(passwordFile.value());
        if (!groups) {
            outcome.summary = QString::fromStdString(groups.error().toDisplayString());
            return outcome;
        }
        outcome.ok = true;
        for (const backup::ForgetGroup& group : groups.value()) {
            outcome.kept += static_cast<int>(group.keptIds.size());
            outcome.removed += static_cast<int>(group.removedIds.size());
            outcome.removedIds.append(group.removedIds);
        }
        outcome.summary = QStringLiteral("Retention applied: %1 kept, %2 removed%3")
                              .arg(outcome.kept)
                              .arg(outcome.removed)
                              .arg(prune == QMessageBox::Yes ? QStringLiteral(", repository pruned")
                                                             : QString());
        return outcome;
    }));
    connect(watcher, &QFutureWatcher<RetentionOutcome>::finished, this, [this, watcher] {
        const RetentionOutcome outcome = watcher->result();
        watcher->deleteLater();
        setBusy(false, outcome.summary);
        if (!outcome.ok) {
            QMessageBox::warning(this, QStringLiteral("Retention"), outcome.summary);
        } else {
            context_.recordEvent(LogLevel::Info, QStringLiteral("backup"), outcome.summary);
            refreshFromRepository();
        }
    });
}

void SnapshotsPage::runCheck() {
    const std::optional<jobs::ArchiveJob> job = selectedJob();
    if (!job.has_value() || busy_) {
        return;
    }
    const jobs::ArchiveJob selectedJobValue = job.value();
    const QString resticExecutable = resticPath(context_);
    const jobs::PasswordOptions passwordOptions =
        jobs::passwordOptionsFromSettings(context_.database());

    setBusy(true, QStringLiteral("Verifying the repository (5% of the data is read)..."));
    auto* watcher = new QFutureWatcher<VerificationOutcome>(this);
    watcher->setFuture(QtConcurrent::run([selectedJobValue, resticExecutable, passwordOptions]() {
        VerificationOutcome outcome;
        const Result<QByteArray> password =
            jobs::loadRepositoryPassword(selectedJobValue.destinationPath, passwordOptions);
        if (!password) {
            outcome.summary = QString::fromStdString(password.error().toDisplayString());
            return outcome;
        }
        const Result<QString> passwordFile = backup::createPasswordFile(password.value());
        if (!passwordFile) {
            outcome.summary = QString::fromStdString(passwordFile.error().toDisplayString());
            return outcome;
        }
        backup::ResticRepository repository(resticExecutable, selectedJobValue.destinationPath,
                                            passwordFile.value());
        const Result<backup::VerifyResult> result = repository.check(QStringLiteral("5%"));
        QFile::remove(passwordFile.value());
        if (!result) {
            outcome.summary = QString::fromStdString(result.error().toDisplayString());
            return outcome;
        }
        outcome.ok = result.value().ok;
        outcome.summary = result.value().ok
                              ? QStringLiteral("Repository check passed (0 errors).")
                              : QStringLiteral("Repository check FAILED: %1 error(s).")
                                    .arg(result.value().errors);
        return outcome;
    }));
    connect(watcher, &QFutureWatcher<VerificationOutcome>::finished, this, [this, watcher] {
        const VerificationOutcome outcome = watcher->result();
        watcher->deleteLater();
        setBusy(false, outcome.summary);
        if (!outcome.ok) {
            QMessageBox::warning(this, QStringLiteral("Repository check"), outcome.summary);
        }
    });
}

void SnapshotsPage::unlockRepository() {
    const std::optional<jobs::ArchiveJob> job = selectedJob();
    if (!job.has_value() || busy_) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Unlock repository"),
        QStringLiteral("Remove stale locks from %1?\n\nOnly do this when no other restic process "
                       "is running against this repository.")
            .arg(job->destinationPath),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    const jobs::ArchiveJob selectedJobValue = job.value();
    const QString resticExecutable = resticPath(context_);
    const jobs::PasswordOptions passwordOptions =
        jobs::passwordOptionsFromSettings(context_.database());

    setBusy(true, QStringLiteral("Removing stale locks..."));
    auto* watcher = new QFutureWatcher<QString>(this);
    watcher->setFuture(QtConcurrent::run([selectedJobValue, resticExecutable, passwordOptions]() {
        const Result<QByteArray> password =
            jobs::loadRepositoryPassword(selectedJobValue.destinationPath, passwordOptions);
        if (!password) {
            return QString::fromStdString(password.error().toDisplayString());
        }
        const Result<QString> passwordFile = backup::createPasswordFile(password.value());
        if (!passwordFile) {
            return QString::fromStdString(passwordFile.error().toDisplayString());
        }
        backup::ResticRepository repository(resticExecutable, selectedJobValue.destinationPath,
                                            passwordFile.value());
        const Status unlocked = repository.unlock();
        QFile::remove(passwordFile.value());
        return unlocked.ok() ? QStringLiteral("Stale locks removed.")
                             : QString::fromStdString(unlocked.error().toDisplayString());
    }));
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const QString message = watcher->result();
        watcher->deleteLater();
        setBusy(false, message);
    });
}

} // namespace nesva::ui
