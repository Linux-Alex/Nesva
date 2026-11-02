// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "job_editor_dialog.hpp"

#include <QtCore/QDir>
#include <QtCore/QLocale>
#include <QtCore/QTimeZone>
#include <QtCore/QFileInfo>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QTimeEdit>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <utility>

#include "app_context.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/jobs/job_store.hpp"
#include "nesva/scan/file_scanner.hpp"
#include "nesva/scheduler/schedule.hpp"

namespace nesva::ui {
namespace {

} // namespace

JobEditorDialog::JobEditorDialog(AppContext& context, std::optional<jobs::ArchiveJob> existing,
                                 QWidget* parent)
    : QDialog(parent), context_(context), existing_(std::move(existing)) {
    setWindowTitle(existing_ ? tr("Edit job") : tr("New archive job"));
    setMinimumWidth(620);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setSpacing(10);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    nameEdit_ = new QLineEdit(this);
    nameEdit_->setObjectName(QStringLiteral("jobName"));
    nameEdit_->setPlaceholderText(tr("Documents archive"));
    form->addRow(tr("Name"), nameEdit_);

    modeCombo_ = new QComboBox(this);
    modeCombo_->setObjectName(QStringLiteral("jobMode"));
    modeCombo_->addItem(tr("Archive - copy files, no versions"), QStringLiteral("archive"));
    modeCombo_->addItem(tr("Backup - versioned Restic snapshots (Phase 4)"), QStringLiteral("backup"));
    modeCombo_->setItemData(1, QVariant(), Qt::ToolTipRole);
    // The backup item is visible but not selectable: it is not implemented yet.
    if (auto* model = modeCombo_->model()) {
        model->setData(model->index(1, 0), QVariant(0), Qt::UserRole - 1);
    }
    form->addRow(tr("Mode"), modeCombo_);

    auto* sourceRow = new QWidget(this);
    auto* sourceLayout = new QHBoxLayout(sourceRow);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceList_ = new QListWidget(sourceRow);
    sourceList_->setObjectName(QStringLiteral("jobSources"));
    sourceList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    sourceList_->setMinimumHeight(90);
    sourceLayout->addWidget(sourceList_, 1);
    auto* sourceButtons = new QVBoxLayout();
    auto* addButton = new QPushButton(tr("Add folder..."), sourceRow);
    auto* removeButton = new QPushButton(tr("Remove"), sourceRow);
    sourceButtons->addWidget(addButton);
    sourceButtons->addWidget(removeButton);
    sourceButtons->addStretch(1);
    sourceLayout->addLayout(sourceButtons);
    form->addRow(tr("Sources"), sourceRow);

    auto* destinationRow = new QWidget(this);
    auto* destinationLayout = new QVBoxLayout(destinationRow);
    destinationLayout->setContentsMargins(0, 0, 0, 0);
    destinationLayout->setSpacing(6);

    destinationKind_ = new QComboBox(destinationRow);
    destinationKind_->setObjectName(QStringLiteral("jobDestinationKind"));
    destinationLayout->addWidget(destinationKind_);

    auto* localRow = new QHBoxLayout();
    destinationPathEdit_ = new QLineEdit(destinationRow);
    destinationPathEdit_->setObjectName(QStringLiteral("jobDestinationPath"));
    destinationPathEdit_->setPlaceholderText(tr("/mnt/archive/Nesva"));
    browseButton_ = new QPushButton(tr("Browse..."), destinationRow);
    localRow->addWidget(destinationPathEdit_, 1);
    localRow->addWidget(browseButton_);
    destinationLayout->addLayout(localRow);

    subPathEdit_ = new QLineEdit(destinationRow);
    subPathEdit_->setObjectName(QStringLiteral("jobSubPath"));
    subPathEdit_->setPlaceholderText(tr("Optional sub-directory inside the provider"));
    destinationLayout->addWidget(subPathEdit_);
    form->addRow(tr("Destination"), destinationRow);

    exclusionsEdit_ = new QPlainTextEdit(this);
    exclusionsEdit_->setObjectName(QStringLiteral("jobExclusions"));
    exclusionsEdit_->setPlaceholderText(tr("One pattern per line, for example *.tmp or build/**"));
    exclusionsEdit_->setMaximumHeight(90);
    form->addRow(tr("Exclusions"), exclusionsEdit_);

    bandwidthSpin_ = new QSpinBox(this);
    bandwidthSpin_->setObjectName(QStringLiteral("jobBandwidth"));
    bandwidthSpin_->setRange(0, 10'000'000);
    bandwidthSpin_->setSingleStep(256);
    bandwidthSpin_->setSuffix(tr(" KiB/s"));
    bandwidthSpin_->setSpecialValueText(tr("Unlimited"));
    form->addRow(tr("Bandwidth limit"), bandwidthSpin_);

    verifyCheck_ = new QCheckBox(tr("Verify checksums after transfer"), this);
    verifyCheck_->setObjectName(QStringLiteral("jobVerify"));
    verifyCheck_->setChecked(true);
    form->addRow(QString(), verifyCheck_);

    enabledCheck_ = new QCheckBox(tr("Job is enabled"), this);
    enabledCheck_->setObjectName(QStringLiteral("jobEnabled"));
    enabledCheck_->setChecked(true);
    form->addRow(QString(), enabledCheck_);

    rootLayout->addLayout(form);


    // ---- schedule ---------------------------------------------------------
    auto* scheduleGroup = new QGroupBox(tr("Schedule"), this);
    auto* scheduleForm = new QFormLayout(scheduleGroup);

    scheduleKind_ = new QComboBox(scheduleGroup);
    scheduleKind_->addItem(tr("Manual only"), QStringLiteral("manual"));
    scheduleKind_->addItem(tr("Daily"), QStringLiteral("daily"));
    scheduleKind_->addItem(tr("Weekly"), QStringLiteral("weekly"));
    scheduleKind_->addItem(tr("Monthly"), QStringLiteral("monthly"));
    scheduleKind_->addItem(tr("When the user logs in"), QStringLiteral("on_login"));
    scheduleKind_->addItem(tr("When the agent starts"), QStringLiteral("on_startup"));
    scheduleForm->addRow(tr("Repeat"), scheduleKind_);

    scheduleTime_ = new QTimeEdit(scheduleGroup);
    scheduleTime_->setDisplayFormat(QStringLiteral("HH:mm"));
    scheduleForm->addRow(tr("Time of day"), scheduleTime_);

    scheduleWeekday_ = new QComboBox(scheduleGroup);
    for (int day = 1; day <= 7; ++day) {
        scheduleWeekday_->addItem(QLocale().dayName(day, QLocale::LongFormat), day);
    }
    scheduleForm->addRow(tr("Day of week"), scheduleWeekday_);

    scheduleMonthDay_ = new QSpinBox(scheduleGroup);
    scheduleMonthDay_->setRange(1, 28);
    scheduleForm->addRow(tr("Day of month"), scheduleMonthDay_);

    scheduleTimezone_ = new QComboBox(scheduleGroup);
    scheduleTimezone_->setEditable(true);
    {
        const QByteArray systemId = QTimeZone::systemTimeZoneId();
        scheduleTimezone_->addItem(QString::fromLatin1(systemId));
        scheduleTimezone_->addItem(QStringLiteral("UTC"));
        for (const QByteArray& id : QTimeZone::availableTimeZoneIds()) {
            const QString name = QString::fromLatin1(id);
            if (name != QString::fromLatin1(systemId) && name != QStringLiteral("UTC")) {
                scheduleTimezone_->addItem(name);
            }
        }
        scheduleTimezone_->setCurrentIndex(0);
    }
    scheduleForm->addRow(tr("Timezone"), scheduleTimezone_);

    scheduleCatchUp_ = new QCheckBox(tr("Run a missed slot when the agent starts"), scheduleGroup);
    scheduleCatchUp_->setChecked(true);
    scheduleForm->addRow(QString(), scheduleCatchUp_);

    scheduleRetries_ = new QSpinBox(scheduleGroup);
    scheduleRetries_->setRange(0, 10);
    scheduleForm->addRow(tr("Retries after failure"), scheduleRetries_);

    scheduleBackoff_ = new QSpinBox(scheduleGroup);
    scheduleBackoff_->setRange(0, 3600);
    scheduleBackoff_->setSuffix(tr(" s"));
    scheduleBackoff_->setValue(60);
    scheduleForm->addRow(tr("Retry backoff"), scheduleBackoff_);

    scheduleMaxDuration_ = new QSpinBox(scheduleGroup);
    scheduleMaxDuration_->setRange(0, 24 * 3600);
    scheduleMaxDuration_->setSuffix(tr(" s"));
    scheduleMaxDuration_->setSpecialValueText(tr("Unlimited"));
    scheduleForm->addRow(tr("Maximum duration"), scheduleMaxDuration_);

    scheduleSummary_ = new QLabel(scheduleGroup);
    scheduleSummary_->setObjectName(QStringLiteral("pageSubtitle"));
    scheduleSummary_->setWordWrap(true);
    scheduleForm->addRow(QString(), scheduleSummary_);

    rootLayout->addWidget(scheduleGroup);

    auto* hint = new QLabel(
        tr("Archive mode always copies. Source files are never deleted, moved or modified, and "
           "the destination is excluded from scanning so a job cannot archive its own output."),
        this);
    hint->setObjectName(QStringLiteral("pageSubtitle"));
    hint->setWordWrap(true);
    rootLayout->addWidget(hint);

    auto* previewRow = new QHBoxLayout();
    previewButton_ = new QPushButton(tr("Dry-run preview"), this);
    previewProgress_ = new QProgressBar(this);
    previewProgress_->setVisible(false);
    previewLabel_ = new QLabel(this);
    previewLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    previewRow->addWidget(previewButton_);
    previewRow->addWidget(previewProgress_, 1);
    rootLayout->addLayout(previewRow);
    rootLayout->addWidget(previewLabel_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    rootLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &JobEditorDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Populate destination kinds: local folder plus configured providers.
    destinationKind_->addItem(tr("Local folder"), QVariant());
    jobs::JobStore store(context_.database());
    Result<QVector<jobs::StorageProviderRecord>> providers = store.listProviders();
    if (providers) {
        for (const jobs::StorageProviderRecord& provider : providers.value()) {
            const QString label = provider.isRcloneBacked()
                                      ? tr("%1 (%2)").arg(provider.name, provider.rcloneRemote)
                                      : tr("%1 (local: %2)").arg(provider.name, provider.basePath);
            destinationKind_->addItem(label, QVariant(provider.id));
        }
    }

    // Load values.
    if (existing_) {
        nameEdit_->setText(existing_->name);
        enabledCheck_->setChecked(existing_->enabled);
        bandwidthSpin_->setValue(existing_->bandwidthLimitKiB);
        verifyCheck_->setChecked(existing_->verifyChecksum);
        for (const jobs::JobSource& source : existing_->sources) {
            new QListWidgetItem(source.path, sourceList_);
        }
        QStringList patterns;
        for (const scan::ExclusionRule& rule : existing_->exclusions) {
            patterns.append(rule.pattern);
        }
        exclusionsEdit_->setPlainText(patterns.join(QLatin1Char('\n')));

        if (existing_->storageProviderId) {
            const int index = destinationKind_->findData(QVariant(*existing_->storageProviderId));
            if (index >= 0) {
                destinationKind_->setCurrentIndex(index);
            }
            subPathEdit_->setText(existing_->destinationPath);
        } else {
            destinationPathEdit_->setText(existing_->destinationPath);
        }
    } else {
        destinationKind_->setCurrentIndex(0);
    }
    destinationKindChanged();

    // ---- schedule values ---------------------------------------------------
    jobs::JobStore scheduleStore(context_.database());
    sched::ScheduleSpec spec;
    if (existing_ && existing_->id > 0) {
        Result<std::optional<jobs::ScheduleRecord>> stored = scheduleStore.getSchedule(existing_->id);
        if (stored && stored.value().has_value()) {
            spec = stored.value()->spec;
        }
    } else {
        spec.timezone = QTimeZone::systemTimeZoneId();
    }
    const int kindIndex = scheduleKind_->findData(sched::toToken(spec.kind));
    scheduleKind_->setCurrentIndex(kindIndex >= 0 ? kindIndex : 0);
    scheduleTime_->setTime(spec.timeOfDay.isValid() ? spec.timeOfDay : QTime(2, 30));
    scheduleWeekday_->setCurrentIndex(qBound(0, spec.dayOfWeek - 1, 6));
    scheduleMonthDay_->setValue(qBound(1, spec.dayOfMonth, 28));
    scheduleCatchUp_->setChecked(spec.catchUpMissed);
    scheduleRetries_->setValue(qBound(0, spec.retryMaxAttempts, 10));
    scheduleBackoff_->setValue(qBound(0, spec.retryBackoffSeconds, 3600));
    scheduleMaxDuration_->setValue(qBound(0, spec.maxDurationSeconds, 24 * 3600));

    const QString zoneName = QString::fromLatin1(spec.timezone.isEmpty()
                                                     ? QTimeZone::systemTimeZoneId()
                                                     : spec.timezone);
    const int zoneIndex = scheduleTimezone_->findText(zoneName);
    if (zoneIndex >= 0) {
        scheduleTimezone_->setCurrentIndex(zoneIndex);
    } else {
        scheduleTimezone_->setEditText(zoneName);
    }

    connect(scheduleKind_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateScheduleSummary(); });
    connect(scheduleWeekday_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateScheduleSummary(); });
    connect(scheduleTime_, &QTimeEdit::timeChanged, this,
            [this](const QTime&) { updateScheduleSummary(); });
    updateScheduleSummary();

    connect(addButton, &QPushButton::clicked, this, &JobEditorDialog::addSource);
    connect(removeButton, &QPushButton::clicked, this, &JobEditorDialog::removeSource);
    connect(browseButton_, &QPushButton::clicked, this, &JobEditorDialog::browseDestination);
    connect(destinationKind_, &QComboBox::currentIndexChanged, this,
            &JobEditorDialog::destinationKindChanged);
    connect(previewButton_, &QPushButton::clicked, this, &JobEditorDialog::previewRun);

    JobExecutor* executor = context_.executor();
    if (executor != nullptr) {
        connect(executor, &JobExecutor::progressChanged, this,
                [this](qint64, const jobs::RunProgress& progress) {
                    if (!previewProgress_->isVisible()) {
                        return;
                    }
                    if (progress.filesTotal > 0) {
                        previewProgress_->setMaximum(static_cast<int>(progress.filesTotal));
                        previewProgress_->setValue(static_cast<int>(progress.phase == jobs::RunPhase::Done
                                                                        ? progress.filesTotal
                                                                        : progress.filesProcessed));
                    }
                    previewLabel_->setText(progress.message);
                });
        connect(executor, &JobExecutor::finished, this,
                [this](qint64, const jobs::RunReport& report) {
                    previewButton_->setEnabled(true);
                    previewProgress_->setVisible(false);
                    if (report.dryRun) {
                        previewLabel_->setText(
                            tr("Preview: %1 file(s), %2 byte(s) to transfer; %3 unchanged; %4 problem(s)")
                                .arg(report.uploads)
                                .arg(report.counters.bytesTransferred)
                                .arg(report.skipped)
                                .arg(report.failures));
                    } else {
                        previewLabel_->setText(tr("Run finished: %1").arg(report.status));
                    }
                });
    }
}

void JobEditorDialog::destinationKindChanged() {
    const bool local = !destinationKind_->currentData().isValid();
    destinationPathEdit_->setVisible(local);
    browseButton_->setVisible(local);
    subPathEdit_->setVisible(!local);
}

void JobEditorDialog::addSource() {
    const QString directory =
        QFileDialog::getExistingDirectory(this, tr("Select a folder to archive"));
    if (directory.isEmpty()) {
        return;
    }
    for (int row = 0; row < sourceList_->count(); ++row) {
        if (QDir::cleanPath(sourceList_->item(row)->text()) == QDir::cleanPath(directory)) {
            return;
        }
    }
    new QListWidgetItem(QDir::cleanPath(directory), sourceList_);
}

void JobEditorDialog::removeSource() {
    const QList<QListWidgetItem*> selected = sourceList_->selectedItems();
    for (QListWidgetItem* item : selected) {
        delete sourceList_->takeItem(sourceList_->row(item));
    }
}

void JobEditorDialog::browseDestination() {
    const QString directory =
        QFileDialog::getExistingDirectory(this, tr("Select the destination folder"));
    if (!directory.isEmpty()) {
        destinationPathEdit_->setText(QDir::cleanPath(directory));
    }
}

std::optional<jobs::StorageProviderRecord> JobEditorDialog::selectedProvider() const {
    if (!destinationKind_->currentData().isValid()) {
        return std::nullopt;
    }
    jobs::JobStore store(context_.database());
    Result<jobs::StorageProviderRecord> provider =
        store.getProvider(destinationKind_->currentData().toLongLong());
    if (!provider) {
        return std::nullopt;
    }
    return provider.value();
}

jobs::ArchiveJob JobEditorDialog::buildJob() const {
    jobs::ArchiveJob job;
    job.id = existing_ ? existing_->id : 0;
    job.name = nameEdit_->text().trimmed();
    job.mode = QStringLiteral("archive");
    job.enabled = enabledCheck_->isChecked();
    job.bandwidthLimitKiB = bandwidthSpin_->value();
    job.verifyChecksum = verifyCheck_->isChecked();

    const std::optional<jobs::StorageProviderRecord> provider = selectedProvider();
    if (provider) {
        job.storageProviderId = provider->id;
        job.destinationPath = subPathEdit_->text().trimmed();
    } else {
        job.destinationPath = QDir::cleanPath(destinationPathEdit_->text().trimmed());
    }

    for (int row = 0; row < sourceList_->count(); ++row) {
        jobs::JobSource source;
        source.path = sourceList_->item(row)->text();
        if (existing_) {
            for (const jobs::JobSource& previous : existing_->sources) {
                if (QDir::cleanPath(previous.path) == QDir::cleanPath(source.path)) {
                    source.label = previous.label;
                }
            }
        }
        job.sources.append(source);
    }

    const QStringList patterns =
        exclusionsEdit_->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& pattern : patterns) {
        if (!pattern.trimmed().isEmpty()) {
            job.exclusions.append({pattern.trimmed(), scan::PatternKind::Glob});
        }
    }
    return job;
}

bool JobEditorDialog::validate(QString* error) const {
    if (nameEdit_->text().trimmed().isEmpty()) {
        *error = tr("The job needs a name.");
        return false;
    }
    if (sourceList_->count() == 0) {
        *error = tr("Add at least one source folder.");
        return false;
    }
    for (int row = 0; row < sourceList_->count(); ++row) {
        const QString path = sourceList_->item(row)->text();
        if (!QFileInfo(path).isDir()) {
            *error = tr("Source folder does not exist: %1").arg(path);
            return false;
        }
    }

    if (!destinationKind_->currentData().isValid()) {
        const QString destination = destinationPathEdit_->text().trimmed();
        if (destination.isEmpty()) {
            *error = tr("Choose a destination folder.");
            return false;
        }
        if (!QFileInfo(destination).isDir()) {
            *error = tr("The destination folder does not exist: %1").arg(destination);
            return false;
        }
    }
    return true;
}

void JobEditorDialog::accept() {
    QString error;
    if (!validate(&error)) {
        QMessageBox::warning(this, tr("Job is not valid"), error);
        return;
    }

    const jobs::ArchiveJob job = buildJob();
    jobs::JobStore store(context_.database());
    if (existing_ && existing_->id > 0) {
        const Status status = store.updateJob(job);
        if (!status.ok()) {
            QMessageBox::critical(this, tr("Could not save the job"),
                                  QString::fromStdString(status.error().toDisplayString()));
            return;
        }
        savedJobId_ = existing_->id;
        context_.recordEvent(LogLevel::Info, QStringLiteral("jobs"),
                             QStringLiteral("Job updated: %1").arg(job.name));
    } else {
        const Result<qint64> created = store.createJob(job);
        if (!created) {
            QMessageBox::critical(this, tr("Could not create the job"),
                                  QString::fromStdString(created.error().toDisplayString()));
            return;
        }
        savedJobId_ = created.value();
        context_.recordEvent(LogLevel::Info, QStringLiteral("jobs"),
                             QStringLiteral("Job created: %1").arg(job.name));
    }

    const sched::ScheduleSpec spec = buildSchedule();
    if (const auto invalid = spec.validate()) {
        QMessageBox::warning(this, tr("Schedule is not valid"), *invalid);
        return;
    }
    const Status stored = store.upsertSchedule(savedJobId_, spec);
    if (!stored.ok()) {
        QMessageBox::critical(this, tr("Could not save the schedule"),
                              QString::fromStdString(stored.error().toDisplayString()));
        return;
    }
    context_.recordEvent(LogLevel::Info, QStringLiteral("schedule"),
                         QStringLiteral("Schedule for '%1': %2")
                             .arg(job.name, sched::describe(spec)));

    QDialog::accept();
}

void JobEditorDialog::previewRun() {
    QString error;
    if (!validate(&error)) {
        QMessageBox::warning(this, tr("Job is not valid"), error);
        return;
    }
    JobExecutor* executor = context_.executor();
    if (executor == nullptr || executor->isRunning()) {
        previewLabel_->setText(tr("Another run is in progress."));
        return;
    }

    const Status started = executor->startPreview(buildJob(), selectedProvider());
    if (!started.ok()) {
        previewLabel_->setText(QString::fromStdString(started.error().toDisplayString()));
        return;
    }
    previewButton_->setEnabled(false);
    previewProgress_->setVisible(true);
    previewProgress_->setRange(0, 0);
    previewLabel_->setText(tr("Scanning..."));
}

sched::ScheduleSpec JobEditorDialog::buildSchedule() const {
    sched::ScheduleSpec spec;
    spec.kind = sched::triggerKindFromString(scheduleKind_->currentData().toString())
                    .value_or(sched::TriggerKind::Manual);
    spec.timeOfDay = scheduleTime_->time();
    spec.dayOfWeek = scheduleWeekday_->currentData().toInt();
    spec.dayOfMonth = scheduleMonthDay_->value();
    spec.timezone = scheduleTimezone_->currentText().trimmed().toLatin1();
    spec.catchUpMissed = scheduleCatchUp_->isChecked();
    spec.retryMaxAttempts = scheduleRetries_->value();
    spec.retryBackoffSeconds = scheduleBackoff_->value();
    spec.maxDurationSeconds = scheduleMaxDuration_->value();
    return spec;
}

void JobEditorDialog::updateScheduleSummary() {
    const bool recurring = scheduleKind_->currentData().toString() == QStringLiteral("daily") ||
                           scheduleKind_->currentData().toString() == QStringLiteral("weekly") ||
                           scheduleKind_->currentData().toString() == QStringLiteral("monthly");
    scheduleTime_->setEnabled(recurring);
    scheduleWeekday_->setEnabled(scheduleKind_->currentData().toString() == QStringLiteral("weekly"));
    scheduleMonthDay_->setEnabled(scheduleKind_->currentData().toString() == QStringLiteral("monthly"));

    const sched::ScheduleSpec spec = buildSchedule();
    QString summary = sched::describe(spec);
    if (spec.isRecurring() && !spec.validate().has_value()) {
        const QDateTime next = sched::computeNextRun(spec, QDateTime::currentDateTimeUtc());
        if (next.isValid()) {
            summary += QStringLiteral(" \u2014 next run %1")
                           .arg(next.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        }
    }
    if (spec.kind == sched::TriggerKind::Manual) {
        summary = QObject::tr("Runs only when started manually.");
    }
    scheduleSummary_->setText(summary);
}

} // namespace nesva::ui
