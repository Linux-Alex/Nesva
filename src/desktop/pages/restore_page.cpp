// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "restore_page.hpp"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDir>
#include <QtCore/QFuture>
#include <QtCore/QFutureWatcher>
#include <QtCore/QMetaObject>
#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableView>

#include <utility>

#include "app_context.hpp"
#include "nesva/config/settings_store.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/archive_restore.hpp"

namespace nesva::ui {

RestorePage::RestorePage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Restore"),
           QStringLiteral("Restore archived files. Restic snapshots are restored from the "
                          "Snapshots page, where the snapshot is the unit of restore. Existing "
                          "files are never overwritten without an explicit choice."),
           parent),
      context_(context) {
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);

    jobCombo_ = new QComboBox(this);
    jobCombo_->setMinimumWidth(200);
    toolbar->addWidget(jobCombo_);

    auto* refreshButton = new QPushButton(QStringLiteral("Load"), this);
    toolbar->addWidget(refreshButton);
    toolbar->addStretch(1);
    body()->addLayout(toolbar);

    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({QStringLiteral("Path"), QStringLiteral("Bytes"),
                                       QStringLiteral("State")});
    view_ = new QTableView(this);
    view_->setModel(model_);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    body()->addWidget(view_, 1);

    auto* options = new QHBoxLayout();
    options->setSpacing(8);

    destinationEdit_ = new QLineEdit(this);
    destinationEdit_->setPlaceholderText(QStringLiteral("Restore destination folder"));
    options->addWidget(destinationEdit_, 1);
    auto* browseButton = new QPushButton(QStringLiteral("Browse..."), this);
    options->addWidget(browseButton);

    policyCombo_ = new QComboBox(this);
    policyCombo_->addItem(QStringLiteral("Skip existing files"), QStringLiteral("skip"));
    policyCombo_->addItem(QStringLiteral("Rename existing files"), QStringLiteral("rename"));
    policyCombo_->addItem(QStringLiteral("Overwrite existing files"), QStringLiteral("overwrite"));
    options->addWidget(policyCombo_);
    body()->addLayout(options);

    conflictLabel_ = new QLabel(this);
    conflictLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    conflictLabel_->setWordWrap(true);
    body()->addWidget(conflictLabel_);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    restoreSelectedButton_ = new QPushButton(QStringLiteral("Restore selected"), this);
    restoreAllButton_ = new QPushButton(QStringLiteral("Restore all"), this);
    progress_ = new QProgressBar(this);
    progress_->setVisible(false);
    actions->addWidget(restoreSelectedButton_);
    actions->addWidget(restoreAllButton_);
    actions->addWidget(progress_, 1);
    body()->addLayout(actions);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    statusLabel_->setWordWrap(true);
    body()->addWidget(statusLabel_);

    connect(refreshButton, &QPushButton::clicked, this, [this] { loadPlan(); });
    connect(jobCombo_, &QComboBox::currentIndexChanged, this, [this](int) { loadPlan(); });
    connect(browseButton, &QPushButton::clicked, this, &RestorePage::browseDestination);
    connect(restoreSelectedButton_, &QPushButton::clicked, this, &RestorePage::startRestore);
    connect(restoreAllButton_, &QPushButton::clicked, this, [this] {
        view_->clearSelection();
        startRestore();
    });
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { updateActionState(); });

    updateActionState();
}

RestorePage::~RestorePage() {
    if (watcher_ && watcher_->isRunning()) {
        cancellation_.cancel();
        watcher_->waitForFinished();
    }
}

void RestorePage::refresh() {
    reloadJobs();
    loadPlan();
}

void RestorePage::reloadJobs() {
    const qint64 selected = jobCombo_->currentData().toLongLong();
    jobCombo_->blockSignals(true);
    jobCombo_->clear();

    jobs::JobStore store(context_.database());
    Result<QVector<jobs::ArchiveJob>> jobs = store.listJobs();
    if (jobs) {
        for (const jobs::ArchiveJob& job : jobs.value()) {
            if (job.mode == QStringLiteral("archive")) {
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

void RestorePage::loadPlan() {
    items_.clear();
    job_.reset();
    provider_.reset();
    model_->removeRows(0, model_->rowCount());

    if (!jobCombo_->currentData().isValid()) {
        statusLabel_->setText(QStringLiteral(
            "No archive jobs yet. Runs appear here once a job has archived at least one file."));
        updateActionState();
        return;
    }

    jobs::JobStore store(context_.database());
    const qint64 jobId = jobCombo_->currentData().toLongLong();
    Result<jobs::ArchiveJob> loaded = store.getJob(jobId);
    if (!loaded) {
        statusLabel_->setText(QString::fromStdString(loaded.error().toDisplayString()));
        updateActionState();
        return;
    }
    job_ = loaded.value();
    if (job_->storageProviderId) {
        Result<jobs::StorageProviderRecord> record = store.getProvider(*job_->storageProviderId);
        if (record) {
            provider_ = record.value();
        }
    }

    const QString rcloneExecutable =
        context_.settings()
            .getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));

    const QString destination = destinationEdit_->text().trimmed();
    jobs::ArchiveRestorer restorer(context_.database());
    Result<jobs::RestorePlan> plan =
        restorer.plan(job_.value(), provider_, destination, {}, rcloneExecutable,
                      !destination.isEmpty());
    if (!plan) {
        statusLabel_->setText(QString::fromStdString(plan.error().toDisplayString()));
        updateActionState();
        return;
    }

    items_ = plan.value().items;
    for (const jobs::ArchiveItem& item : items_) {
        QList<QStandardItem*> row;
        row.append(new QStandardItem(item.relativePath));
        row.append(new QStandardItem(QString::number(item.sizeBytes)));
        row.append(new QStandardItem(item.state.isEmpty() ? QStringLiteral("unknown") : item.state));
        model_->appendRow(row);
    }

    if (plan.value().hasConflicts()) {
        conflictLabel_->setText(
            QStringLiteral("%1 file(s) already exist in the destination, for example: %2")
                .arg(plan.value().conflictingFiles)
                .arg(plan.value().conflictSamples.join(QStringLiteral(", "))));
    } else {
        conflictLabel_->setText(QStringLiteral("%1 object(s) available to restore.")
                                    .arg(items_.size()));
    }
    statusLabel_->setText(QString());
    updateActionState();
}

void RestorePage::browseDestination() {
    const QString directory =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Select the restore destination"));
    if (directory.isEmpty()) {
        return;
    }
    destinationEdit_->setText(QDir::cleanPath(directory));
    loadPlan();
}

void RestorePage::updateActionState() {
    const bool hasItems = !items_.isEmpty();
    const bool hasDestination = !destinationEdit_->text().trimmed().isEmpty();
    const bool busy = restoring_;
    restoreAllButton_->setEnabled(hasItems && hasDestination && !busy);
    restoreSelectedButton_->setEnabled(hasItems && hasDestination && !busy &&
                                       view_->selectionModel()->hasSelection());
}

void RestorePage::startRestore() {
    if (!job_ || items_.isEmpty() || restoring_) {
        return;
    }
    const QString destination = destinationEdit_->text().trimmed();
    if (destination.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Restore"),
                             QStringLiteral("Choose a restore destination first."));
        return;
    }

    QStringList onlyPaths;
    const QModelIndexList selected = view_->selectionModel()->selectedRows();
    for (const QModelIndex& index : selected) {
        onlyPaths.append(model_->item(index.row(), 0)->text());
    }
    const bool restoreEverything = onlyPaths.isEmpty();

    if (!restoreEverything && policyCombo_->currentData().toString() == QStringLiteral("overwrite")) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Overwrite files?"),
            QStringLiteral("Overwrite existing files in the destination?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    jobs::RestoreRequest request;
    request.job = job_.value();
    request.providerRecord = provider_;
    request.destinationDirectory = destination;
    request.policy = jobs::conflictPolicyFromString(policyCombo_->currentData().toString(),
                                                    jobs::ConflictPolicy::Skip);
    request.onlyPaths = onlyPaths;
    request.verify = true;

    const QString rcloneExecutable =
        context_.settings()
            .getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));

    restoring_ = true;
    cancellation_ = CancellationToken();
    progress_->setVisible(true);
    progress_->setRange(0, restoreEverything ? 0 : static_cast<int>(onlyPaths.size()));
    progress_->setValue(0);
    statusLabel_->setText(QStringLiteral("Restoring..."));
    updateActionState();

    // The restore runs off the GUI thread with its own database connection;
    // progress is marshalled back through queued invocations.
    const QString databasePath = Paths::databasePath();
    auto* watcher = new QFutureWatcher<jobs::RestoreReport>(this);
    watcher_.reset(watcher);

    QFuture<jobs::RestoreReport> future = QtConcurrent::run([this, request, rcloneExecutable, databasePath]() {
        db::Database database;
        const Status opened = database.open(databasePath);
        if (!opened) {
            jobs::RestoreReport report;
            report.errors.append(QString::fromStdString(opened.error().toDisplayString()));
            return report;
        }
        jobs::ArchiveRestorer restorer(database, [this](const jobs::RestoreProgress& progress) {
            const int done = static_cast<int>(progress.filesDone);
            const int total = static_cast<int>(progress.filesTotal);
            const QString message = progress.currentFile;
            QMetaObject::invokeMethod(
                this,
                [this, done, total, message] {
                    if (total > 0) {
                        progress_->setRange(0, total);
                        progress_->setValue(done);
                    }
                    statusLabel_->setText(QStringLiteral("Restoring %1").arg(message));
                },
                Qt::QueuedConnection);
        });
        return restorer.restore(request, cancellation_, rcloneExecutable);
    });

    connect(watcher, &QFutureWatcher<jobs::RestoreReport>::finished, this,
            &RestorePage::finishRestore);
    watcher->setFuture(future);
}

void RestorePage::finishRestore() {
    restoring_ = false;
    progress_->setVisible(false);

    auto* watcher = static_cast<QFutureWatcher<jobs::RestoreReport>*>(watcher_.get());
    if (watcher != nullptr && watcher->isFinished()) {
        const jobs::RestoreReport report = watcher->result();
        statusLabel_->setText(
            QStringLiteral("%1: %2 restored, %3 skipped, %4 renamed, %5 failed, %6 verified")
                .arg(report.status)
                .arg(report.restored)
                .arg(report.skipped)
                .arg(report.renamed)
                .arg(report.failed)
                .arg(report.verifiedFiles.size()));
        if (!report.errors.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Restore finished with problems"),
                                 report.errors.join(QLatin1Char('\n')));
        }
        // Refresh counters shown elsewhere.
        emit context_.statusChanged();
    }
    updateActionState();
}

} // namespace nesva::ui
