// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "job_list_model.hpp"

#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <utility>

namespace nesva::ui {

JobListModel::JobListModel(db::Database& database, QObject* parent)
    : QAbstractTableModel(parent), database_(database) {}

int JobListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int JobListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant JobListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const Row& row = rows_.at(index.row());

    if (role == Qt::TextAlignmentRole && index.column() == IdColumn) {
        return QStringLiteral("right");
    }

    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) {
        return {};
    }

    switch (index.column()) {
    case IdColumn:
        return QString::number(row.id);
    case NameColumn:
        return row.name;
    case ModeColumn:
        return row.mode;
    case EnabledColumn:
        return row.enabled ? QStringLiteral("yes") : QStringLiteral("no");
    case SourcesColumn:
        return QString::number(row.sourceCount);
    case DestinationColumn:
        return row.destination.isEmpty() ? row.provider : row.destination;
    case LastRunColumn:
        return row.lastStatus.isEmpty() ? QStringLiteral("never") : row.lastStatus;
    case FinishedColumn:
        return row.lastFinished;
    default:
        return {};
    }
}

QVariant JobListModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case IdColumn:
        return QStringLiteral("ID");
    case NameColumn:
        return QStringLiteral("Name");
    case ModeColumn:
        return QStringLiteral("Mode");
    case EnabledColumn:
        return QStringLiteral("Enabled");
    case SourcesColumn:
        return QStringLiteral("Sources");
    case DestinationColumn:
        return QStringLiteral("Destination");
    case LastRunColumn:
        return QStringLiteral("Last run");
    case FinishedColumn:
        return QStringLiteral("Finished");
    default:
        return {};
    }
}

QString JobListModel::refresh() {
    Result<QSqlQuery> query = database_.select(QStringLiteral(
        "SELECT j.id, j.name, j.mode, j.enabled, COALESCE(sp.name, ''), "
        "COALESCE(j.destination_path, ''), "
        "(SELECT COUNT(*) FROM backup_sources s WHERE s.job_id = j.id), "
        "COALESCE((SELECT r.status FROM job_runs r WHERE r.job_id = j.id ORDER BY r.id DESC LIMIT 1), ''), "
        "COALESCE((SELECT r.finished_at FROM job_runs r WHERE r.job_id = j.id ORDER BY r.id DESC LIMIT 1), '') "
        "FROM backup_jobs j LEFT JOIN storage_providers sp ON sp.id = j.storage_provider_id "
        "ORDER BY j.name"));
    if (!query) {
        return QString::fromStdString(query.error().toDisplayString());
    }

    QVector<Row> loaded;
    QSqlQuery rows = std::move(query).take();
    while (rows.next()) {
        Row row;
        row.id = rows.value(0).toLongLong();
        row.name = rows.value(1).toString();
        row.mode = rows.value(2).toString();
        row.enabled = rows.value(3).toBool();
        row.provider = rows.value(4).toString();
        row.destination = rows.value(5).toString();
        row.sourceCount = rows.value(6).toInt();
        row.lastStatus = rows.value(7).toString();
        row.lastFinished = rows.value(8).toString();
        loaded.append(row);
    }

    beginResetModel();
    rows_ = std::move(loaded);
    endResetModel();
    return {};
}

} // namespace nesva::ui
