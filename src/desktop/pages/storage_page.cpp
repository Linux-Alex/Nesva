// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "storage_page.hpp"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableView>

#include <utility>

#include "app_context.hpp"
#include "nesva/config/settings_store.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/jobs/job_store.hpp"
#include "nesva/providers/provider_factory.hpp"

namespace nesva::ui {

StoragePage::StoragePage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Storage"),
           QStringLiteral("Destinations for archive jobs. Local folders work immediately; Rclone "
                          "remotes use your existing rclone configuration (for example iCloud "
                          "Drive)."),
           parent),
      context_(context) {
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(6);
    auto* addLocalButton = new QPushButton(QStringLiteral("Add local folder..."), this);
    auto* addRcloneButton = new QPushButton(QStringLiteral("Add Rclone remote..."), this);
    testButton_ = new QPushButton(QStringLiteral("Test connection"), this);
    removeButton_ = new QPushButton(QStringLiteral("Remove"), this);
    toolbar->addWidget(addLocalButton);
    toolbar->addWidget(addRcloneButton);
    toolbar->addSpacing(12);
    toolbar->addWidget(testButton_);
    toolbar->addWidget(removeButton_);
    toolbar->addStretch(1);
    auto* refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    toolbar->addWidget(refreshButton);
    body()->addLayout(toolbar);

    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setStyleSheet(QStringLiteral("color: #c0392b;"));
    errorLabel_->setVisible(false);
    body()->addWidget(errorLabel_);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    statusLabel_->setWordWrap(true);
    body()->addWidget(statusLabel_);

    emptyState_ = new QLabel(this);
    emptyState_->setObjectName(QStringLiteral("emptyState"));
    emptyState_->setWordWrap(true);
    body()->addWidget(emptyState_, 1);

    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("Name"),
                                       QStringLiteral("Type"), QStringLiteral("Location"),
                                       QStringLiteral("Status"), QStringLiteral("Last checked")});

    view_ = new QTableView(this);
    view_->setModel(model_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    view_->setVisible(false);
    body()->addWidget(view_, 1);

    connect(refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    connect(addLocalButton, &QPushButton::clicked, this, &StoragePage::addLocalProvider);
    connect(addRcloneButton, &QPushButton::clicked, this, &StoragePage::addRcloneProvider);
    connect(testButton_, &QPushButton::clicked, this, &StoragePage::testSelected);
    connect(removeButton_, &QPushButton::clicked, this, &StoragePage::removeSelected);
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { updateActionState(); });

    updateActionState();
}

void StoragePage::refresh() {
    model_->removeRows(0, model_->rowCount());
    errorLabel_->setVisible(false);

    Result<QSqlQuery> query = context_.database().select(QStringLiteral(
        "SELECT id, name, provider_type, "
        "CASE WHEN COALESCE(rclone_remote, '') <> '' THEN rclone_remote || COALESCE(base_path, '') "
        "ELSE COALESCE(base_path, '') END, status, COALESCE(last_checked_at, '') "
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
        model_->appendRow(items);
    }

    const bool hasProviders = model_->rowCount() > 0;
    view_->setVisible(hasProviders);
    emptyState_->setVisible(!hasProviders);
    if (!hasProviders) {
        emptyState_->setText(QStringLiteral(
            "No storage providers yet. Add a local folder, or an Rclone remote that already "
            "exists in your rclone configuration."));
    }
    updateActionState();
}

qint64 StoragePage::selectedProviderId() const {
    const QModelIndexList rows = view_->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return 0;
    }
    return model_->item(rows.first().row(), 0)->text().toLongLong();
}

void StoragePage::updateActionState() {
    const bool hasSelection = selectedProviderId() > 0;
    testButton_->setEnabled(hasSelection);
    removeButton_->setEnabled(hasSelection);
}

void StoragePage::addLocalProvider() {
    const QString directory =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Select an archive folder"));
    if (directory.isEmpty()) {
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Storage provider"),
                                               QStringLiteral("Display name:"), QLineEdit::Normal,
                                               QFileInfo(directory).fileName(), &accepted);
    if (!accepted || name.trimmed().isEmpty()) {
        return;
    }

    jobs::JobStore store(context_.database());
    const Result<qint64> id = store.upsertLocalProvider(name.trimmed(), directory);
    if (!id) {
        QMessageBox::warning(this, QStringLiteral("Add storage provider"),
                             QString::fromStdString(id.error().toDisplayString()));
        return;
    }
    context_.recordEvent(LogLevel::Info, QStringLiteral("storage"),
                         QStringLiteral("Local storage provider added: %1").arg(name));
    refresh();
}

void StoragePage::addRcloneProvider() {
    bool accepted = false;
    const QString remote = QInputDialog::getText(
        this, QStringLiteral("Rclone remote"),
        QStringLiteral("Remote name as configured in rclone (for example icloud):"),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted || remote.trimmed().isEmpty()) {
        return;
    }
    const QString basePath = QInputDialog::getText(
        this, QStringLiteral("Rclone remote"),
        QStringLiteral("Base directory inside the remote (for example Nesva/Archive):"),
        QLineEdit::Normal, QStringLiteral("Nesva/Archive"), &accepted);
    if (!accepted) {
        return;
    }
    const QString displayName = QInputDialog::getText(
        this, QStringLiteral("Rclone remote"), QStringLiteral("Display name:"), QLineEdit::Normal,
        remote.trimmed(), &accepted);
    if (!accepted || displayName.trimmed().isEmpty()) {
        return;
    }

    QString remoteSpec = remote.trimmed();
    if (!remoteSpec.endsWith(QLatin1Char(':'))) {
        remoteSpec += QLatin1Char(':');
    }

    jobs::JobStore store(context_.database());
    const Result<qint64> id = store.upsertRcloneProvider(displayName.trimmed(), remoteSpec, basePath);
    if (!id) {
        QMessageBox::warning(this, QStringLiteral("Add storage provider"),
                             QString::fromStdString(id.error().toDisplayString()));
        return;
    }
    context_.recordEvent(LogLevel::Info, QStringLiteral("storage"),
                         QStringLiteral("Rclone storage provider added: %1 (%2)")
                             .arg(displayName, remoteSpec + basePath));
    statusLabel_->setText(
        QStringLiteral("Added %1. Use \"Test connection\" to verify that rclone can reach it.")
            .arg(displayName));
    refresh();
}

void StoragePage::testSelected() {
    const qint64 id = selectedProviderId();
    if (id <= 0) {
        return;
    }
    jobs::JobStore store(context_.database());
    Result<jobs::StorageProviderRecord> provider = store.getProvider(id);
    if (!provider) {
        QMessageBox::warning(this, QStringLiteral("Test connection"),
                             QString::fromStdString(provider.error().toDisplayString()));
        return;
    }

    providers::ProviderSelection selection;
    selection.displayName = provider.value().name;
    if (provider.value().isRcloneBacked()) {
        selection.rcloneRemote = provider.value().rcloneRemote;
        selection.basePath = provider.value().basePath;
    } else {
        selection.localRoot = provider.value().basePath;
    }

    const QString rcloneExecutable =
        context_.settings()
            .getString(QString::fromUtf8(config::keys::kRcloneBinaryPath), QStringLiteral("rclone"))
            .valueOr(QStringLiteral("rclone"));

    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    Result<std::unique_ptr<providers::StorageProvider>> created =
        providers::createProvider(selection, rcloneExecutable);
    Status status = created ? created.value()->testConnection() : created.error();
    QGuiApplication::restoreOverrideCursor();

    store.updateProviderStatus(id, status.ok() ? QStringLiteral("ok") : QStringLiteral("error"),
                               status.ok() ? QString() : QString::fromStdString(status.error().toDisplayString()));
    refresh();

    if (status.ok()) {
        statusLabel_->setText(QStringLiteral("%1 is reachable.").arg(provider.value().name));
    } else {
        statusLabel_->setText(QStringLiteral("%1 is not reachable: %2")
                                  .arg(provider.value().name,
                                       QString::fromStdString(status.error().toDisplayString())));
    }
}

void StoragePage::removeSelected() {
    const qint64 id = selectedProviderId();
    if (id <= 0) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Remove storage provider"),
        QStringLiteral("Remove this storage provider? Files already written to it are not "
                       "deleted."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    jobs::JobStore store(context_.database());
    const Status status = store.deleteProvider(id);
    if (!status.ok()) {
        QMessageBox::warning(this, QStringLiteral("Remove storage provider"),
                             QString::fromStdString(status.error().toDisplayString()));
        return;
    }
    refresh();
}

} // namespace nesva::ui
