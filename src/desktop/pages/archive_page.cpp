// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "archive_page.hpp"

#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableView>

#include <utility>

#include "app_context.hpp"
#include "nesva/config/settings_store.hpp"
#include "nesva/jobs/archive_restore.hpp"

namespace nesva::ui {

ArchivePage::ArchivePage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Archive Explorer"),
           QStringLiteral("Files a job has archived. The destination is the source of truth; "
                          "hashes come from the run manifest."),
           parent),
      context_(context) {
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);

    jobCombo_ = new QComboBox(this);
    jobCombo_->setMinimumWidth(220);
    toolbar->addWidget(jobCombo_);

    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(QStringLiteral("Filter by path"));
    filterEdit_->setClearButtonEnabled(true);
    toolbar->addWidget(filterEdit_, 1);

    auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    toolbar->addWidget(refreshButton);
    body()->addLayout(toolbar);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    statusLabel_->setWordWrap(true);
    body()->addWidget(statusLabel_);

    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({QStringLiteral("Path"), QStringLiteral("Bytes"),
                                       QStringLiteral("SHA-256"), QStringLiteral("State"),
                                       QStringLiteral("Modified")});

    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(0);

    view_ = new QTableView(this);
    view_->setModel(proxy_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->setSortingEnabled(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    body()->addWidget(view_, 1);

    connect(refreshButton, &QPushButton::clicked, this, [this] { loadArchive(); });
    connect(filterEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) { proxy_->setFilterFixedString(text); });
    connect(jobCombo_, &QComboBox::currentIndexChanged, this, [this](int) { loadArchive(); });
}

void ArchivePage::refresh() {
    reloadJobs();
    loadArchive();
}

void ArchivePage::reloadJobs() {
    const qint64 selected = jobCombo_->currentData().toLongLong();
    jobCombo_->blockSignals(true);
    jobCombo_->clear();

    jobs::JobStore store(context_.database());
    Result<QVector<jobs::ArchiveJob>> jobs = store.listJobs();
    if (!jobs) {
        jobCombo_->blockSignals(false);
        statusLabel_->setText(QString::fromStdString(jobs.error().toDisplayString()));
        return;
    }
    for (const jobs::ArchiveJob& job : jobs.value()) {
        if (job.mode != QStringLiteral("archive")) {
            continue;
        }
        jobCombo_->addItem(job.name, QVariant(job.id));
    }
    const int index = jobCombo_->findData(QVariant(selected));
    if (index >= 0) {
        jobCombo_->setCurrentIndex(index);
    }
    jobCombo_->blockSignals(false);
}

void ArchivePage::loadArchive() {
    model_->removeRows(0, model_->rowCount());

    if (!jobCombo_->currentData().isValid()) {
        statusLabel_->setText(QStringLiteral(
            "No archive jobs yet. Create one on the Backup Jobs page; its contents appear here "
            "after the first run."));
        return;
    }

    jobs::JobStore store(context_.database());
    const qint64 jobId = jobCombo_->currentData().toLongLong();
    Result<jobs::ArchiveJob> job = store.getJob(jobId);
    if (!job) {
        statusLabel_->setText(QString::fromStdString(job.error().toDisplayString()));
        return;
    }
    std::optional<jobs::StorageProviderRecord> provider;
    if (job.value().storageProviderId) {
        Result<jobs::StorageProviderRecord> loaded = store.getProvider(*job.value().storageProviderId);
        if (loaded) {
            provider = loaded.value();
        }
    }

    const QString rcloneExecutable =
        context_.settings()
            .getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));

    jobs::ArchiveRestorer restorer(context_.database());
    Result<jobs::RestorePlan> plan = restorer.plan(job.value(), provider, QString(), {}, rcloneExecutable,
                                                   /*checkDestination=*/false);
    if (!plan) {
        statusLabel_->setText(QString::fromStdString(plan.error().toDisplayString()));
        return;
    }

    qint64 totalBytes = 0;
    for (const jobs::ArchiveItem& item : plan.value().items) {
        totalBytes += item.sizeBytes;
        QList<QStandardItem*> row;
        row.append(new QStandardItem(item.relativePath));
        row.append(new QStandardItem(QString::number(item.sizeBytes)));
        row.append(new QStandardItem(item.sha256.isEmpty() ? QStringLiteral("-")
                                                           : item.sha256.left(16)));
        row.append(new QStandardItem(item.state.isEmpty() ? QStringLiteral("unknown") : item.state));
        row.append(new QStandardItem(item.modifiedUtc.toUTC().toString(Qt::ISODate)));
        model_->appendRow(row);
    }

    statusLabel_->setText(QStringLiteral("%1 object(s), %2 byte(s) in %3")
                              .arg(plan.value().items.size())
                              .arg(totalBytes)
                              .arg(job.value().name));
}

} // namespace nesva::ui
