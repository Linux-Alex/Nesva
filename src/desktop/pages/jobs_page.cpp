// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "jobs_page.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableView>

#include <QtCore/QDateTime>
#include <QtSql/QSqlQuery>

#include "app_context.hpp"
#include "nesva/core/disk_space.hpp"
#include "widgets/elided_label.hpp"
#include "nesva/agent/client.hpp"
#include "dialogs/history_dialog.hpp"
#include "dialogs/job_editor_dialog.hpp"
#include "job_executor.hpp"
#include "models/job_list_model.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/jobs/job_store.hpp"

namespace nesva::ui {

JobsPage::JobsPage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Backup Jobs"),
           QStringLiteral("Archive jobs copy files to a destination; backup jobs create "
                          "versioned, encrypted Restic snapshots. Mail jobs archive a mailbox."),
           parent),
      context_(context) {
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(6);
    newButton_ = new QPushButton(QStringLiteral("New job..."), this);
    editButton_ = new QPushButton(QStringLiteral("Edit..."), this);
    deleteButton_ = new QPushButton(QStringLiteral("Delete"), this);
    runButton_ = new QPushButton(QStringLiteral("Run now"), this);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), this);
    historyButton_ = new QPushButton(QStringLiteral("History..."), this);
    toolbar->addWidget(newButton_);
    toolbar->addWidget(editButton_);
    toolbar->addWidget(deleteButton_);
    toolbar->addSpacing(12);
    toolbar->addWidget(runButton_);
    toolbar->addWidget(cancelButton_);
    toolbar->addWidget(historyButton_);
    toolbar->addStretch(1);
    auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    toolbar->addWidget(refreshButton);
    body()->addLayout(toolbar);

    // Status lines keep one fixed height: a longer message must never wrap and
    // push the table below it around while a run is in progress.
    auto* progressRow = new QHBoxLayout();
    progressRow->setSpacing(8);
    runLabel_ = new ElidedLabel(this);
    runLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    runProgress_ = new QProgressBar(this);
    runProgress_->setVisible(false);
    runProgress_->setRange(0, 0);
    progressRow->addWidget(runLabel_, 1);
    progressRow->addWidget(runProgress_, 0);
    body()->addLayout(progressRow);

    errorLabel_ = new ElidedLabel(this);
    errorLabel_->setStyleSheet(QStringLiteral("color: #c0392b;"));
    body()->addWidget(errorLabel_);

    emptyState_ = new QLabel(this);
    emptyState_->setObjectName(QStringLiteral("emptyState"));
    emptyState_->setWordWrap(true);
    emptyState_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    body()->addWidget(emptyState_, 1);

    model_ = new JobListModel(context_.database(), this);
    view_ = new QTableView(this);
    view_->setModel(model_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->horizontalHeader()->setSectionResizeMode(JobListModel::NameColumn, QHeaderView::Stretch);
    view_->horizontalHeader()->setSectionResizeMode(JobListModel::DestinationColumn,
                                                     QHeaderView::Stretch);
    // The ID and the source count are short and should never be elided.
    view_->horizontalHeader()->setSectionResizeMode(JobListModel::IdColumn,
                                                    QHeaderView::ResizeToContents);
    view_->horizontalHeader()->setSectionResizeMode(JobListModel::SourcesColumn,
                                                    QHeaderView::ResizeToContents);
    view_->verticalHeader()->setDefaultSectionSize(28);
    view_->setVisible(false);
    body()->addWidget(view_, 1);

    connect(refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    connect(newButton_, &QPushButton::clicked, this, &JobsPage::createJob);
    connect(editButton_, &QPushButton::clicked, this, &JobsPage::editJob);
    connect(deleteButton_, &QPushButton::clicked, this, &JobsPage::deleteJob);
    connect(runButton_, &QPushButton::clicked, this, &JobsPage::runJob);
    connect(cancelButton_, &QPushButton::clicked, this, &JobsPage::cancelJob);
    connect(historyButton_, &QPushButton::clicked, this, &JobsPage::showHistory);
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { updateActionState(); });
    connect(view_, &QTableView::doubleClicked, this, [this](const QModelIndex&) { editJob(); });

    JobExecutor* executor = context_.executor();
    if (executor != nullptr) {
        connect(executor, &JobExecutor::started, this, [this](qint64, const QString& name) {
            setRunBusy(true);
            runLabel_->setText(QStringLiteral("Running %1...").arg(name));
        });
        connect(executor, &JobExecutor::progressChanged, this,
                [this](qint64, const jobs::RunProgress& progress) {
                    if (progress.filesTotal > 0) {
                        runProgress_->setRange(0, static_cast<int>(progress.filesTotal));
                        runProgress_->setValue(static_cast<int>(progress.filesProcessed));
                    } else {
                        runProgress_->setRange(0, 0);
                    }
                    QString text = progress.message;
                    if (!progress.currentFile.isEmpty()) {
                        text = QStringLiteral("%1 - %2").arg(progress.message, progress.currentFile);
                    }
                    runLabel_->setText(text);
                });
        connect(executor, &JobExecutor::finished, this,
                [this](qint64, const jobs::RunReport& report) {
                    setRunBusy(false);
                    runLabel_->setText(
                        QStringLiteral("%1: %2 transferred, %3 unchanged, %4 failed")
                            .arg(report.status)
                            .arg(report.counters.filesTransferred)
                            .arg(report.counters.filesSkipped)
                            .arg(report.counters.filesFailed));
                    refresh();
                    if (report.status == QStringLiteral("failed") && !report.errors.isEmpty()) {
                        QMessageBox::warning(this, QStringLiteral("Archive run failed"),
                                             report.errors.join(QLatin1Char('\n')));
                    }
                });
    }

    updateActionState();
}

void JobsPage::refresh() {
    const QString error = model_->refresh();
    // The label keeps its row: showing or clearing an error must not move the
    // table, only fill the line that is already reserved for it.
    errorLabel_->setText(error);

    const bool hasJobs = model_->jobCount() > 0;
    view_->setVisible(hasJobs);
    emptyState_->setVisible(!hasJobs);
    if (!hasJobs) {
        emptyState_->setText(QStringLiteral(
            "No jobs yet. Use \"New job...\" to create an archive job: choose the folders to copy "
            "and a destination folder or storage provider."));
    }

    // A run started elsewhere - by the agent, or from a terminal - used to be
    // invisible here: the page only knew about runs its own executor started.
    // Read the running row from the database so the window always shows it.
    const bool ownRun = context_.executor() != nullptr && context_.executor()->isRunning();
    if (!ownRun) {
        Result<QSqlQuery> running = context_.database().select(QStringLiteral(
            "SELECT j.name, r.files_total, r.files_transferred, r.files_failed, "
            "r.bytes_total, r.bytes_transferred, COALESCE(r.started_at, '') "
            "FROM job_runs r JOIN backup_jobs j ON j.id = r.job_id "
            "WHERE r.status = 'running' ORDER BY r.id DESC LIMIT 1"));
        if (running) {
            QSqlQuery row = std::move(running).take();
            if (row.next()) {
                const QString name = row.value(0).toString();
                const qint64 total = row.value(1).toLongLong();
                const qint64 done = row.value(2).toLongLong();
                Q_UNUSED(row.value(3).toLongLong()); // failed, reported on finish
                const qint64 bytesTotal = row.value(4).toLongLong();
                const qint64 bytesDone = row.value(5).toLongLong();
                const QDateTime started =
                    QDateTime::fromString(row.value(6).toString(), Qt::ISODateWithMs).toUTC();
                const double elapsed = started.isValid()
                                           ? static_cast<double>(started.secsTo(
                                                 QDateTime::currentDateTimeUtc()))
                                           : 0.0;

                runProgress_->setVisible(true);
                if (done > 0 && total > 0) {
                    // Per-file progress (the per-file transfer path).
                    runProgress_->setRange(0, static_cast<int>(total));
                    runProgress_->setValue(static_cast<int>(done));
                    const double rate = elapsed > 0 ? done / elapsed : 0.0;
                    const qint64 eta = rate > 0 ? static_cast<qint64>((total - done) / rate) : -1;
                    runLabel_->setText(QStringLiteral("%1: %2/%3 files (%4%) - %5/min - %6 left")
                                           .arg(name)
                                           .arg(done)
                                           .arg(total)
                                           .arg(QString::number(100.0 * done / total, 'f', 1))
                                           .arg(QString::number(rate * 60.0, 'f', 0),
                                                eta >= 0 ? QStringLiteral("about %1 min")
                                                               .arg((eta + 59) / 60)
                                                         : QStringLiteral("ETA unknown")));
                } else if (bytesTotal > 0) {
                    // Batched transfer: progress in bytes, which is what moves.
                    const double percent = 100.0 * bytesDone / bytesTotal;
                    runProgress_->setRange(0, 1000);
                    runProgress_->setValue(static_cast<int>(percent * 10));
                    const double speed = elapsed > 0 ? bytesDone / elapsed : 0.0;
                    const qint64 eta =
                        speed > 0 ? static_cast<qint64>((bytesTotal - bytesDone) / speed) : -1;
                    runLabel_->setText(
                        QStringLiteral("%1: %2 of %3 (%4%) - %5/s - %6 left")
                            .arg(name, humanBytes(bytesDone), humanBytes(bytesTotal),
                                 QString::number(percent, 'f', 1),
                                 humanBytes(static_cast<qint64>(speed)),
                                 eta >= 0 ? QStringLiteral("about %1 min").arg((eta + 59) / 60)
                                          : QStringLiteral("ETA unknown")));
                } else {
                    runProgress_->setRange(0, 0);
                    runLabel_->setText(QStringLiteral("%1: scanning...").arg(name));
                }
            }
        }
    }

    agent::AgentClient client;
    const bool agentRunning = client.isReachable(200);
    setSubtitle(QStringLiteral("Archive jobs copy files to a destination; backup jobs create "
                               "versioned Restic snapshots. Runs start in the %1. Database: %2")
                    .arg(agentRunning ? QStringLiteral("background agent")
                                      : QStringLiteral("application"),
                         context_.database().filePath()));
    updateActionState();
}

qint64 JobsPage::selectedJobId() const {
    const QModelIndexList rows = view_->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return 0;
    }
    const QModelIndex idIndex = model_->index(rows.first().row(), JobListModel::IdColumn);
    return model_->data(idIndex).toString().toLongLong();
}

void JobsPage::updateActionState() {
    const qint64 id = selectedJobId();
    const bool hasSelection = id > 0;
    JobExecutor* executor = context_.executor();
    const bool running = executor != nullptr && executor->isRunning();

    editButton_->setEnabled(hasSelection && !running);
    deleteButton_->setEnabled(hasSelection && !running);
    runButton_->setEnabled(hasSelection && !running);
    historyButton_->setEnabled(hasSelection);
    cancelButton_->setEnabled(running);
}

void JobsPage::setRunBusy(bool busy) {
    runProgress_->setVisible(busy);
    if (busy) {
        runProgress_->setRange(0, 0);
    }
    updateActionState();
}

void JobsPage::createJob() {
    JobEditorDialog dialog(context_, std::nullopt, this);
    if (dialog.exec() == QDialog::Accepted) {
        refresh();
    }
}

void JobsPage::editJob() {
    const qint64 id = selectedJobId();
    if (id <= 0) {
        return;
    }
    jobs::JobStore store(context_.database());
    Result<jobs::ArchiveJob> job = store.getJob(id);
    if (!job) {
        QMessageBox::warning(this, QStringLiteral("Edit job"),
                             QString::fromStdString(job.error().toDisplayString()));
        return;
    }
    JobEditorDialog dialog(context_, job.value(), this);
    if (dialog.exec() == QDialog::Accepted) {
        refresh();
    }
}

void JobsPage::deleteJob() {
    const qint64 id = selectedJobId();
    if (id <= 0) {
        return;
    }
    const auto answer =
        QMessageBox::question(this, QStringLiteral("Delete job"),
                              QStringLiteral("Delete this job and its run history? Archived files "
                                             "at the destination are not touched."),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    jobs::JobStore store(context_.database());
    const Status status = store.deleteJob(id);
    if (!status.ok()) {
        QMessageBox::warning(this, QStringLiteral("Delete job"),
                             QString::fromStdString(status.error().toDisplayString()));
        return;
    }
    context_.recordEvent(LogLevel::Info, QStringLiteral("jobs"),
                         QStringLiteral("Job %1 deleted").arg(id));
    refresh();
}

void JobsPage::runJob() {
    const qint64 id = selectedJobId();
    if (id <= 0) {
        return;
    }
    // Prefer the background agent: its run survives closing this window and is
    // the same path a scheduled run takes. Fall back to an in-process run when
    // the agent is not running.
    agent::AgentClient client;
    if (client.isReachable(400)) {
        const Result<QString> accepted = client.runJob(id, false, QStringLiteral("desktop"));
        if (accepted) {
            runLabel_->setText(
                QStringLiteral("The background agent accepted the job and is running it."));
            return;
        }
        QMessageBox::warning(this, QStringLiteral("Run job"),
                             QString::fromStdString(accepted.error().toDisplayString()));
        return;
    }

    JobExecutor* executor = context_.executor();
    if (executor == nullptr) {
        return;
    }
    const Status started = executor->startRun(id, /*dryRun=*/false);
    if (!started.ok()) {
        QMessageBox::warning(this, QStringLiteral("Run job"),
                             QString::fromStdString(started.error().toDisplayString()));
        return;
    }
    setRunBusy(true);
}

void JobsPage::cancelJob() {
    if (JobExecutor* executor = context_.executor()) {
        executor->cancel();
        runLabel_->setText(QStringLiteral("Cancelling..."));
    }
}

void JobsPage::showHistory() {
    const qint64 id = selectedJobId();
    HistoryDialog dialog(context_, id > 0 ? std::optional<qint64>(id) : std::nullopt, this);
    dialog.exec();
}

} // namespace nesva::ui
