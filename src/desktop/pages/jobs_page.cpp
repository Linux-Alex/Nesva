// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "jobs_page.hpp"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableView>

#include "app_context.hpp"
#include "models/job_list_model.hpp"

namespace nesva::ui {

JobsPage::JobsPage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Backup Jobs"),
           QStringLiteral("Archive and backup jobs. Archive jobs copy files; backup jobs create "
                          "versioned snapshots with Restic."),
           parent),
      context_(context) {
    auto* toolbar = new QHBoxLayout();
    toolbar->addStretch(1);
    auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    toolbar->addWidget(refreshButton);
    body()->addLayout(toolbar);

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
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->horizontalHeader()->setSectionResizeMode(JobListModel::NameColumn,
                                                     QHeaderView::Stretch);
    view_->horizontalHeader()->setSectionResizeMode(JobListModel::DestinationColumn,
                                                     QHeaderView::Stretch);
    view_->setVisible(false);
    body()->addWidget(view_, 1);

    connect(refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
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
            "No jobs are configured yet.\n\n"
            "Job creation is delivered in Phase 2 (functional archive). This page already reads "
            "real job rows from the Nesva database, so jobs added by later phases or by the "
            "command line appear here without changes."));
    }

    setSubtitle(QStringLiteral("Archive and backup jobs. Archive jobs copy files; backup jobs "
                               "create versioned snapshots with Restic. Database: %1")
                    .arg(context_.database().filePath()));
}

} // namespace nesva::ui
