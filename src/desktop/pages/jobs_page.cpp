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

#include "app_context.hpp"
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
           QStringLiteral("Archive jobs copy files to a destination. Backup jobs with Restic "
                          "snapshots arrive in Phase 4."),
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

    auto* progressRow = new QHBoxLayout();
    progressRow->setSpacing(8);
    runLabel_ = new QLabel(this);
    runLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    runProgress_ = new QProgressBar(this);
    runProgress_->setVisible(false);
    runProgress_->setRange(0, 0);
    progressRow->addWidget(runLabel_, 1);
    progressRow->addWidget(runProgress_, 1);
    body()->addLayout(progressRow);

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(QStringLiteral("color: #c0392b;"));
    errorLabel_->setVisible(false);
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
    errorLabel_->setText(error);
    errorLabel_->setVisible(!error.isEmpty());

    const bool hasJobs = model_->jobCount() > 0;
    view_->setVisible(hasJobs);
    emptyState_->setVisible(!hasJobs);
    if (!hasJobs) {
        emptyState_->setText(QStringLiteral(
            "No jobs yet. Use \"New job...\" to create an archive job: choose the folders to copy "
            "and a destination folder or storage provider."));
    }

    agent::AgentClient client;
    const bool agentRunning = client.isReachable(200);
    setSubtitle(QStringLiteral("Archive jobs copy files to a destination. Runs start in the %1. "
                               "Backup jobs with Restic snapshots arrive in Phase 4. Database: %2")
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
