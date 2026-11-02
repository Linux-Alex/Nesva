// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QFutureWatcher>
#include <QtCore/QVector>

#include "page.hpp"

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QStandardItemModel;
class QTableView;

namespace nesva::jobs {
struct ArchiveJob;
struct SnapshotInfo;
} // namespace nesva::jobs

namespace nesva::backup {
struct SnapshotInfo;
}

namespace nesva::ui {

class AppContext;

/// Restic snapshot history for backup jobs, with restore, retention and
/// repository maintenance actions.
class SnapshotsPage : public Page {
    Q_OBJECT

public:
    explicit SnapshotsPage(AppContext& context, QWidget* parent = nullptr);

    void refresh() override;

private:
    struct VerificationOutcome {
        bool ok = false;
        QString summary;
    };
    struct RetentionOutcome {
        bool ok = false;
        bool applied = false;
        int kept = 0;
        int removed = 0;
        QStringList removedIds;
        QString summary;
    };

    void reloadJobs();
    void loadSnapshots();
    void refreshFromRepository();
    void browseSelected();
    void restoreSelected();
    void previewRetention();
    void applyRetention();
    void runCheck();
    void unlockRepository();
    void setBusy(bool busy, const QString& message);
    void updateActionState();
    qint64 selectedJobId() const;
    QString selectedSnapshotId() const;
    std::optional<jobs::ArchiveJob> selectedJob() const;

    AppContext& context_;
    QComboBox* jobCombo_ = nullptr;
    QTableView* view_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QProgressBar* busyBar_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    QPushButton* retentionPreviewButton_ = nullptr;
    QPushButton* retentionApplyButton_ = nullptr;
    QPushButton* checkButton_ = nullptr;
    QPushButton* unlockButton_ = nullptr;
    QVector<QMetaObject::Connection> watcherConnections_;
    bool busy_ = false;
};

} // namespace nesva::ui
