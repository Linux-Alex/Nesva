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
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTableView>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QFrame>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtCore/QTimer>

#include "app_autostart.hpp"
#include "single_instance.hpp"
#include "dialogs/job_editor_dialog.hpp"
#include "dialogs/rclone_config_dialog.hpp"
#include "time_display.hpp"
#include "widgets/elided_label.hpp"
#include "dialogs/snapshot_browser_dialog.hpp"
#include "pages/snapshots_page.hpp"
#include "nesva/backup/restic.hpp"
#include "nesva/core/external_tool.hpp"
#include "nesva/jobs/backup_runner.hpp"
#include "job_executor.hpp"
#include "main_window.hpp"
#include "models/event_list_model.hpp"
#include "models/job_list_model.hpp"
#include "nesva/core/paths.hpp"
#include "nesva/jobs/archive_runner.hpp"
#include "nesva/mail/mail_store.hpp"
#include "nesva/jobs/job_store.hpp"
#include "nesva/scheduler/schedule.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;
using nesva::ui::AppContext;
using nesva::ui::EventListModel;
using nesva::ui::JobListModel;
using nesva::ui::ElidedLabel;
using nesva::ui::MainWindow;
using nesva::ui::Page;

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
    void settingsPageOffersTheRcloneInstaller();
    void settingsPageOffersTrayAutostart();
    void singleInstanceActivatesTheFirstInstance();
    void mailPageShowsAccountsAndFolderCounts();
    void dashboardCardsStackWhenNarrow();
    void jobModelAlignsNumericColumns();
    void timestampsUseTheConfiguredZone();
    void rcloneWizardShipsPerProviderTutorials();
    void statusTextDoesNotResizeTheJobsTable();
    void settingsPageScrollsInsteadOfSqueezing();
    void mainWindowSwitchesPages();
    void mainWindowThemeIsApplied();
    void executorRunsArchiveJobInWorkerThread();
    void executorRejectsConcurrentRuns();
    void jobEditorCreatesJobFromForm();
    void jobEditorRejectsInvalidForm();
    void statusBarShowsAgentState();
    void jobEditorCreatesBackupJobFromForm();
    void snapshotsPageListsRecordedSnapshots();
    void executorRunsBackupJob();

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

void GuiSmokeTest::settingsPageScrollsInsteadOfSqueezing() {
    // A page with more cards than the window is tall has to scroll. Without a
    // scroll area Qt compresses every card, the buttons become unreadable and
    // later sections are simply not reachable.
    MainWindow window(*context_);
    window.show();

    int settingsIndex = -1;
    for (int index = 0; index < window.pageCount(); ++index) {
        if (window.pageTitleAt(index) == QStringLiteral("Settings")) {
            settingsIndex = index;
            break;
        }
    }
    QVERIFY(settingsIndex >= 0);

    // A roomy window and a small laptop screen: the content must never be
    // compressed below what it needs.
    for (const QSize size : {QSize(1100, 700), QSize(900, 560)}) {
        window.resize(size);
        window.selectPage(settingsIndex);
        QCoreApplication::processEvents();

        Page* settings = window.pageAt(settingsIndex);
        QVERIFY(settings != nullptr);
        auto* scroll = settings->findChild<QScrollArea*>(QStringLiteral("pageBodyScroll"));
        QVERIFY2(scroll != nullptr, "page bodies must live in a scroll area");
        QVERIFY(scroll->widgetResizable());

        const int contentHeight = scroll->widget()->sizeHint().height();
        const int viewportHeight = scroll->viewport()->height();
        QVERIFY2(contentHeight > viewportHeight,
                 qPrintable(QStringLiteral("at %1x%2 the Settings content (%3 px) must be "
                                           "taller than the viewport (%4 px)")
                                .arg(size.width())
                                .arg(size.height())
                                .arg(contentHeight)
                                .arg(viewportHeight)));
        QVERIFY2(scroll->verticalScrollBar()->maximum() > 0,
                 "a window this size must scroll the Settings page rather than squeeze it");

        // The installer must be inside that scroll area and reachable by
        // scrolling, not clipped away.
        QPushButton* install = nullptr;
        for (QPushButton* button : scroll->findChildren<QPushButton*>()) {
            if (button->text().contains(QStringLiteral("Rclone"))) {
                install = button;
                break;
            }
        }
        QVERIFY2(install != nullptr, "the Rclone installer must live inside the page body");

        // Restic needs a versioned backup repository, so its installer has to be
        // reachable from the same place - rclone alone cannot create a backup.
        bool resticInstaller = false;
        for (QPushButton* button : scroll->findChildren<QPushButton*>()) {
            if (button->text().contains(QStringLiteral("Restic"))) {
                resticInstaller = true;
                break;
            }
        }
        QVERIFY2(resticInstaller, "the Restic installer must be offered next to the Rclone one");

        scroll->ensureWidgetVisible(install, 0, 0);
        QVERIFY(install->isVisible());
        const QRect viewport = scroll->viewport()->rect();
        const QRect inViewport(install->mapTo(scroll->viewport(), QPoint(0, 0)), install->size());
        QVERIFY2(viewport.intersects(inViewport),
                 qPrintable(QStringLiteral("installer button at y=%1 is outside the %2 px "
                                           "viewport")
                                .arg(inViewport.y())
                                .arg(viewport.height())));
    }
}

void GuiSmokeTest::statusTextDoesNotResizeTheJobsTable() {
    // A long run message used to wrap, grow its row and push the table down
    // while a job was running. The status lines must never change the layout.
    jobs::JobStore store(context_->database());
    jobs::ArchiveJob job;
    job.name = QStringLiteral("Layout job");
    job.mode = QStringLiteral("archive");
    job.destinationPath = directory_->filePath(QStringLiteral("destination"));
    job.sources = {{0, directory_->path(), QStringLiteral("source")}};
    QVERIFY(store.createJob(job));

    MainWindow window(*context_);
    window.resize(1100, 700);
    window.show();

    int jobsIndex = -1;
    for (int index = 0; index < window.pageCount(); ++index) {
        if (window.pageTitleAt(index) == QStringLiteral("Backup Jobs")) {
            jobsIndex = index;
            break;
        }
    }
    QVERIFY(jobsIndex >= 0);
    window.selectPage(jobsIndex);
    QCoreApplication::processEvents();

    Page* page = window.pageAt(jobsIndex);
    QVERIFY(page != nullptr);
    auto* table = page->findChild<QTableView*>();
    QVERIFY(table != nullptr);
    QVERIFY(table->isVisible());

    const QRect before = table->geometry();
    const QList<ElidedLabel*> labels = page->findChildren<ElidedLabel*>();
    QVERIFY(!labels.isEmpty());
    const QString longMessage =
        QStringLiteral("Running Documents archive: transferred 1204 files, skipped 88 unchanged "
                       "files, 0 failed - /home/you/Documents/deeply/nested/folder/file.txt");
    for (ElidedLabel* label : labels) {
        label->setText(longMessage);
    }
    QCoreApplication::processEvents();

    QCOMPARE(table->geometry(), before);
    // The whole text is still reachable, through the tooltip.
    QCOMPARE(labels.first()->toolTip(), longMessage);
    QCOMPARE(labels.first()->text(), longMessage);
}

void GuiSmokeTest::dashboardCardsStackWhenNarrow() {
    // Five cards in a QHBoxLayout were squeezed into slivers on a narrow window.
    // They must flow onto further rows instead.
    MainWindow window(*context_);
    window.resize(560, 720);
    window.show();
    window.selectPage(0); // Dashboard
    QCoreApplication::processEvents();

    Page* dashboard = window.pageAt(0);
    QVERIFY(dashboard != nullptr);
    const QList<QFrame*> cards = dashboard->findChildren<QFrame*>(QStringLiteral("statCard"));
    QVERIFY2(cards.size() >= 5, "the dashboard cards must exist");

    // Collect the cards that share the first row: at 560 px not all five fit.
    QSet<int> rowTops;
    for (QFrame* card : cards) {
        if (card->isVisible()) {
            rowTops.insert(card->mapTo(dashboard, QPoint(0, 0)).y());
        }
    }
    QVERIFY2(rowTops.size() >= 2,
             qPrintable(QStringLiteral("expected the cards to occupy several rows, got %1")
                            .arg(rowTops.size())));
}

void GuiSmokeTest::jobModelAlignsNumericColumns() {
    // The ID column used to return the string "right" for Qt::TextAlignmentRole,
    // which Qt ignores: the cell was drawn top-left and looked wrong next to the
    // other columns.
    jobs::JobStore store(context_->database());
    jobs::ArchiveJob job;
    job.name = QStringLiteral("Align job");
    job.destinationPath = directory_->filePath(QStringLiteral("dest"));
    job.sources = {{0, directory_->path(), QStringLiteral("source")}};
    QVERIFY(store.createJob(job));

    JobListModel model(context_->database());
    QCOMPARE(model.refresh(), QString());
    const QVariant alignment =
        model.data(model.index(0, JobListModel::IdColumn), Qt::TextAlignmentRole);
    QVERIFY2(alignment.isValid(), "the ID column must report an alignment");
    const int value = alignment.toInt();
    QVERIFY2((value & Qt::AlignRight) != 0, "IDs are right aligned");
    QVERIFY2((value & Qt::AlignVCenter) != 0, "every cell is vertically centred");
}

void GuiSmokeTest::timestampsUseTheConfiguredZone() {
    // Stored timestamps are UTC; the UI renders them in the configured zone.
    const QTimeZone ljubljana("Europe/Ljubljana");
    QVERIFY(ljubljana.isValid());
    QCOMPARE(ui::formatInZone(QStringLiteral("2026-11-03T19:15:00.000Z"), ljubljana),
             QStringLiteral("2026-11-03 20:15"));
    const QTimeZone utc("UTC");
    QCOMPARE(ui::formatInZone(QStringLiteral("2026-11-03T19:15:00.000Z"), utc),
             QStringLiteral("2026-11-03 19:15"));
    // Unparsable input must not become an epoch date.
    QCOMPARE(ui::formatInZone(QString(), ljubljana), QString(QChar(0x2014)));
    QVERIFY(ui::resolveDisplayTimeZone(QString()).isValid());
    QCOMPARE(QString::fromUtf8(ui::resolveDisplayTimeZone(QStringLiteral("Not/AZone")).id()),
             QString::fromUtf8(QTimeZone::systemTimeZone().id()));
}

void GuiSmokeTest::rcloneWizardShipsPerProviderTutorials() {
    // The wizard is the only place the credentials walkthrough lives, so every
    // supported provider must ship one, with the backend name rclone expects.
    const QVector<ui::RcloneProviderProfile> profiles = ui::rcloneProviderProfiles();
    QVERIFY(profiles.size() >= 6);

    QStringList backends;
    for (const ui::RcloneProviderProfile& profile : profiles) {
        backends.append(profile.backend);
        QVERIFY2(!profile.html.isEmpty(), qPrintable(profile.label));
        QVERIFY2(profile.html.contains(QStringLiteral("Storage&gt;")),
                 qPrintable(profile.label));
        QVERIFY2(!profile.remote.isEmpty(), qPrintable(profile.label));
        // Every tutorial ends with the same two answers rclone needs.
        QVERIFY2(profile.html.contains(QStringLiteral("Keep this remote?")),
                 qPrintable(profile.label));
    }
    for (const QString& expected : {QStringLiteral("drive"), QStringLiteral("pcloud"),
                                    QStringLiteral("b2"), QStringLiteral("onedrive"),
                                    QStringLiteral("iclouddrive"), QStringLiteral("webdav")}) {
        QVERIFY2(backends.contains(expected), qPrintable(expected));
    }

    // Google Drive must warn about needing your own OAuth client, and B2 must
    // point at the application key screen: those are the two steps people miss.
    const auto driveProfile = std::find_if(profiles.cbegin(), profiles.cend(), [](const auto& p) {
        return p.backend == QStringLiteral("drive");
    });
    QVERIFY(driveProfile != profiles.cend());
    QVERIFY(driveProfile->html.contains(QStringLiteral("OAuth client ID")));

    const auto b2Profile = std::find_if(profiles.cbegin(), profiles.cend(), [](const auto& p) {
        return p.backend == QStringLiteral("b2");
    });
    QVERIFY(b2Profile != profiles.cend());
    QVERIFY(b2Profile->html.contains(QStringLiteral("Application Keys")));
}

void GuiSmokeTest::mailPageShowsAccountsAndFolderCounts() {
    // The Mail page is where a connected mailbox becomes a schedulable job, so
    // it has to show the account and the per-folder counters it works from.
    mail::MailStore mailStore(context_->database());
    mail::MailAccount account;
    account.displayName = QStringLiteral("Test mailbox");
    account.tenantId = QStringLiteral("tenant");
    account.clientId = QStringLiteral("client");
    account.accountIdentifier = QStringLiteral("user@example.com");
    const Result<qint64> accountId = mailStore.upsertAccount(account);
    QVERIFY(accountId.hasValue());

    mail::MailFolderInfo inbox;
    inbox.id = QStringLiteral("inbox");
    inbox.displayName = QStringLiteral("Inbox");
    inbox.path = QStringLiteral("Inbox");
    inbox.totalItemCount = 42;
    QVERIFY(mailStore.storeFolders(accountId.value(), {inbox}).ok());

    MainWindow window(*context_);
    window.show();

    int mailIndex = -1;
    for (int index = 0; index < window.pageCount(); ++index) {
        if (window.pageTitleAt(index) == QStringLiteral("Mail")) {
            mailIndex = index;
            break;
        }
    }
    QVERIFY2(mailIndex >= 0, "the Mail page must exist in the sidebar");
    window.selectPage(mailIndex);
    QCoreApplication::processEvents();

    Page* page = window.pageAt(mailIndex);
    QVERIFY(page != nullptr);
    auto* accounts = page->findChild<QComboBox*>(QStringLiteral("mailAccount"));
    QVERIFY(accounts != nullptr);
    QVERIFY(accounts->count() >= 1);
    QVERIFY(accounts->itemText(0).contains(QStringLiteral("user@example.com")));

    auto* folders = page->findChild<QTableView*>(QStringLiteral("mailFolders"));
    QVERIFY(folders != nullptr);
    QVERIFY2(folders->model()->rowCount() == 1, "the folder table must list the stored folder");
    QCOMPARE(folders->model()->index(0, 0).data().toString(), QStringLiteral("Inbox"));
    QCOMPARE(folders->model()->index(0, 1).data().toString(), QStringLiteral("42"));

    // The Restic shortcut must exist: it is how the archive gets versioned,
    // encrypted copies rather than only a plain directory.
    bool hasResticAction = false;
    for (QPushButton* button : page->findChildren<QPushButton*>()) {
        if (button->text().contains(QStringLiteral("Restic"))) {
            hasResticAction = true;
            break;
        }
    }
    QVERIFY(hasResticAction);
}

void GuiSmokeTest::settingsPageOffersTheRcloneInstaller() {
    // The install button and the tool status must be reachable from the real
    // window: this is the control the user asked for, so its absence is a bug.
    MainWindow window(*context_);
    window.show();

    QPushButton* install = nullptr;
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
        if (button->text().contains(QStringLiteral("Rclone"))) {
            install = button;
            break;
        }
    }
    QVERIFY2(install != nullptr, "the Settings page must offer the Rclone installer");

    // The status label names both tools, so a missing tool is visible before the
    // user tries to use it.
    bool foundStatus = false;
    for (QLabel* label : window.findChildren<QLabel*>()) {
        const QString text = label->text();
        if (text.contains(QStringLiteral("rclone")) && text.contains(QStringLiteral("restic"))) {
            foundStatus = true;
            break;
        }
    }
    QVERIFY2(foundStatus, "the external tools card must report both tools");
}

void GuiSmokeTest::settingsPageOffersTrayAutostart() {
    // The tray application and the background agent are separate autostart
    // entries. The Settings page must expose the desktop one and keep the
    // agent's clearly labelled, otherwise "Install autostart" reads as if the
    // window would appear in the tray at the next logon.
    QVERIFY(ui::appAutostartCommand().contains(QStringLiteral("--tray")));

    MainWindow window(*context_);
    window.show();

    int settingsIndex = -1;
    for (int index = 0; index < window.pageCount(); ++index) {
        if (window.pageTitleAt(index) == QStringLiteral("Settings")) {
            settingsIndex = index;
            break;
        }
    }
    QVERIFY(settingsIndex >= 0);
    window.selectPage(settingsIndex);
    QCoreApplication::processEvents();

    Page* settings = window.pageAt(settingsIndex);
    QVERIFY(settings != nullptr);

    auto* startAtLogin = settings->findChild<QCheckBox*>(QStringLiteral("settingsStartAtLogin"));
    QVERIFY2(startAtLogin != nullptr, "Settings must offer starting Nesva in the tray at sign-in");
    QVERIFY(startAtLogin->text().contains(QStringLiteral("notification area")));

    auto* agentAutostart =
        settings->findChild<QLabel*>(QStringLiteral("settingsAgentAutostart"));
    QVERIFY2(agentAutostart != nullptr, "the agent autostart state must be labelled");
    QVERIFY(agentAutostart->text().startsWith(QStringLiteral("Agent autostart:")));

    bool installAgentAutostart = false;
    for (QPushButton* button : settings->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Install agent autostart")) {
            installAgentAutostart = true;
            break;
        }
    }
    QVERIFY2(installAgentAutostart, "the agent autostart button must name the agent");
}

void GuiSmokeTest::singleInstanceActivatesTheFirstInstance() {
    // A second launch must talk to the running process and exit instead of
    // creating another window, database connection and tray icon.
    ui::SingleInstance first;
    QVERIFY2(first.listen(), qPrintable(first.errorString()));
    QSignalSpy showSpy(&first, &ui::SingleInstance::showRequested);

    ui::SingleInstance second;
    QVERIFY(second.notifyExistingInstance());
    QTRY_COMPARE_WITH_TIMEOUT(showSpy.count(), 1, 5000);
}

void GuiSmokeTest::mainWindowExposesEveryPage() {
    MainWindow window(*context_);
    QCOMPARE(window.pageCount(), 9);

    const QStringList expected = {
        QStringLiteral("Dashboard"), QStringLiteral("Backup Jobs"),
        QStringLiteral("Storage"),   QStringLiteral("Archive Explorer"),
        QStringLiteral("Mail"),      QStringLiteral("Snapshots"),
        QStringLiteral("Restore"),   QStringLiteral("Activity"),
        QStringLiteral("Settings"),
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
    QCOMPARE(navigation->count(), 9);

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
    const Status started = context_->executor()->startRun(created.value(), false);
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
    QVERIFY(context_->executor()->startRun(created.value(), false).ok());

    // A second start while the first runs must be refused, not queued.
    const Status second = context_->executor()->startRun(created.value(), false);
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


void GuiSmokeTest::jobEditorCreatesBackupJobFromForm() {
    const QString sourceDirectory = directory_->filePath(QStringLiteral("backup-source"));
    QVERIFY(QDir().mkpath(sourceDirectory));
    const QString repository = directory_->filePath(QStringLiteral("restic-repo"));

    ui::JobEditorDialog dialog(*context_, std::nullopt);
    auto* mode = dialog.findChild<QComboBox*>(QStringLiteral("jobMode"));
    auto* nameEdit = dialog.findChild<QLineEdit*>(QStringLiteral("jobName"));
    auto* sources = dialog.findChild<QListWidget*>(QStringLiteral("jobSources"));
    auto* repositoryEdit = dialog.findChild<QLineEdit*>(QStringLiteral("jobRepository"));
    auto* keepLast = dialog.findChild<QSpinBox*>(QStringLiteral("jobKeepLast"));
    auto* keepDaily = dialog.findChild<QSpinBox*>(QStringLiteral("jobKeepDaily"));
    auto* checkAfter = dialog.findChild<QCheckBox*>(QStringLiteral("jobCheckAfterBackup"));
    QVERIFY(mode && nameEdit && sources && repositoryEdit && keepLast && keepDaily && checkAfter);

    QCOMPARE(mode->count(), 2);
    QCOMPARE(mode->itemData(1).toString(), QStringLiteral("backup"));

    mode->setCurrentIndex(1);
    nameEdit->setText(QStringLiteral("Nightly backup"));
    sources->addItem(sourceDirectory);
    repositoryEdit->setText(repository);
    keepLast->setValue(3);
    keepDaily->setValue(5);
    checkAfter->setChecked(true);

    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(dialog.savedJobId() > 0);

    jobs::JobStore store(context_->database());
    const Result<jobs::ArchiveJob> saved = store.getJob(dialog.savedJobId());
    QVERIFY(saved);
    QVERIFY(saved.value().isBackup());
    QCOMPARE(saved.value().destinationPath, repository);
    QCOMPARE(saved.value().retention.keepLast, 3);
    QCOMPARE(saved.value().retention.keepDaily, 5);
    QVERIFY(saved.value().checkAfterBackup);
}

void GuiSmokeTest::snapshotsPageListsRecordedSnapshots() {
    // No backup jobs yet: the page must say so instead of showing an empty table.
    ui::SnapshotsPage emptyPage(*context_);
    emptyPage.refresh();
    auto* emptyCombo = emptyPage.findChild<QComboBox*>(QStringLiteral("snapshotJob"));
    QVERIFY(emptyCombo != nullptr);
    QCOMPARE(emptyCombo->count(), 0);

    // Create a backup job and record a snapshot; the page lists it.
    jobs::JobStore store(context_->database());
    jobs::ArchiveJob job;
    job.name = QStringLiteral("Backup job");
    job.mode = QStringLiteral("backup");
    job.destinationPath = directory_->filePath(QStringLiteral("repo-for-page"));
    job.sources = {{0, directory_->path(), QStringLiteral("src")}};
    const Result<qint64> created = store.createJob(job);
    QVERIFY(created);

    jobs::JobStore::SnapshotRecord record;
    record.snapshotId = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    record.createdAt = QStringLiteral("2026-03-10T02:00:00.000Z");
    record.hostname = QStringLiteral("unit");
    record.sizeBytes = 4096;
    record.filesProcessed = 7;
    QVERIFY(store.recordSnapshot(created.value(), 0, record).ok());

    ui::SnapshotsPage page(*context_);
    page.refresh();
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("snapshotJob"));
    QVERIFY(combo != nullptr);
    QCOMPARE(combo->count(), 1);
    QCOMPARE(combo->currentText(), QStringLiteral("Backup job"));

    auto* view = page.findChild<QTableView*>();
    QVERIFY(view != nullptr);
    QVERIFY(view->model() != nullptr);
    QCOMPARE(view->model()->rowCount(), 1);
    QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("01234567"));
    QCOMPARE(view->model()->index(0, 3).data().toString(), QStringLiteral("7"));
}

void GuiSmokeTest::executorRunsBackupJob() {
    if (resolveExecutable(QStringLiteral("restic")).isEmpty()) {
        QSKIP("restic is not installed");
    }

    // Point the password source at a file so the run does not depend on a keyring.
    const QString passwordFile = directory_->filePath(QStringLiteral("backup-password"));
    QVERIFY(test::writeFile(passwordFile, QByteArray("gui-backup-password")));
    QVERIFY(QFile::setPermissions(passwordFile,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QVERIFY(context_->settings()
                .setString(QString::fromUtf8(config::keys::kBackupPasswordSource),
                           QStringLiteral("file"))
                .ok());
    QVERIFY(context_->settings()
                .setString(QString::fromUtf8(config::keys::kBackupPasswordFile), passwordFile)
                .ok());

    const QString sourceDirectory = directory_->filePath(QStringLiteral("exec-source"));
    const QString repository = directory_->filePath(QStringLiteral("exec-repo"));
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(test::writeFile(QDir(sourceDirectory).absoluteFilePath(QStringLiteral("a.txt")), "alpha"));

    jobs::JobStore store(context_->database());
    jobs::ArchiveJob job;
    job.name = QStringLiteral("GUI backup job");
    job.mode = QStringLiteral("backup");
    job.destinationPath = repository;
    job.sources = {{0, sourceDirectory, QStringLiteral("src")}};
    const Result<qint64> created = store.createJob(job);
    QVERIFY(created);

    // Initialise the repository with the same password contract the runner uses.
    const Result<QString> tempPassword = backup::createPasswordFile(QByteArray("gui-backup-password"));
    QVERIFY(tempPassword);
    backup::ResticRepository resticRepository(QStringLiteral("restic"), repository,
                                              tempPassword.value());
    QVERIFY2(resticRepository.init().ok(), "repository init");
    QFile::remove(tempPassword.value());

    MainWindow window(*context_);
    window.show();

    QSignalSpy finishedSpy(context_->executor(), &ui::JobExecutor::finished);
    const Status started = context_->executor()->startRun(created.value(), false);
    QVERIFY2(started.ok(), started.ok() ? "" : started.error().message().c_str());
    QTRY_VERIFY_WITH_TIMEOUT(finishedSpy.count() == 1, 60000);

    const auto report = finishedSpy.at(0).at(1).value<jobs::RunReport>();
    QCOMPARE(report.status, QStringLiteral("succeeded"));

    const Result<QVector<jobs::JobStore::SnapshotRecord>> snapshots =
        store.listSnapshots(created.value());
    QVERIFY(snapshots);
    QCOMPARE(snapshots.value().size(), 1);
    QVERIFY(!snapshots.value().constFirst().snapshotId.isEmpty());

    // Browse the snapshot through the real dialog: it reads the repository off
    // the GUI thread and reports what it found.
    ui::SnapshotBrowserDialog browser(*context_, store.getJob(created.value()).value(),
                                      snapshots.value().constFirst().snapshotId);
    QSignalSpy loadedSpy(&browser, &ui::SnapshotBrowserDialog::loaded);
    QTRY_VERIFY_WITH_TIMEOUT(loadedSpy.count() == 1, 60000);
    QVERIFY(browser.fileCount() >= 1);
    QCOMPARE(browser.loadError(), QString());
}

QTEST_MAIN(GuiSmokeTest)
#include "tst_gui_smoke.moc"
