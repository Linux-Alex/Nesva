// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QAbstractTableModel>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "nesva/db/database.hpp"

namespace nesva::ui {

/// Read-only model over the combined event stream: job events plus
/// application-level events, newest first.
class EventListModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        TimeColumn = 0,
        LevelColumn,
        SourceColumn,
        CategoryColumn,
        MessageColumn,
        ColumnCount,
    };

    EventListModel(db::Database& database, int rowLimit, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    /// Reloads the model. Returns an error description on failure.
    QString refresh();

private:
    struct Row {
        QString timestamp;
        QString level;
        QString source;
        QString category;
        QString message;
    };

    db::Database& database_;
    int rowLimit_ = 100;
    QVector<Row> rows_;
};

} // namespace nesva::ui
