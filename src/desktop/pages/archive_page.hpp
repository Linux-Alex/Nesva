// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QStringList>
#include <QtCore/QVector>

#include "page.hpp"
#include "nesva/jobs/job_store.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTableView;

namespace nesva::ui {

class AppContext;

/// Browses the objects an archive job has produced.
///
/// The provider is the source of truth for what exists; the manifest adds the
/// recorded hash and state, so the view never claims more than is known.
class ArchivePage : public Page {
    Q_OBJECT

public:
    explicit ArchivePage(AppContext& context, QWidget* parent = nullptr);

    void refresh() override;

private:
    void reloadJobs();
    void loadArchive();

    AppContext& context_;
    QComboBox* jobCombo_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QTableView* view_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QLabel* statusLabel_ = nullptr;
};

} // namespace nesva::ui
