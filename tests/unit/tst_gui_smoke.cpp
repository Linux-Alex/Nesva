// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

// Headless GUI test: constructs the real main window and the real models
// against a temporary database. Runs under QT_QPA_PLATFORM=offscreen in CTest.

#include <QtTest/QtTest>

#include <QtCore/QTemporaryDir>
#include <QtWidgets/QApplication>
#include <QtWidgets/QListWidget>

#include <memory>

#include "app_context.hpp"
#include "main_window.hpp"
#include "models/event_list_model.hpp"
#include "models/job_list_model.hpp"
#include "nesva/core/paths.hpp"

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

QTEST_MAIN(GuiSmokeTest)
#include "tst_gui_smoke.moc"
