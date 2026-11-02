// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include "page.hpp"

class QLabel;
class QStandardItemModel;
class QTableView;

namespace nesva::ui {

class AppContext;

/// Lists configured storage providers and their last known connection status.
class StoragePage : public Page {
    Q_OBJECT

public:
    explicit StoragePage(AppContext& context, QWidget* parent = nullptr);

    void refresh() override;

private:
    AppContext& context_;
    QStandardItemModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QLabel* errorLabel_ = nullptr;
};

} // namespace nesva::ui
