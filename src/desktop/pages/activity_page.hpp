// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include "page.hpp"

class QLabel;
class QLineEdit;
class QSortFilterProxyModel;
class QTableView;

namespace nesva::ui {

class AppContext;
class EventListModel;

/// Combined job and application event log with text filtering.
class ActivityPage : public Page {
    Q_OBJECT

public:
    explicit ActivityPage(AppContext& context, QWidget* parent = nullptr);

    void refresh() override;

private:
    AppContext& context_;
    EventListModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* view_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
};

} // namespace nesva::ui
