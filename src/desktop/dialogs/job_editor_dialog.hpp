// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QDialog>

#include <optional>

#include "nesva/jobs/job_store.hpp"
#include "nesva/scheduler/schedule.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QGroupBox;
class QSpinBox;
class QTimeEdit;

namespace nesva::ui {

class AppContext;

/// Creates or edits an archive job.
///
/// Only archive mode is offered: Restic backup jobs arrive in Phase 4, and the
/// dialog says so instead of pretending the option works.
class JobEditorDialog : public QDialog {
    Q_OBJECT

public:
    JobEditorDialog(AppContext& context, std::optional<jobs::ArchiveJob> existing,
                    QWidget* parent = nullptr);

    qint64 savedJobId() const { return savedJobId_; }

    /// Validates the form and stores the job; refuses with a message box when
    /// the form is incomplete.
    void accept() override;

private slots:
    void addSource();
    void removeSource();
    void browseDestination();
    void destinationKindChanged();
    void previewRun();

private:
    jobs::ArchiveJob buildJob() const;
    sched::ScheduleSpec buildSchedule() const;
    void updateModeUi();
    void updateSourceUi();
    void setRepositoryPassword();
    void initialiseRepository();
    void updateScheduleSummary();
    std::optional<jobs::StorageProviderRecord> selectedProvider() const;
    bool validate(QString* error) const;

    AppContext& context_;
    std::optional<jobs::ArchiveJob> existing_;
    qint64 savedJobId_ = 0;

    QLineEdit* nameEdit_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QComboBox* sourceKind_ = nullptr;
    QComboBox* mailAccount_ = nullptr;
    QWidget* sourceRow_ = nullptr;
    QWidget* mailAccountRow_ = nullptr;
    QListWidget* sourceList_ = nullptr;
    QComboBox* destinationKind_ = nullptr;
    QLineEdit* destinationPathEdit_ = nullptr;
    QLineEdit* subPathEdit_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPlainTextEdit* exclusionsEdit_ = nullptr;
    QSpinBox* bandwidthSpin_ = nullptr;
    QCheckBox* verifyCheck_ = nullptr;
    QWidget* archiveDestinationRow_ = nullptr;
    QGroupBox* backupGroup_ = nullptr;
    QLineEdit* repositoryEdit_ = nullptr;
    QSpinBox* keepLastSpin_ = nullptr;
    QSpinBox* keepDailySpin_ = nullptr;
    QSpinBox* keepWeeklySpin_ = nullptr;
    QSpinBox* keepMonthlySpin_ = nullptr;
    QSpinBox* keepYearlySpin_ = nullptr;
    QCheckBox* checkAfterBackupCheck_ = nullptr;
    QCheckBox* enabledCheck_ = nullptr;
    QComboBox* scheduleKind_ = nullptr;
    QTimeEdit* scheduleTime_ = nullptr;
    QComboBox* scheduleWeekday_ = nullptr;
    QSpinBox* scheduleMonthDay_ = nullptr;
    QComboBox* scheduleTimezone_ = nullptr;
    QCheckBox* scheduleCatchUp_ = nullptr;
    QSpinBox* scheduleRetries_ = nullptr;
    QSpinBox* scheduleBackoff_ = nullptr;
    QSpinBox* scheduleMaxDuration_ = nullptr;
    QLabel* scheduleSummary_ = nullptr;
    QPushButton* previewButton_ = nullptr;
    QProgressBar* previewProgress_ = nullptr;
    QLabel* previewLabel_ = nullptr;
};

} // namespace nesva::ui
