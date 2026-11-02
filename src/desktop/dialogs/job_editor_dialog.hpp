// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtWidgets/QDialog>

#include <optional>

#include "nesva/jobs/job_store.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

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
    std::optional<jobs::StorageProviderRecord> selectedProvider() const;
    bool validate(QString* error) const;

    AppContext& context_;
    std::optional<jobs::ArchiveJob> existing_;
    qint64 savedJobId_ = 0;

    QLineEdit* nameEdit_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QListWidget* sourceList_ = nullptr;
    QComboBox* destinationKind_ = nullptr;
    QLineEdit* destinationPathEdit_ = nullptr;
    QLineEdit* subPathEdit_ = nullptr;
    QPushButton* browseButton_ = nullptr;
    QPlainTextEdit* exclusionsEdit_ = nullptr;
    QSpinBox* bandwidthSpin_ = nullptr;
    QCheckBox* verifyCheck_ = nullptr;
    QCheckBox* enabledCheck_ = nullptr;
    QPushButton* previewButton_ = nullptr;
    QProgressBar* previewProgress_ = nullptr;
    QLabel* previewLabel_ = nullptr;
};

} // namespace nesva::ui
