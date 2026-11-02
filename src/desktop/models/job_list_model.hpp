// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QAbstractTableModel>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "nesva/db/database.hpp"

namespace nesva::ui {

/// Read-only model over the "backup_jobs" table, reloaded on demand.
class JobListModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        IdColumn = 0,
        NameColumn,
        ModeColumn,
        EnabledColumn,
        SourcesColumn,
        DestinationColumn,
        LastRunColumn,
        NextRunColumn,
        FinishedColumn,
        ColumnCount,
    };

    explicit JobListModel(db::Database& database, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    /// Reloads the model from the database. Returns an error description on failure.
    QString refresh();
    int jobCount() const { return static_cast<int>(rows_.size()); }

private:
    struct Row {
        qint64 id = 0;
        QString name;
        QString mode;
        bool enabled = false;
        int sourceCount = 0;
        QString provider;
        QString destination;
        QString lastStatus;
        QString lastFinished;
        QString nextRun;
    };

    db::Database& database_;
    QVector<Row> rows_;
};

} // namespace nesva::ui
