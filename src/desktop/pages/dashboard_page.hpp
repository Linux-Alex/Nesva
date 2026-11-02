// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QStringList>
#include <QtCore/QVector>

#include "page.hpp"

class QLabel;
class QPushButton;
class QTableView;

namespace nesva::ui {

class AppContext;
class EventListModel;

/// Operational overview: job counters, last run outcomes, storage state,
/// environment warnings and the most recent activity.
class DashboardPage : public Page {
    Q_OBJECT

public:
    explicit DashboardPage(AppContext& context, QWidget* parent = nullptr);

    void refresh() override;

private:
    struct StatCard {
        QLabel* value = nullptr;
        QLabel* caption = nullptr;
    };

    void buildUi();
    QStringList collectAttention() const;
    void probeTools(bool force);

    AppContext& context_;
    QVector<StatCard> cards_;
    QLabel* attentionLabel_ = nullptr;
    QPushButton* recheckButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    EventListModel* recentModel_ = nullptr;
    QTableView* recentView_ = nullptr;

    bool toolsProbed_ = false;
    QStringList toolWarnings_;
};

} // namespace nesva::ui
