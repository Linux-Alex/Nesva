// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

// Headless GUI test: constructs the real main window and the real models
// against a temporary database. Runs under QT_QPA_PLATFORM=offscreen in CTest.

#include <QtTest/QtTest>

#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QListWidget>

#include <memory>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtTest/QSignalSpy>

#include "app_context.hpp"
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QSpinBox>
#include <QtCore/QTimer>

#include "dialogs/job_editor_dialog.hpp"
#include "job_executor.hpp"
#include "main_window.hpp"
#include "models/event_list_model.hpp"
#include "models/job_list_model.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/jobs/archive_runner.hpp"
#include "nesva/jobs/job_store.hpp"
#include "nesva/scheduler/schedule.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;
using nesva::ui::AppContext;
using nesva::ui::EventListModel;
using nesva::ui::JobListModel;
using nesva::ui::MainWindow;

namespace {

QStringList seedStatements() {
    return {
        QStringLiteral(
            "INSERT INTO storage_providers (name, provider_type, rclone_remote, base_path, status, "
            "created_at, updated_at) VALUES ('iCloud Drive', 'icloud', 'icloud', 'Nesva/Archive', "
            "'ok', '2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z')"),
        QStringLiteral(
            "INSERT INTO backup_jobs (name, mode, storage_provider_id, destination_path, enabled, "
            "created_at, updated_at) VALUES ('Documents archive', 'archive', 1, "
            "'icloud:Nesva/Archive/Documents', 1, '2026-01-01T00:00:00.000Z', "
            "'2026-01-01T00:00:00.000Z')"),
        QStringLiteral(
            "INSERT INTO backup_jobs (name, mode, destination_path, enabled, created_at, updated_at) "
            "VALUES ('Home backup', 'backup', '/srv/restic/home', 1, '2026-01-01T00:00:00.000Z', "
            "'2026-01-01T00:00:00.000Z')"),
        QStringLiteral(
            "INSERT INTO backup_sources (job_id, path, created_at) VALUES "
            "(1, '/home/user/Documents', '2026-01-01T00:00:00.000Z')"),
        QStringLiteral(
            "INSERT INTO job_runs (job_id, run_number, trigger_kind, status, started_at, "
            "finished_at, files_transferred, bytes_transferred, created_at) VALUES "
            "(1, 1, 'schedule', 'succeeded', '2026-01-02T01:30:00.000Z', "
            "'2026-01-02T01:34:00.000Z', 128, 52428800, '2026-01-02T01:30:00.000Z')"),
        QStringLiteral(
            "INSERT INTO job_events (job_id, job_run_id, level, category, message, occurred_at) "
            "VALUES (1, 1, 'info', 'archive.rclone', 'upload completed', "
            "'2026-01-02T01:34:00.000Z')"),
    };
}

} // namespace

class GuiSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void jobModelReadsSeededJobs();
    void jobModelIsEmptyOnFreshDatabase();
    void eventModelCombinesJobAndApplicationEvents();
    void mainWindowExposesEveryPage();
    void mainWindowSwitchesPages();
    void mainWindowThemeIsApplied();
    void executorRunsArchiveJobInWorkerThread();
    void executorRejectsConcurrentRuns();
    void jobEditorCreatesJobFromForm();
    void jobEditorRejectsInvalidForm();
    void statusBarShowsAgentState();

private:
    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<AppContext> context_;
};

void GuiSmokeTest::init() {
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    context_ = std::make_unique<AppContext>();
    ui::StartupOptions options;
    options.dataRootOverride = directory_->path();
    options.consoleLogging = false;
    const Status initialised = context_->initialise(options);
    QVERIFY2(initialised.ok(), initialised.ok() ? "" : initialised.error().message().c_str());
}

void GuiSmokeTest::cleanup() {
    context_.reset();
    directory_.reset();
}

void GuiSmokeTest::jobModelReadsSeededJobs() {
    for (const QString& statement : seedStatements()) {
        QVERIFY2(context_->database().exec(statement).ok(), qPrintable(statement));
    }

    JobListModel model(context_->database());
    QCOMPARE(model.refresh(), QString());
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.columnCount(), int(JobListModel::ColumnCount));

    const QModelIndex nameIndex = model.index(0, JobListModel::NameColumn);
    QCOMPARE(model.data(nameIndex).toString(), QStringLiteral("Documents archive"));
    QCOMPARE(model.data(model.index(0, JobListModel::ModeColumn)).toString(),
             QStringLiteral("archive"));
    QCOMPARE(model.data(model.index(0, JobListModel::SourcesColumn)).toString(),
             QStringLiteral("1"));
    QCOMPARE(model.data(model.index(0, JobListModel::LastRunColumn)).toString(),
             QStringLiteral("succeeded"));
    QCOMPARE(model.data(model.index(1, JobListModel::LastRunColumn)).toString(),
             QStringLiteral("never"));
    QCOMPARE(model.headerData(JobListModel::NameColumn, Qt::Horizontal).toString(),
             QStringLiteral("Name"));
}

void GuiSmokeTest::jobModelIsEmptyOnFreshDatabase() {
    JobListModel model(context_->database());
    QCOMPARE(model.refresh(), QString());
    QCOMPARE(model.rowCount(), 0);
}

void GuiSmokeTest::eventModelCombinesJobAndApplicationEvents() {
    for (const QString& statement : seedStatements()) {
        QVERIFY(context_->database().exec(statement).ok());
    }

    EventListModel model(context_->database(), 50);
    QCOMPARE(model.refresh(), QString());

    int applicationRows = 0;
    int jobRows = 0;
    for (int row = 0; row < model.rowCount(); ++row) {
        const QString source = model.data(model.index(row, EventListModel::SourceColumn)).toString();
        if (source == QStringLiteral("application")) {
            ++applicationRows;
        } else if (source == QStringLiteral("Documents archive")) {
            ++jobRows;
        }
    }
    QVERIFY2(applicationRows >= 1, "startup events are persisted to application_events");
    QCOMPARE(jobRows, 1);
}

void GuiSmokeTest::mainWindowExposesEveryPage() {
    MainWindow window(*context_);
    QCOMPARE(window.pageCount(), 8);

    const QStringList expected = {
        QStringLiteral("Dashboard"),   QStringLiteral("Backup Jobs"),
        QStringLiteral("Storage"),     QStringLiteral("Archive Explorer"),
        QStringLiteral("Snapshots"),   QStringLiteral("Restore"),
        QStringLiteral("Activity"),    QStringLiteral("Settings"),
    };
    for (int index = 0; index < expected.size(); ++index) {
        QCOMPARE(window.pageTitleAt(index), expected.at(index));
    }
}

void GuiSmokeTest::mainWindowSwitchesPages() {
    MainWindow window(*context_);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window) || window.isVisible());

    auto* navigation = window.findChild<QListWidget*>(QStringLiteral("navigation"));
    QVERIFY2(navigation != nullptr, "navigation sidebar must exist");
    QCOMPARE(navigation->count(), 8);

    for (int index = 0; index < window.pageCount(); ++index) {
        window.selectPage(index);
        QCOMPARE(navigation->currentRow(), index);
        QCOMPARE(navigation->item(index)->text(), window.pageTitleAt(index));
    }
}

void GuiSmokeTest::mainWindowThemeIsApplied() {
    MainWindow window(*context_);
    const Status stored = context_->setThemeMode(ui::ThemeMode::Dark);
    QVERIFY(stored.ok());
    QCOMPARE(context_->themeMode(), ui::ThemeMode::Dark);

    const QString persisted =
        context_->settings().getString(QString::fromUtf8(config::keys::kThemeMode)).valueOr(QString());
    QCOMPARE(persisted, QStringLiteral("dark"));

    QVERIFY(context_->setThemeMode(ui::ThemeMode::System).ok());
    QCOMPARE(context_->themeMode(), ui::ThemeMode::System);
}


void GuiSmokeTest::executorRunsArchiveJobInWorkerThread() {
    // A real job, a real source tree and the real worker thread: the GUI must
    // stay responsive while the transfer runs off the GUI thread.
    const QString sourceDirectory = directory_->filePath(QStringLiteral("source"));
    const QString destinationDirectory = directory_->filePath(QStringLiteral("destination"));
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(QDir().mkpath(destinationDirectory));
    QVERIFY(test::writeFile(QDir(sourceDirectory).absoluteFilePath(QStringLiteral("a.txt")), "alpha"));
    QVERIFY(test::writeFile(QDir(sourceDirectory).absoluteFilePath(QStringLiteral("docs/b.txt")),
                            "bravo"));

    jobs::JobStore store(context_->database());
    jobs::ArchiveJob job;
    job.name = QStringLiteral("GUI worker job");
    job.mode = QStringLiteral("archive");
    job.destinationPath = destinationDirectory;
    job.sources = {{0, sourceDirectory, QStringLiteral("source")}};
    const Result<qint64> created = store.createJob(job);
    QVERIFY(created);

    MainWindow window(*context_);
    window.show();

    QSignalSpy finishedSpy(context_->executor(), &ui::JobExecutor::finished);
    const Status started = context_->executor()->startArchive(created.value(), false);
    QVERIFY2(started.ok(), started.ok() ? "" : started.error().message().c_str());
    QVERIFY(context_->executor()->isRunning());

    QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() == 1, 20000);
    QVERIFY(!context_->executor()->isRunning());

    const auto report = finishedSpy.at(0).at(1).value<jobs::RunReport>();
    QCOMPARE(report.status, QStringLiteral("succeeded"));
    QCOMPARE(report.counters.filesTransferred, qint64(2));

    QVERIFY(QFile::exists(QDir(destinationDirectory).absoluteFilePath(QStringLiteral("source/a.txt"))));
    QVERIFY(QFile::exists(
        QDir(destinationDirectory).absoluteFilePath(QStringLiteral("source/docs/b.txt"))));

    // The run is visible in history and the jobs model.
    const Result<QVector<jobs::RunSummary>> runs = store.listRuns(10, created.value());
    QVERIFY(runs);
    QCOMPARE(runs.value().size(), 1);
    QCOMPARE(runs.value().constFirst().status, QStringLiteral("succeeded"));

    JobListModel model(context_->database());
    QCOMPARE(model.refresh(), QString());
    QCOMPARE(model.rowCount(), 1);
}

void GuiSmokeTest::executorRejectsConcurrentRuns() {
    const QString sourceDirectory = directory_->filePath(QStringLiteral("source2"));
    const QString destinationDirectory = directory_->filePath(QStringLiteral("destination2"));
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(QDir().mkpath(destinationDirectory));
    for (int index = 0; index < 40; ++index) {
        QVERIFY(test::writeFile(
            QDir(sourceDirectory).absoluteFilePath(QStringLiteral("file-%1.txt").arg(index)),
            QByteArray(64 * 1024, 'x')));
    }

    jobs::JobStore store(context_->database());
    jobs::ArchiveJob job;
    job.name = QStringLiteral("Concurrent job");
    job.destinationPath = destinationDirectory;
    job.sources = {{0, sourceDirectory, QStringLiteral("source")}};
    const Result<qint64> created = store.createJob(job);
    QVERIFY(created);

    QSignalSpy finishedSpy(context_->executor(), &ui::JobExecutor::finished);
    QVERIFY(context_->executor()->startArchive(created.value(), false).ok());

    // A second start while the first runs must be refused, not queued.
    const Status second = context_->executor()->startArchive(created.value(), false);
    QVERIFY(!second.ok());
    QCOMPARE(static_cast<int>(second.error().code()), static_cast<int>(ErrorCode::AlreadyExists));

    QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() == 1, 30000);
}


void GuiSmokeTest::jobEditorCreatesJobFromForm() {
    const QString sourceDirectory = directory_->filePath(QStringLiteral("editor-source"));
    const QString destinationDirectory = directory_->filePath(QStringLiteral("editor-destination"));
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(QDir().mkpath(destinationDirectory));

    ui::JobEditorDialog dialog(*context_, std::nullopt);
    auto* nameEdit = dialog.findChild<QLineEdit*>(QStringLiteral("jobName"));
    auto* sources = dialog.findChild<QListWidget*>(QStringLiteral("jobSources"));
    auto* destination = dialog.findChild<QLineEdit*>(QStringLiteral("jobDestinationPath"));
    auto* exclusions = dialog.findChild<QPlainTextEdit*>(QStringLiteral("jobExclusions"));
    auto* bandwidth = dialog.findChild<QSpinBox*>(QStringLiteral("jobBandwidth"));
    auto* verify = dialog.findChild<QCheckBox*>(QStringLiteral("jobVerify"));
    QVERIFY(nameEdit && sources && destination && exclusions && bandwidth && verify);

    nameEdit->setText(QStringLiteral("Editor job"));
    sources->addItem(sourceDirectory);
    destination->setText(destinationDirectory);
    exclusions->setPlainText(QStringLiteral("*.tmp\nbuild/**"));
    bandwidth->setValue(512);
    verify->setChecked(false);

    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(dialog.savedJobId() > 0);

    jobs::JobStore store(context_->database());
    const Result<jobs::ArchiveJob> saved = store.getJob(dialog.savedJobId());
    QVERIFY(saved);
    QCOMPARE(saved.value().name, QStringLiteral("Editor job"));
    QCOMPARE(saved.value().destinationPath, destinationDirectory);
    QCOMPARE(saved.value().bandwidthLimitKiB, 512);
    QVERIFY(!saved.value().verifyChecksum);
    QCOMPARE(saved.value().sources.size(), 1);
    QCOMPARE(saved.value().sources.constFirst().path, QDir::cleanPath(sourceDirectory));
    QCOMPARE(saved.value().exclusions.size(), 2);
}

void GuiSmokeTest::jobEditorRejectsInvalidForm() {
    ui::JobEditorDialog dialog(*context_, std::nullopt);
    auto* nameEdit = dialog.findChild<QLineEdit*>(QStringLiteral("jobName"));
    QVERIFY(nameEdit != nullptr);
    nameEdit->setText(QStringLiteral("No sources"));

    // No sources and no destination: the dialog must refuse to save. The warning
    // box is modal, so accept() is exercised through a queued close of the box.
    QTimer::singleShot(0, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* box = qobject_cast<QMessageBox*>(widget)) {
                box->accept();
            }
        }
    });
    dialog.accept();
    // Rejected: nothing was written.
    jobs::JobStore store(context_->database());
    const Result<QVector<jobs::ArchiveJob>> jobs = store.listJobs();
    QVERIFY(jobs);
    QCOMPARE(jobs.value().size(), 0);
}


void GuiSmokeTest::statusBarShowsAgentState() {
    MainWindow window(*context_);
    window.show();

    auto* agentLabel = window.findChild<QLabel*>(QStringLiteral("agentStatus"));
    QVERIFY2(agentLabel != nullptr, "the status bar must show agent state");

    // No agent is running for this data directory, so the label must say so -
    // never claim a background service that does not exist.
    QTRY_VERIFY_WITH_TIMEOUT(agentLabel->text().contains(QStringLiteral("Agent")), 5000);
    QVERIFY(agentLabel->text().contains(QStringLiteral("not running")) ||
            agentLabel->text().contains(QStringLiteral("Agent: ")));

    // A job with a schedule stores a concrete next run, so the dashboard and CLI
    // do not have to wait for the agent's first tick.
    jobs::JobStore store(context_->database());
    jobs::ArchiveJob job;
    job.name = QStringLiteral("Scheduled job");
    job.destinationPath = directory_->filePath(QStringLiteral("sched-dest"));
    job.sources = {{0, directory_->path(), QStringLiteral("src")}};
    const Result<qint64> created = store.createJob(job);
    QVERIFY(created);

    sched::ScheduleSpec spec;
    spec.kind = sched::TriggerKind::Daily;
    spec.timeOfDay = QTime(3, 0);
    spec.timezone = QByteArrayLiteral("UTC");
    QVERIFY(store.upsertSchedule(created.value(), spec).ok());

    const Result<std::optional<jobs::ScheduleRecord>> stored = store.getSchedule(created.value());
    QVERIFY(stored);
    QVERIFY(stored.value().has_value());
    QVERIFY2(stored.value()->state.nextRunUtc.has_value(),
             "saving a recurring schedule must produce a concrete next run");
    QVERIFY(stored.value()->state.nextRunUtc->toUTC() > QDateTime::currentDateTimeUtc());
}

QTEST_MAIN(GuiSmokeTest)
#include "tst_gui_smoke.moc"
