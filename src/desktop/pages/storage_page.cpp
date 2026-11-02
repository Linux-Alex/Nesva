// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "storage_page.hpp"

#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableView>

#include <utility>

#include "app_context.hpp"

namespace nesva::ui {

StoragePage::StoragePage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Storage"),
           QStringLiteral("Remote and local destinations used by archive and backup jobs."), parent),
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
    body()->addWidget(emptyState_, 1);

    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("Name"),
                                       QStringLiteral("Type"), QStringLiteral("Rclone remote"),
                                       QStringLiteral("Base path"), QStringLiteral("Status"),
                                       QStringLiteral("Last checked")});

    view_ = new QTableView(this);
    view_->setModel(model_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->setVisible(false);
    body()->addWidget(view_, 1);

    connect(refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
}

void StoragePage::refresh() {
    model_->removeRows(0, model_->rowCount());
    errorLabel_->setVisible(false);

    Result<QSqlQuery> query = context_.database().select(QStringLiteral(
        "SELECT id, name, provider_type, COALESCE(rclone_remote, ''), "
        "COALESCE(base_path, ''), status, COALESCE(last_checked_at, '') "
        "FROM storage_providers ORDER BY name"));
    if (!query) {
        errorLabel_->setText(QString::fromStdString(query.error().toDisplayString()));
        errorLabel_->setVisible(true);
        return;
    }

    QSqlQuery rows = std::move(query).take();
    while (rows.next()) {
        QList<QStandardItem*> items;
        items.append(new QStandardItem(QString::number(rows.value(0).toLongLong())));
        items.append(new QStandardItem(rows.value(1).toString()));
        items.append(new QStandardItem(rows.value(2).toString()));
        items.append(new QStandardItem(rows.value(3).toString()));
        items.append(new QStandardItem(rows.value(4).toString()));
        items.append(new QStandardItem(rows.value(5).toString()));
        items.append(new QStandardItem(rows.value(6).toString()));
        model_->appendRow(items);
    }

    const bool hasProviders = model_->rowCount() > 0;
    view_->setVisible(hasProviders);
    emptyState_->setVisible(!hasProviders);
    if (!hasProviders) {
        emptyState_->setText(QStringLiteral(
            "No storage providers are configured yet.\n\n"
            "Connection setup (local filesystem, SFTP, S3-compatible, WebDAV and Rclone remotes "
            "including iCloud Drive) is delivered in Phase 2. Provider rows already live in the "
            "Nesva database, so anything added later appears here."));
    }
}

} // namespace nesva::ui
