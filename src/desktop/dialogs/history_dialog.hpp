// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QDialog>

#include <optional>

class QStandardItemModel;
class QTableView;

namespace nesva::ui {

class AppContext;

/// Run history for one job (or for every job), with the counters and the error
/// that ended each run.
class HistoryDialog : public QDialog {
    Q_OBJECT

public:
    HistoryDialog(AppContext& context, std::optional<qint64> jobId, QWidget* parent = nullptr);

    void refresh();

private:
    AppContext& context_;
    std::optional<qint64> jobId_;
    QStandardItemModel* model_ = nullptr;
    QTableView* view_ = nullptr;
};

} // namespace nesva::ui
