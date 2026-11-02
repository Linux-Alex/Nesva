// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QVector>

#include <memory>
#include <optional>

#include "page.hpp"
#include "nesva/core/cancellation.hpp"
#include "nesva/jobs/archive_restore.hpp"
#include "nesva/jobs/job_store.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStandardItemModel;
class QTableView;
class QFutureWatcherBase;

namespace nesva::ui {

class AppContext;

/// Restore wizard: choose an archive job, inspect its objects, pick a
/// destination, resolve conflicts explicitly and restore with verification.
///
/// Existing files are never overwritten silently: the conflict policy is a
/// required choice with a default of "skip".
class RestorePage : public Page {
    Q_OBJECT

public:
    explicit RestorePage(AppContext& context, QWidget* parent = nullptr);
    ~RestorePage() override;

    void refresh() override;

private:
    void reloadJobs();
    void loadPlan();
    void browseDestination();
    void startRestore();
    void finishRestore();
    void updateActionState();

    AppContext& context_;
    QComboBox* jobCombo_ = nullptr;
    QTableView* view_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QLineEdit* destinationEdit_ = nullptr;
    QComboBox* policyCombo_ = nullptr;
    QPushButton* restoreSelectedButton_ = nullptr;
    QPushButton* restoreAllButton_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* conflictLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    std::optional<jobs::ArchiveJob> job_;
    std::optional<jobs::StorageProviderRecord> provider_;
    QVector<jobs::ArchiveItem> items_;
    CancellationToken cancellation_;
    std::unique_ptr<QFutureWatcherBase> watcher_;
    bool restoring_ = false;
};

} // namespace nesva::ui
