// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QDialog>

#include <memory>

#include "nesva/core/cancellation.hpp"
#include "nesva/jobs/job_store.hpp"

class QLabel;
class QLineEdit;
class QProgressBar;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTableView;
class QFutureWatcherBase;

namespace nesva::ui {

class AppContext;

/// Browses the files stored inside one Restic snapshot.
///
/// The list is read from the repository (restic ls --json), never guessed from
/// the local filesystem, and it is loaded off the GUI thread.
class SnapshotBrowserDialog : public QDialog {
    Q_OBJECT

public:
    SnapshotBrowserDialog(AppContext& context, jobs::ArchiveJob job, QString snapshotId,
                          QWidget* parent = nullptr);
    ~SnapshotBrowserDialog() override;

    int fileCount() const { return fileCount_; }
    QString loadError() const { return loadError_; }

signals:
    void loaded(int fileCount);

private:
    void load();
    void finishLoad();

    AppContext& context_;
    jobs::ArchiveJob job_;
    QString snapshotId_;
    QTableView* view_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QProgressBar* busyBar_ = nullptr;
    CancellationToken cancellation_;
    std::unique_ptr<QFutureWatcherBase> watcher_;
    int fileCount_ = 0;
    QString loadError_;
};

} // namespace nesva::ui
