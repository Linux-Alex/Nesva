// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "snapshot_restore_dialog.hpp"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDir>
#include <QtCore/QFuture>
#include <QtCore/QFutureWatcher>
#include <QtCore/QMetaObject>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

#include <utility>

#include "app_context.hpp"
#include "nesva/backup/restic.hpp"
#include "nesva/config/settings_store.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/db/database.hpp"
#include "nesva/jobs/backup_runner.hpp"

namespace nesva::ui {

SnapshotRestoreDialog::SnapshotRestoreDialog(AppContext& context, jobs::ArchiveJob job,
                                             QString snapshotId, QWidget* parent)
    : QDialog(parent), context_(context), job_(std::move(job)), snapshotId_(std::move(snapshotId)) {
    setWindowTitle(QStringLiteral("Restore snapshot"));
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(
        QStringLiteral("Restore snapshot <b>%1</b> of job <b>%2</b>.\n"
                       "Files are staged first and then moved into place, so an existing file is "
                       "only replaced when you choose that below.")
            .arg(snapshotId_.left(8), job_.name),
        this);
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    auto* destinationRow = new QWidget(this);
    auto* destinationLayout = new QHBoxLayout(destinationRow);
    destinationLayout->setContentsMargins(0, 0, 0, 0);
    destinationEdit_ = new QLineEdit(destinationRow);
    destinationEdit_->setPlaceholderText(QStringLiteral("Empty folder to restore into"));
    auto* browseButton = new QPushButton(QStringLiteral("Browse..."), destinationRow);
    destinationLayout->addWidget(destinationEdit_, 1);
    destinationLayout->addWidget(browseButton);
    form->addRow(QStringLiteral("Destination"), destinationRow);

    policyCombo_ = new QComboBox(this);
    policyCombo_->addItem(QStringLiteral("Skip files that already exist"), QStringLiteral("skip"));
    policyCombo_->addItem(QStringLiteral("Rename the restored copy"), QStringLiteral("rename"));
    policyCombo_->addItem(QStringLiteral("Overwrite existing files"), QStringLiteral("overwrite"));
    form->addRow(QStringLiteral("If a file exists"), policyCombo_);
    layout->addLayout(form);

    progress_ = new QProgressBar(this);
    progress_->setVisible(false);
    progress_->setRange(0, 0);
    layout->addWidget(progress_);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    restoreButton_ = buttons->addButton(QStringLiteral("Restore"), QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browseButton, &QPushButton::clicked, this, &SnapshotRestoreDialog::browseDestination);
    connect(restoreButton_, &QPushButton::clicked, this, &SnapshotRestoreDialog::startRestore);
    connect(policyCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateActionState(); });
    connect(destinationEdit_, &QLineEdit::textChanged, this,
            [this](const QString&) { updateActionState(); });

    updateActionState();
}

SnapshotRestoreDialog::~SnapshotRestoreDialog() {
    if (watcher_ && watcher_->isRunning()) {
        cancellation_.cancel();
        watcher_->waitForFinished();
    }
}

void SnapshotRestoreDialog::updateActionState() {
    const bool ready = !destinationEdit_->text().trimmed().isEmpty() && !restoring_;
    restoreButton_->setEnabled(ready);
}

void SnapshotRestoreDialog::browseDestination() {
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select the restore destination"));
    if (!directory.isEmpty()) {
        destinationEdit_->setText(QDir::cleanPath(directory));
    }
}

void SnapshotRestoreDialog::startRestore() {
    if (restoring_) {
        return;
    }
    const QString destination = destinationEdit_->text().trimmed();
    if (destination.isEmpty()) {
        return;
    }
    if (policyCombo_->currentData().toString() == QStringLiteral("overwrite")) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Overwrite files?"),
            QStringLiteral("Overwrite existing files in the destination with the snapshot content?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    jobs::BackupRestorer::Request request;
    request.job = job_;
    request.snapshotId = snapshotId_;
    request.destinationDirectory = destination;
    request.policy = jobs::conflictPolicyFromString(policyCombo_->currentData().toString(),
                                                    jobs::ConflictPolicy::Skip);
    request.passwordOptions = jobs::passwordOptionsFromSettings(context_.database());
    request.resticExecutable =
        context_.settings()
            .getString(QString::fromUtf8(config::keys::kResticBinaryPath), QStringLiteral("restic"))
            .valueOr(QStringLiteral("restic"));

    const QString databasePath = Paths::databasePath();
    restoring_ = true;
    cancellation_ = CancellationToken();
    progress_->setVisible(true);
    statusLabel_->setText(QStringLiteral("Restoring snapshot %1...").arg(snapshotId_.left(8)));
    updateActionState();

    auto* watcher = new QFutureWatcher<jobs::BackupRestorer::Report>(this);
    watcher_.reset(watcher);
    watcher->setFuture(QtConcurrent::run([this, request, databasePath]() {
        db::Database database;
        const Status opened = database.open(databasePath);
        if (!opened) {
            jobs::BackupRestorer::Report report;
            report.errors.append(QString::fromStdString(opened.error().toDisplayString()));
            return report;
        }
        jobs::BackupRestorer restorer(database);
        return restorer.restore(request, cancellation_);
    }));
    connect(watcher, &QFutureWatcher<jobs::BackupRestorer::Report>::finished, this,
            &SnapshotRestoreDialog::finishRestore);
}

void SnapshotRestoreDialog::finishRestore() {
    restoring_ = false;
    progress_->setVisible(false);
    auto* watcher = static_cast<QFutureWatcher<jobs::BackupRestorer::Report>*>(watcher_.get());
    if (watcher != nullptr && watcher->isFinished()) {
        const jobs::BackupRestorer::Report report = watcher->result();
        statusLabel_->setText(
            QStringLiteral("%1: %2 restored, %3 skipped, %4 renamed, %5 failed")
                .arg(report.status)
                .arg(report.restored)
                .arg(report.skipped)
                .arg(report.renamed)
                .arg(report.failed));
        if (!report.errors.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Restore finished with problems"),
                                 report.errors.join(QLatin1Char('\n')));
        }
    }
    updateActionState();
}

} // namespace nesva::ui
