// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include "page.hpp"

class QLabel;
class QTableView;

namespace nesva::ui {

class AppContext;
class JobListModel;

/// Lists archive and backup jobs stored in the local database.
class JobsPage : public Page {
    Q_OBJECT

public:
    explicit JobsPage(AppContext& context, QWidget* parent = nullptr);

    void refresh() override;

private:
    AppContext& context_;
    JobListModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QLabel* errorLabel_ = nullptr;
};

} // namespace nesva::ui
