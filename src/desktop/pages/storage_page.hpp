// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include "page.hpp"

class QLabel;
class QPushButton;
class QStandardItemModel;
class QTableView;

namespace nesva::ui {

class AppContext;

/// Storage connection manager: local folders and Rclone remotes, with an
/// explicit connection test.
class StoragePage : public Page {
    Q_OBJECT

public:
    explicit StoragePage(AppContext& context, QWidget* parent = nullptr);

    void refresh() override;

private:
    void addLocalProvider();
    void addRcloneProvider();
    void testSelected();
    void removeSelected();
    void updateActionState();
    qint64 selectedProviderId() const;

    AppContext& context_;
    QStandardItemModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* testButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
};

} // namespace nesva::ui
