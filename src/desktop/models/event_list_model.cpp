// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "event_list_model.hpp"

#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <utility>

namespace nesva::ui {

EventListModel::EventListModel(db::Database& database, int rowLimit, QObject* parent)
    : QAbstractTableModel(parent), database_(database), rowLimit_(rowLimit) {}

int EventListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int EventListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant EventListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const Row& row = rows_.at(index.row());

    if (role == Qt::ForegroundRole && index.column() == LevelColumn) {
        if (row.level == QStringLiteral("error") || row.level == QStringLiteral("critical")) {
            return QBrush(QColor(0xc0, 0x39, 0x2b));
        }
        if (row.level == QStringLiteral("warn")) {
            return QBrush(QColor(0xb5, 0x7a, 0x00));
        }
    }

    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) {
        return {};
    }

    switch (index.column()) {
    case TimeColumn:
        return row.timestamp;
    case LevelColumn:
        return row.level;
    case SourceColumn:
        return row.source;
    case CategoryColumn:
        return row.category;
    case MessageColumn:
        return row.message;
    default:
        return {};
    }
}

QVariant EventListModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case TimeColumn:
        return QStringLiteral("Time (UTC)");
    case LevelColumn:
        return QStringLiteral("Level");
    case SourceColumn:
        return QStringLiteral("Source");
    case CategoryColumn:
        return QStringLiteral("Category");
    case MessageColumn:
        return QStringLiteral("Message");
    default:
        return {};
    }
}

QString EventListModel::refresh() {
    Result<QSqlQuery> query = database_.select(QStringLiteral(
        "SELECT ts, level, source, category, message FROM ("
        "SELECT e.occurred_at AS ts, e.level AS level, COALESCE(j.name, 'application') AS source, "
        "e.category AS category, e.message AS message "
        "FROM job_events e LEFT JOIN backup_jobs j ON j.id = e.job_id "
        "UNION ALL "
        "SELECT created_at, level, 'application', category, message FROM application_events"
        ") ORDER BY ts DESC LIMIT ?"),
        {QVariant(rowLimit_)});
    if (!query) {
        return QString::fromStdString(query.error().toDisplayString());
    }

    QVector<Row> loaded;
    QSqlQuery rows = std::move(query).take();
    while (rows.next()) {
        Row row;
        row.timestamp = rows.value(0).toString();
        row.level = rows.value(1).toString();
        row.source = rows.value(2).toString();
        row.category = rows.value(3).toString();
        row.message = rows.value(4).toString();
        loaded.append(row);
    }

    beginResetModel();
    rows_ = std::move(loaded);
    endResetModel();
    return {};
}

} // namespace nesva::ui
