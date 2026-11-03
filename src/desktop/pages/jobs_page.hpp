// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include "page.hpp"

class QLabel;
class QProgressBar;
class QPushButton;
class QTableView;

namespace nesva::ui {

class AppContext;
class ElidedLabel;
class JobListModel;

/// Lists archive jobs and drives their execution.
class JobsPage : public Page {
    Q_OBJECT

public:
    explicit JobsPage(AppContext& context, QWidget* parent = nullptr);

    void refresh() override;

private:
    void createJob();
    void editJob();
    void deleteJob();
    void runJob();
    void cancelJob();
    void showHistory();
    void updateActionState();
    void setRunBusy(bool busy);
    qint64 selectedJobId() const;

    AppContext& context_;
    JobListModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    QLabel* emptyState_ = nullptr;
    ElidedLabel* errorLabel_ = nullptr;
    ElidedLabel* runLabel_ = nullptr;
    QProgressBar* runProgress_ = nullptr;
    QPushButton* newButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* runButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* historyButton_ = nullptr;
};

} // namespace nesva::ui
