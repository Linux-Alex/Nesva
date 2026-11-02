// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "history_dialog.hpp"

#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QTableView>
#include <QtWidgets/QVBoxLayout>

#include "app_context.hpp"
#include "nesva/jobs/job_store.hpp"

namespace nesva::ui {

HistoryDialog::HistoryDialog(AppContext& context, std::optional<qint64> jobId, QWidget* parent)
    : QDialog(parent), context_(context), jobId_(jobId) {
    setWindowTitle(jobId_ ? tr("Run history") : tr("All runs"));
    resize(900, 420);

    auto* layout = new QVBoxLayout(this);

    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({tr("Run"), tr("Job"), tr("Status"), tr("Started"),
                                       tr("Finished"), tr("Transferred"), tr("Unchanged"),
                                       tr("Failed"), tr("Bytes"), tr("Error")});

    view_ = new QTableView(this);
    view_->setModel(model_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(view_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    refresh();
}

void HistoryDialog::refresh() {
    model_->removeRows(0, model_->rowCount());

    jobs::JobStore store(context_.database());
    Result<QVector<jobs::RunSummary>> runs = store.listRuns(200, jobId_);
    if (!runs) {
        auto* item = new QStandardItem(QString::fromStdString(runs.error().toDisplayString()));
        model_->appendRow(item);
        return;
    }

    for (const jobs::RunSummary& run : runs.value()) {
        QList<QStandardItem*> row;
        row.append(new QStandardItem(QString::number(run.runNumber)));
        row.append(new QStandardItem(run.jobName));
        row.append(new QStandardItem(run.status));
        row.append(new QStandardItem(run.startedAt));
        row.append(new QStandardItem(run.finishedAt));
        row.append(new QStandardItem(QString::number(run.counters.filesTransferred)));
        row.append(new QStandardItem(QString::number(run.counters.filesSkipped)));
        row.append(new QStandardItem(QString::number(run.counters.filesFailed)));
        row.append(new QStandardItem(QString::number(run.counters.bytesTransferred)));
        row.append(new QStandardItem(run.errorMessage));
        model_->appendRow(row);
    }
}

} // namespace nesva::ui
