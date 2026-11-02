// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "snapshot_browser_dialog.hpp"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDir>
#include <QtCore/QFuture>
#include <QtCore/QFutureWatcher>
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QTableView>
#include <QtWidgets/QVBoxLayout>

#include <utility>

#include "app_context.hpp"
#include "nesva/backup/restic.hpp"
#include "nesva/config/settings_store.hpp"
#include "nesva/jobs/backup_runner.hpp"

namespace nesva::ui {

SnapshotBrowserDialog::SnapshotBrowserDialog(AppContext& context, jobs::ArchiveJob job,
                                             QString snapshotId, QWidget* parent)
    : QDialog(parent), context_(context), job_(std::move(job)), snapshotId_(std::move(snapshotId)) {
    setWindowTitle(QStringLiteral("Snapshot %1 - %2").arg(snapshotId_.left(8), job_.name));
    resize(900, 520);

    auto* layout = new QVBoxLayout(this);

    auto* toolbar = new QHBoxLayout();
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(QStringLiteral("Filter by path"));
    filterEdit_->setClearButtonEnabled(true);
    toolbar->addWidget(filterEdit_, 1);
    layout->addLayout(toolbar);

    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({QStringLiteral("Path"), QStringLiteral("Type"),
                                       QStringLiteral("Bytes"), QStringLiteral("Modified")});
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(0);

    view_ = new QTableView(this);
    view_->setModel(proxy_);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setAlternatingRowColors(true);
    view_->setSortingEnabled(true);
    view_->verticalHeader()->setVisible(false);
    view_->horizontalHeader()->setStretchLastSection(true);
    view_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(view_, 1);

    busyBar_ = new QProgressBar(this);
    busyBar_->setRange(0, 0);
    layout->addWidget(busyBar_);

    statusLabel_ = new QLabel(QStringLiteral("Reading the snapshot..."), this);
    statusLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(filterEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) { proxy_->setFilterFixedString(text); });

    load();
}

SnapshotBrowserDialog::~SnapshotBrowserDialog() {
    if (watcher_ && watcher_->isRunning()) {
        cancellation_.cancel();
        watcher_->waitForFinished();
    }
}

void SnapshotBrowserDialog::load() {
    const QString resticExecutable =
        context_.settings()
            .getString(QString::fromUtf8(config::keys::kResticBinaryPath), QStringLiteral("restic"))
            .valueOr(QStringLiteral("restic"));
    const jobs::PasswordOptions passwordOptions =
        jobs::passwordOptionsFromSettings(context_.database());
    const jobs::ArchiveJob job = job_;
    const QString snapshotId = snapshotId_;

    auto* watcher = new QFutureWatcher<QVector<backup::FileEntry>>(this);
    watcher_.reset(watcher);
    watcher->setFuture(QtConcurrent::run([job, snapshotId, resticExecutable, passwordOptions]() {
        const Result<QByteArray> password =
            jobs::loadRepositoryPassword(job.destinationPath, passwordOptions);
        if (!password) {
            return QVector<backup::FileEntry>{};
        }
        const Result<QString> passwordFile = backup::createPasswordFile(password.value());
        if (!passwordFile) {
            return QVector<backup::FileEntry>{};
        }
        backup::ResticRepository repository(resticExecutable, job.destinationPath,
                                            passwordFile.value());
        const Result<QVector<backup::FileEntry>> entries = repository.listSnapshot(snapshotId);
        QFile::remove(passwordFile.value());
        if (!entries) {
            return QVector<backup::FileEntry>{};
        }
        return entries.value();
    }));
    connect(watcher, &QFutureWatcher<QVector<backup::FileEntry>>::finished, this,
            &SnapshotBrowserDialog::finishLoad);
}

void SnapshotBrowserDialog::finishLoad() {
    busyBar_->setVisible(false);
    auto* watcher = static_cast<QFutureWatcher<QVector<backup::FileEntry>>*>(watcher_.get());
    if (watcher == nullptr || !watcher->isFinished()) {
        return;
    }
    const QVector<backup::FileEntry> entries = watcher->result();
    if (entries.isEmpty()) {
        loadError_ = QStringLiteral("Could not read the snapshot contents. Check the repository "
                                    "password and that Restic is installed.");
        statusLabel_->setText(loadError_);
    }

    qint64 totalBytes = 0;
    for (const backup::FileEntry& entry : entries) {
        if (entry.type != QStringLiteral("file")) {
            continue;
        }
        ++fileCount_;
        totalBytes += entry.sizeBytes;
        QList<QStandardItem*> row;
        row.append(new QStandardItem(entry.path));
        row.append(new QStandardItem(entry.type));
        row.append(new QStandardItem(QString::number(entry.sizeBytes)));
        row.append(new QStandardItem(
            entry.modifiedUtc.isValid()
                ? entry.modifiedUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                : QString()));
        model_->appendRow(row);
    }

    if (fileCount_ > 0) {
        statusLabel_->setText(QStringLiteral("%1 file(s), %2 byte(s) in this snapshot.")
                                  .arg(fileCount_)
                                  .arg(totalBytes));
    }
    emit loaded(fileCount_);
}

} // namespace nesva::ui
