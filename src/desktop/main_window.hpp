// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QVector>
#include <QtGui/QIcon>
#include <QtWidgets/QMainWindow>

class QActionGroup;
class QDockWidget;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QSystemTrayIcon;
class QTableView;
class QTimer;

namespace nesva::ui {

class AppContext;
class EventListModel;
class Page;

/// Main application window: navigation sidebar, page stack, status bar,
/// optional activity dock and system tray integration.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    /// \param backgroundChecks false makes the constructor skip network work (the
    ///          release check, agent polling). The GUI self-test sets it: a test
    ///          must not depend on a network or wait for an HTTP timeout.
    explicit MainWindow(AppContext& context, bool backgroundChecks = true,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

    int pageCount() const { return static_cast<int>(pages_.size()); }
    QString pageTitleAt(int index) const;
    /// The page widget itself, for tests that need to reach real controls.
    Page* pageAt(int index) const;
    void selectPage(int index);

    /// True when a notification-area icon exists for this window.
    bool trayAvailable() const;

public slots:
    void refreshCurrentPage();
    void refreshAll();
    void applyTheme();

    /// Brings the window forward, whether it is hidden in the notification
    /// area, minimised or merely behind another window. Used by the tray menu
    /// and when a second launch asks the running instance to show itself.
    void showFromTray();

    /// Leaves the tray, saves the window state and exits. This is what both
    /// "Quit" actions use, so they work even while the window is hidden in the
    /// notification area.
    void requestQuit();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void createPages();
    void updateAgentIndicator();
    /// Reads the release service once per start (rate-limited) and notifies when a
    /// newer version is published. Enabled by default; see Settings -> Updates.
    void checkForUpdatesOnStart();


    void notify(const QString& title, const QString& message);
    bool notificationsEnabled() const;
    bool minimizeToTrayEnabled() const;
    void buildMenus();
    void buildStatusBar();
    void buildActivityPanel();
    void buildTray();
    void updateStatus();
    void updateNavigationIcons();
    void restoreWindowState();
    void saveWindowState();

    AppContext& context_;
    /// False in the GUI self-test: no release check, no network waits.
    bool backgroundChecks_ = true;
    QListWidget* navigation_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* applicationTitle_ = nullptr;
    QLabel* statusBadge_ = nullptr;
    QLabel* databaseStatus_ = nullptr;
    QLabel* schemaStatus_ = nullptr;
    QLabel* agentStatus_ = nullptr;
    QDockWidget* activityDock_ = nullptr;
    QTableView* activityView_ = nullptr;
    EventListModel* activityModel_ = nullptr;
    QSystemTrayIcon* trayIcon_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    bool quitting_ = false;
    QActionGroup* themeGroup_ = nullptr;
    QVector<Page*> pages_;
    QVector<QListWidgetItem*> navigationItems_;
};

} // namespace nesva::ui
