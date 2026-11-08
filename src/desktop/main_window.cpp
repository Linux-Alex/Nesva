// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "main_window.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include "nesva/config/settings_store.hpp"
#include "nesva/core/app_info.hpp"
#include "nesva/core/http.hpp"
#include "nesva/core/updater.hpp"
#include <QtGui/QCloseEvent>
#include <QtGui/QShortcut>
#include <QtGui/QActionGroup>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QTableView>
#include <QtCore/QTimer>
#include <QtSql/QSqlQuery>
#include <QtWidgets/QVBoxLayout>

#include "app_context.hpp"
#include "icon_factory.hpp"
#include "job_executor.hpp"
#include "nesva/agent/client.hpp"
#include "models/event_list_model.hpp"
#include "nesva/core/app_info.hpp"
#include "nesva/core/paths.hpp"
#include "pages/activity_page.hpp"
#include "pages/archive_page.hpp"
#include "pages/dashboard_page.hpp"
#include "pages/jobs_page.hpp"
#include "pages/mail_page.hpp"
#include "pages/page.hpp"
#include "pages/placeholder_page.hpp"
#include "pages/restore_page.hpp"
#include "pages/settings_page.hpp"
#include "pages/snapshots_page.hpp"
#include "pages/storage_page.hpp"
#include "theme.hpp"

namespace nesva::ui {
namespace {

constexpr int kStatusRefreshIntervalMs = 60000;
constexpr int kRunRefreshIntervalMs = 5000; ///< while a job is running

QIcon tintedGlyph(Glyph glyph, const QPalette& palette) {
    QColor color = palette.color(QPalette::WindowText);
    color.setAlpha(220);
    return makeGlyphIcon(glyph, color);
}

} // namespace

MainWindow::MainWindow(AppContext& context, bool backgroundChecks, QWidget* parent)
    : QMainWindow(parent), context_(context), backgroundChecks_(backgroundChecks) {
    setWindowTitle(QString::fromUtf8(kApplicationName));
    setWindowIcon(applicationIcon());
    setMinimumSize(940, 620);
    resize(1200, 780);

    buildUi();
    buildActivityPanel();
    buildMenus();
    buildStatusBar();
    buildTray();
    restoreWindowState();
    if (backgroundChecks_) {
        checkForUpdatesOnStart();
    }

    refreshAll();

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(kStatusRefreshIntervalMs);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        updateStatus();
        refreshCurrentPage();
    });
    refreshTimer_->start();

    connect(&context_, &AppContext::themeModeChanged, this, &MainWindow::applyTheme);

    if (JobExecutor* executor = context_.executor()) {
        connect(executor, &JobExecutor::started, this, [this](qint64, const QString& jobName) {
            if (statusBadge_ != nullptr) {
                statusBadge_->setText(QStringLiteral("Running: %1").arg(jobName));
            }
        });
        connect(executor, &JobExecutor::finished, this,
                [this](qint64, const jobs::RunReport& report) {
                    updateStatus();
                    refreshAll();
                    if (report.dryRun) {
                        return;
                    }
                    if (report.succeeded()) {
                        notify(QStringLiteral("Nesva: archive finished"),
                               QStringLiteral("%1: %2 file(s), %3 transferred")
                                   .arg(report.jobName)
                                   .arg(report.counters.filesTotal)
                                   .arg(report.counters.filesTransferred));
                    } else {
                        notify(QStringLiteral("Nesva: archive needs attention"),
                               QStringLiteral("%1 ended as %2 (%3 file(s) failed)")
                                   .arg(report.jobName, report.status)
                                   .arg(report.counters.filesFailed));
                    }
                });
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* topBar = new QFrame(central);
    topBar->setObjectName(QStringLiteral("topBar"));
    topBar->setFixedHeight(54);
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(20, 8, 20, 8);
    topLayout->setSpacing(12);

    applicationTitle_ = new QLabel(ApplicationInfo::displayVersion(), topBar);
    applicationTitle_->setObjectName(QStringLiteral("applicationTitle"));
    topLayout->addWidget(applicationTitle_);
    topLayout->addStretch(1);

    statusBadge_ = new QLabel(topBar);
    statusBadge_->setObjectName(QStringLiteral("statusBadge"));
    topLayout->addWidget(statusBadge_);

    auto* content = new QWidget(central);
    auto* contentLayout = new QHBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    auto* sidebar = new QFrame(content);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(212);
    auto* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(8, 10, 8, 10);
    sidebarLayout->setSpacing(0);

    navigation_ = new QListWidget(sidebar);
    navigation_->setObjectName(QStringLiteral("navigation"));
    navigation_->setFrameShape(QFrame::NoFrame);
    navigation_->setIconSize(QSize(18, 18));
    navigation_->setSpacing(2);
    navigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation_->setUniformItemSizes(true);
    sidebarLayout->addWidget(navigation_, 1);
    contentLayout->addWidget(sidebar);

    stack_ = new QStackedWidget(content);
    contentLayout->addWidget(stack_, 1);

    rootLayout->addWidget(topBar);
    rootLayout->addWidget(content, 1);
    setCentralWidget(central);

    createPages();

    connect(navigation_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= pages_.size()) {
            return;
        }
        stack_->setCurrentIndex(row);
        pages_.at(row)->refresh();
    });

    // Keyboard navigation: Ctrl+1 .. Ctrl+8 jump straight to a page.
    for (int index = 0; index < pages_.size() && index < 9; ++index) {
        auto* shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(index + 1)), this);
        connect(shortcut, &QShortcut::activated, this, [this, index] { selectPage(index); });
    }
}

void MainWindow::createPages() {
    struct NavEntry {
        QString label;
        Glyph glyph;
    };

    const QVector<NavEntry> entries = {
        {QStringLiteral("Dashboard"), Glyph::Dashboard},
        {QStringLiteral("Backup Jobs"), Glyph::Jobs},
        {QStringLiteral("Storage"), Glyph::Storage},
        {QStringLiteral("Archive Explorer"), Glyph::Archive},
        {QStringLiteral("Mail"), Glyph::Mail},
        {QStringLiteral("Snapshots"), Glyph::Snapshots},
        {QStringLiteral("Restore"), Glyph::Restore},
        {QStringLiteral("Activity"), Glyph::Activity},
        {QStringLiteral("Settings"), Glyph::Settings},
    };

    pages_.append(new DashboardPage(context_, this));
    pages_.append(new JobsPage(context_, this));
    pages_.append(new StoragePage(context_, this));
    pages_.append(new ArchivePage(context_, this));
    pages_.append(new MailPage(context_, this));
    pages_.append(new SnapshotsPage(context_, this));
    pages_.append(new RestorePage(context_, this));
    pages_.append(new ActivityPage(context_, this));
    pages_.append(new SettingsPage(context_, this));

    for (int index = 0; index < pages_.size(); ++index) {
        stack_->addWidget(pages_.at(index));
        auto* item = new QListWidgetItem(entries.at(index).label, navigation_);
        item->setData(Qt::UserRole, index);
        navigationItems_.append(item);
    }

    updateNavigationIcons();
    if (!pages_.isEmpty()) {
        navigation_->setCurrentRow(0);
    }
}

void MainWindow::updateNavigationIcons() {
    const QPalette pal = palette();
    for (int index = 0; index < navigationItems_.size(); ++index) {
        navigationItems_.at(index)->setIcon(tintedGlyph(static_cast<Glyph>(index), pal));
    }
}

void MainWindow::buildActivityPanel() {
    activityDock_ = new QDockWidget(QStringLiteral("Activity"), this);
    activityDock_->setObjectName(QStringLiteral("activityDock"));
    activityDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);

    activityModel_ = new EventListModel(context_.database(), 200, this);
    activityView_ = new QTableView(activityDock_);
    activityView_->setModel(activityModel_);
    activityView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    activityView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    activityView_->setAlternatingRowColors(true);
    activityView_->verticalHeader()->setVisible(false);
    activityView_->horizontalHeader()->setStretchLastSection(true);
    activityView_->horizontalHeader()->setSectionResizeMode(EventListModel::TimeColumn,
                                                            QHeaderView::ResizeToContents);
    activityView_->horizontalHeader()->setSectionResizeMode(EventListModel::LevelColumn,
                                                            QHeaderView::ResizeToContents);
    activityView_->horizontalHeader()->setSectionResizeMode(EventListModel::SourceColumn,
                                                            QHeaderView::ResizeToContents);
    activityDock_->setWidget(activityView_);
    addDockWidget(Qt::BottomDockWidgetArea, activityDock_);
    activityDock_->hide();
    resizeDocks({activityDock_}, {220}, Qt::Vertical);
}

void MainWindow::buildMenus() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* quitAction = fileMenu->addAction(QStringLiteral("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &MainWindow::requestQuit);

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    QMenu* themeMenu = viewMenu->addMenu(QStringLiteral("&Theme"));
    themeGroup_ = new QActionGroup(this);
    themeGroup_->setExclusive(true);

    struct ThemeEntry {
        QString label;
        ThemeMode mode;
    };
    const QVector<ThemeEntry> themes = {{QStringLiteral("Follow system"), ThemeMode::System},
                                        {QStringLiteral("Light"), ThemeMode::Light},
                                        {QStringLiteral("Dark"), ThemeMode::Dark}};
    for (const ThemeEntry& entry : themes) {
        QAction* action = themeMenu->addAction(entry.label);
        action->setCheckable(true);
        action->setData(static_cast<int>(entry.mode));
        themeGroup_->addAction(action);
        if (entry.mode == context_.themeMode()) {
            action->setChecked(true);
        }
        const ThemeMode mode = entry.mode;
        connect(action, &QAction::triggered, this, [this, mode] { context_.setThemeMode(mode); });
    }

    viewMenu->addSeparator();
    QAction* activityToggle = activityDock_->toggleViewAction();
    activityToggle->setText(QStringLiteral("Show &activity panel"));
    viewMenu->addAction(activityToggle);
    viewMenu->addSeparator();
    QAction* refreshAction = viewMenu->addAction(QStringLiteral("&Refresh"));
    refreshAction->setShortcut(QKeySequence::Refresh);
    connect(refreshAction, &QAction::triggered, this, &MainWindow::refreshAll);

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("&About Nesva"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, QStringLiteral("About Nesva"),
            QStringLiteral("<h3>%1</h3>"
                           "<p>Cross-platform desktop backup and archival application.</p>"
                           "<p>Commit: %2<br/>Qt: %3<br/>Platform: %4</p>"
                           "<p>Data directory: %5<br/>Database: %6<br/>Log file: %7</p>")
                .arg(ApplicationInfo::displayVersion(), ApplicationInfo::gitCommit(),
                     ApplicationInfo::qtVersion(), ApplicationInfo::platformDescription(),
                     Paths::dataRoot(), context_.database().filePath(), context_.logFilePath()));
    });
    QAction* aboutQtAction = helpMenu->addAction(QStringLiteral("About &Qt"));
    connect(aboutQtAction, &QAction::triggered, this, [this] { QMessageBox::aboutQt(this); });
}

void MainWindow::buildStatusBar() {
    databaseStatus_ = new QLabel(this);
    databaseStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addWidget(databaseStatus_);

    schemaStatus_ = new QLabel(this);
    statusBar()->addPermanentWidget(schemaStatus_);

    agentStatus_ = new QLabel(this);
    agentStatus_->setObjectName(QStringLiteral("agentStatus"));
    agentStatus_->setToolTip(QStringLiteral(
        "Background agent state. Start it with \"nesva-agent\" or enable the systemd user service."));
    statusBar()->addPermanentWidget(agentStatus_);

    updateStatus();
}

void MainWindow::buildTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }
    trayIcon_ = new QSystemTrayIcon(applicationIcon(), this);
    trayIcon_->setToolTip(QString::fromUtf8(kApplicationName));

    auto* trayMenu = new QMenu(this);
    QAction* showAction = trayMenu->addAction(QStringLiteral("Show Nesva"));
    connect(showAction, &QAction::triggered, this, &MainWindow::showFromTray);
    trayMenu->addSeparator();
    QAction* quitAction = trayMenu->addAction(QStringLiteral("Quit"));
    connect(quitAction, &QAction::triggered, this, &MainWindow::requestQuit);
    trayIcon_->setContextMenu(trayMenu);

    connect(trayIcon_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                    showFromTray();
                }
            });
    trayIcon_->show();
}

bool MainWindow::trayAvailable() const {
    return trayIcon_ != nullptr && trayIcon_->isVisible();
}

void MainWindow::showFromTray() {
    if (isMinimized()) {
        showNormal();
    } else {
        show();
    }
    raise();
    activateWindow();
    // A background process (a second launch) may not be allowed to take the
    // foreground; flashing the taskbar button still tells the user where the
    // window is.
    QApplication::alert(this, 0);
}

void MainWindow::requestQuit() {
    quitting_ = true;
    saveWindowState();
    if (trayIcon_ != nullptr) {
        trayIcon_->hide();
    }
    close();
    // A window that was never shown (--tray) has no visible top-level window to
    // close, so the lastWindowClosed path is not guaranteed. End the event loop
    // explicitly; the normal closeEvent cleanup above has already run.
    QCoreApplication::quit();
}

bool MainWindow::notificationsEnabled() const {
    const Result<bool> enabled =
        context_.settings().getBoolean(QString::fromUtf8(config::keys::kNotificationsEnabled), true);
    return enabled && enabled.value();
}

bool MainWindow::minimizeToTrayEnabled() const {
    const Result<bool> enabled =
        context_.settings().getBoolean(QString::fromUtf8(config::keys::kMinimizeToTray), false);
    return enabled && enabled.value();
}

void MainWindow::notify(const QString& title, const QString& message) {
    if (!notificationsEnabled() || trayIcon_ == nullptr || !trayIcon_->isVisible()) {
        return;
    }
    trayIcon_->showMessage(title, message, QSystemTrayIcon::Information, 6000);
}

void MainWindow::checkForUpdatesOnStart() {
    if (!backgroundChecks_) {
        return;
    }
    config::SettingsStore settings(context_.database());
    const bool enabled =
        settings.getBoolean(QString::fromUtf8(config::keys::kUpdateCheckOnStart), true)
            .valueOr(true);
    if (!enabled) {
        return;
    }

    // At most one automatic check every six hours, so restarting Nesva does not
    // hammer the release service (which rate-limits anonymous clients).
    const QString lastCheck =
        settings.getString(QString::fromUtf8(config::keys::kUpdateLastCheckUtc), QString())
            .valueOr(QString());
    const QDateTime lastCheckUtc = QDateTime::fromString(lastCheck, Qt::ISODate);
    if (lastCheckUtc.isValid() &&
        lastCheckUtc.secsTo(QDateTime::currentDateTimeUtc()) < 6 * 3600) {
        return;
    }

    const QString currentVersion = ApplicationInfo::version();
    auto* watcher = new QFutureWatcher<Result<update::UpdateCheck>>(this);
    connect(watcher, &QFutureWatcher<Result<update::UpdateCheck>>::finished, this,
            [this, watcher, currentVersion] {
                const Result<update::UpdateCheck> result = watcher->result();
                watcher->deleteLater();
                config::SettingsStore store(context_.database());
                store.setString(QString::fromUtf8(config::keys::kUpdateLastCheckUtc),
                                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
                if (!result) {
                    return;  // an offline start is not an error worth reporting
                }
                const update::UpdateCheck& check = result.value();
                if (!check.newer) {
                    return;
                }
                store.setString(QString::fromUtf8(config::keys::kUpdateAvailableVersion),
                                check.release.version);
                notify(QStringLiteral("Nesva %1 is available").arg(check.release.version),
                       QStringLiteral("This installation runs %1. Open Settings -> Updates to "
                                      "install it, or let the weekly update do it on Sunday at "
                                      "18:00.")
                           .arg(currentVersion));
            });
    watcher->setFuture(QtConcurrent::run([currentVersion]() -> Result<update::UpdateCheck> {
        net::QNetworkHttpTransport transport;
        return update::checkForUpdate(transport, currentVersion);
    }));
}

void MainWindow::updateAgentIndicator() {
    if (agentStatus_ == nullptr) {
        return;
    }
    // The heartbeat file is cheap; only talk to the socket when it claims that
    // an agent is running, so a stopped agent never costs a timeout.
    agent::AgentStatus heartbeat = agent::AgentClient::lastKnownStatus();
    agent::AgentStatus status = heartbeat;
    bool connected = false;
    if (heartbeat.running) {
        agent::AgentClient client;
        Result<agent::AgentStatus> live = client.status(600);
        if (live) {
            status = live.value();
            connected = true;
        }
    }

    if (connected) {
        QString text = QStringLiteral("Agent: %1").arg(status.state);
        if (status.runningJobId > 0 && !status.runningJobName.isEmpty()) {
            text += QStringLiteral(" (%1)").arg(status.runningJobName);
        }
        agentStatus_->setText(text);
        agentStatus_->setToolTip(QStringLiteral(
            "Scheduled jobs: %1\nNext run: %2\nLast: %3")
                                     .arg(status.scheduledJobs)
                                     .arg(status.nextRunUtc.isValid()
                                              ? status.nextRunUtc.toLocalTime().toString(Qt::ISODate)
                                              : QStringLiteral("-"),
                                          status.lastMessage));
    } else {
        agentStatus_->setText(QStringLiteral("Agent: not running"));
    }
}

void MainWindow::updateStatus() {
    if (statusBadge_ != nullptr) {
        statusBadge_->setText(context_.healthSummary());
    }
    // Tick faster while a run is in progress: a minute between progress updates
    // makes a running job look frozen, which is exactly what the user cannot
    // distinguish from a hang.
    if (refreshTimer_ != nullptr) {
        bool running = false;
        const bool own = context_.executor() != nullptr && context_.executor()->isRunning();
        if (own) {
            running = true;
        } else {
            Result<QSqlQuery> query = context_.database().select(QStringLiteral(
                "SELECT 1 FROM job_runs WHERE status = 'running' LIMIT 1"));
            if (query) {
                QSqlQuery rows = std::move(query).take();
                running = rows.next();
            }
        }
        const int wanted = running ? kRunRefreshIntervalMs : kStatusRefreshIntervalMs;
        if (refreshTimer_->interval() != wanted) {
            refreshTimer_->setInterval(wanted);
        }
    }
    updateAgentIndicator();
    if (schemaStatus_ != nullptr) {
        const Result<int> schema = context_.database().schemaVersion();
        schemaStatus_->setText(QStringLiteral("Schema %1 / %2")
                                   .arg(schema ? schema.value() : -1)
                                   .arg(db::Database::latestSchemaVersion()));
    }
    if (databaseStatus_ != nullptr) {
        databaseStatus_->setText(QStringLiteral("Database: %1").arg(context_.database().filePath()));
        databaseStatus_->setToolTip(QStringLiteral("Log file: %1").arg(context_.logFilePath()));
    }
    if (activityModel_ != nullptr) {
        activityModel_->refresh();
    }
}

void MainWindow::applyTheme() {
    if (qApp != nullptr) {
        ::nesva::ui::applyTheme(*qApp, context_.themeMode());
    }
    updateNavigationIcons();

    if (themeGroup_ != nullptr) {
        for (QAction* action : themeGroup_->actions()) {
            action->setChecked(action->data().toInt() == static_cast<int>(context_.themeMode()));
        }
    }
    updateStatus();
}

void MainWindow::restoreWindowState() {
    const Result<QString> geometry = context_.settings().getString(
        QString::fromUtf8(config::keys::kUiWindowGeometry));
    if (geometry && !geometry.value().isEmpty()) {
        restoreGeometry(QByteArray::fromBase64(geometry.value().toLatin1()));
    }

    const Result<bool> showPanel = context_.settings().getBoolean(
        QString::fromUtf8(config::keys::kUiShowActivityPanel), false);
    if (showPanel && showPanel.value() && activityDock_ != nullptr) {
        activityDock_->show();
    }
}

void MainWindow::saveWindowState() {
    const QString geometry =
        QString::fromLatin1(saveGeometry().toBase64());
    context_.settings().setString(QString::fromUtf8(config::keys::kUiWindowGeometry), geometry);
    context_.settings().setBoolean(QString::fromUtf8(config::keys::kUiShowActivityPanel),
                                   activityDock_ != nullptr && activityDock_->isVisible());
}

void MainWindow::refreshAll() {
    for (Page* page : pages_) {
        page->refresh();
    }
    updateStatus();
}

void MainWindow::refreshCurrentPage() {
    const int index = stack_->currentIndex();
    if (index >= 0 && index < pages_.size()) {
        pages_.at(index)->refresh();
    }
}

void MainWindow::selectPage(int index) {
    if (index < 0 || index >= pages_.size()) {
        return;
    }
    navigation_->setCurrentRow(index);
    stack_->setCurrentIndex(index);
}

QString MainWindow::pageTitleAt(int index) const {
    if (index < 0 || index >= pages_.size()) {
        return QString();
    }
    return pages_.at(index)->pageTitle();
}

Page* MainWindow::pageAt(int index) const {
    if (index < 0 || index >= pages_.size()) {
        return nullptr;
    }
    return pages_.at(index);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Minimising to the tray keeps the window (and its in-process runs) alive;
    // the background agent is what guarantees unattended scheduling.
    if (!quitting_ && minimizeToTrayEnabled() && trayIcon_ != nullptr &&
        trayIcon_->isVisible() && !context_.executor()->isRunning()) {
        hide();
        event->ignore();
        notify(QStringLiteral("Nesva is still running"),
               QStringLiteral("Nesva continues in the tray. Quit from the tray menu to exit."));
        context_.recordEvent(LogLevel::Info, QStringLiteral("application"),
                            QStringLiteral("main window hidden to the tray"));
        return;
    }

    saveWindowState();
    context_.recordEvent(LogLevel::Info, QStringLiteral("application"),
                         QStringLiteral("main window closed"));
    globalLogger().flush();
    QMainWindow::closeEvent(event);
}

} // namespace nesva::ui
