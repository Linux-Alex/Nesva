// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QDialog>

#include <memory>

#include "nesva/core/cancellation.hpp"
#include "nesva/jobs/archive_restore.hpp"
#include "nesva/jobs/job_store.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QFutureWatcherBase;

namespace nesva::ui {

class AppContext;

/// Restores one Restic snapshot into a chosen directory.
///
/// The destination and conflict policy are explicit; existing files are never
/// overwritten without choosing "overwrite" first.
class SnapshotRestoreDialog : public QDialog {
    Q_OBJECT

public:
    SnapshotRestoreDialog(AppContext& context, jobs::ArchiveJob job, QString snapshotId,
                          QWidget* parent = nullptr);
    ~SnapshotRestoreDialog() override;

private:
    void browseDestination();
    void startRestore();
    void finishRestore();
    void updateActionState();

    AppContext& context_;
    jobs::ArchiveJob job_;
    QString snapshotId_;
    QLineEdit* destinationEdit_ = nullptr;
    QComboBox* policyCombo_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    CancellationToken cancellation_;
    std::unique_ptr<QFutureWatcherBase> watcher_;
    bool restoring_ = false;
};

} // namespace nesva::ui
