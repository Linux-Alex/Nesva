// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "settings_page.hpp"

#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QVBoxLayout>

#include "app_context.hpp"
#include "dialogs/tool_install_dialog.hpp"
#include "nesva/agent/client.hpp"
#include "nesva/core/external_tool.hpp"
#include "nesva/core/paths.hpp"

namespace nesva::ui {
namespace {

QFrame* makeCard(QWidget* parent, const QString& title, QVBoxLayout*& cardLayout) {
    auto* card = new QFrame(parent);
    card->setObjectName(QStringLiteral("statCard"));
    cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(16, 12, 16, 16);
    cardLayout->setSpacing(10);

    auto* titleLabel = new QLabel(title, card);
    titleLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
    cardLayout->addWidget(titleLabel);
    return card;
}

} // namespace

SettingsPage::SettingsPage(AppContext& context, QWidget* parent)
    : Page(QStringLiteral("Settings"),
           QStringLiteral("Application preferences. Credentials are never stored here; keyring "
                          "integration arrives with the storage and mail modules."),
           parent),
      context_(context) {
    buildAppearanceSection();
    buildLoggingSection();
    buildSafetySection();
    buildBehaviourSection();
    buildToolsSection();
    buildAgentSection();
    buildLocationsSection();

    savedLabel_ = new QLabel(this);
    savedLabel_->setObjectName(QStringLiteral("pageSubtitle"));
    savedLabel_->setVisible(false);
    body()->addWidget(savedLabel_);
    body()->addStretch(1);
}

void SettingsPage::buildAppearanceSection() {
    QVBoxLayout* cardLayout = nullptr;
    QFrame* card = makeCard(this, QStringLiteral("Appearance"), cardLayout);

    auto* form = new QFormLayout();
    themeCombo_ = new QComboBox(card);
    themeCombo_->addItem(QStringLiteral("Follow system"), QStringLiteral("system"));
    themeCombo_->addItem(QStringLiteral("Light"), QStringLiteral("light"));
    themeCombo_->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
    form->addRow(QStringLiteral("Theme"), themeCombo_);
    cardLayout->addLayout(form);

    connect(themeCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QString mode = themeCombo_->currentData().toString();
        if (const auto parsed = themeModeFromString(mode)) {
            const Status status = context_.setThemeMode(*parsed);
            if (status) {
                showSaved();
            }
        }
    });

    body()->addWidget(card);
}

void SettingsPage::buildLoggingSection() {
    QVBoxLayout* cardLayout = nullptr;
    QFrame* card = makeCard(this, QStringLiteral("Logging"), cardLayout);

    auto* form = new QFormLayout();
    logLevelCombo_ = new QComboBox(card);
    for (const LogLevel level :
         {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error}) {
        logLevelCombo_->addItem(toDisplayString(level), QString::fromUtf8(toToken(level)));
    }
    form->addRow(QStringLiteral("Minimum level"), logLevelCombo_);
    cardLayout->addLayout(form);

    auto* hint = new QLabel(
        QStringLiteral("Applies immediately to new records written by this window. The log file "
                       "keeps structured JSON lines and is rotated automatically."),
        card);
    hint->setObjectName(QStringLiteral("pageSubtitle"));
    hint->setWordWrap(true);
    cardLayout->addWidget(hint);

    connect(logLevelCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        const QString value = logLevelCombo_->currentData().toString();
        if (const auto parsed = logLevelFromString(value.toStdString())) {
            const Status stored = context_.settings().setString(
                QString::fromUtf8(config::keys::kLogLevel), value);
            if (stored) {
                globalLogger().setMinLevel(*parsed);
                showSaved();
            }
        }
    });

    body()->addWidget(card);
}

void SettingsPage::buildSafetySection() {
    QVBoxLayout* cardLayout = nullptr;
    QFrame* card = makeCard(this, QStringLiteral("Safety"), cardLayout);

    confirmDestructive_ = new QCheckBox(
        QStringLiteral("Ask for confirmation before destructive actions"), card);
    cardLayout->addWidget(confirmDestructive_);

    auto* hint = new QLabel(
        QStringLiteral("Archive jobs always use copy semantics: source files are never deleted "
                       "and remote deletion requires an explicit per-job setting plus confirmation."),
        card);
    hint->setObjectName(QStringLiteral("pageSubtitle"));
    hint->setWordWrap(true);
    cardLayout->addWidget(hint);

    connect(confirmDestructive_, &QCheckBox::toggled, this, [this](bool checked) {
        const Status stored = context_.settings().setBoolean(
            QString::fromUtf8(config::keys::kConfirmDestructiveActions), checked);
        if (stored) {
            showSaved();
        }
    });

    body()->addWidget(card);
}

void SettingsPage::buildLocationsSection() {
    QVBoxLayout* cardLayout = nullptr;
    QFrame* card = makeCard(this, QStringLiteral("Locations"), cardLayout);

    auto* form = new QFormLayout();
    dataRootValue_ = new QLabel(card);
    dataRootValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    databaseValue_ = new QLabel(card);
    databaseValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    logFileValue_ = new QLabel(card);
    logFileValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("Data directory"), dataRootValue_);
    form->addRow(QStringLiteral("Database"), databaseValue_);
    form->addRow(QStringLiteral("Log file"), logFileValue_);
    cardLayout->addLayout(form);

    auto* buttons = new QHBoxLayout();
    auto* openData = new QPushButton(QStringLiteral("Open data folder"), card);
    auto* openLogs = new QPushButton(QStringLiteral("Open log folder"), card);
    buttons->addWidget(openData);
    buttons->addWidget(openLogs);
    buttons->addStretch(1);
    cardLayout->addLayout(buttons);

    connect(openData, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(Paths::dataRoot()));
    });
    connect(openLogs, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(Paths::logsDirectory()));
    });

    body()->addWidget(card);
}

void SettingsPage::showSaved() {
    if (savedLabel_ == nullptr) {
        return;
    }
    savedLabel_->setText(QStringLiteral("Preferences saved."));
    savedLabel_->setVisible(true);
    QTimer::singleShot(2500, savedLabel_, [this] { savedLabel_->setVisible(false); });
}

void SettingsPage::refresh() {
    const QSignalBlocker themeBlocker(themeCombo_);
    const QSignalBlocker levelBlocker(logLevelCombo_);
    const QSignalBlocker confirmBlocker(confirmDestructive_);
    const QSignalBlocker minimizeBlocker(minimizeToTrayCheck_);
    const QSignalBlocker notificationsBlocker(notificationsCheck_);

    const Result<QString> theme =
        context_.settings().getString(QString::fromUtf8(config::keys::kThemeMode),
                                      QStringLiteral("system"));
    const QString themeValue = theme ? theme.value() : QStringLiteral("system");
    const int themeIndex = themeCombo_->findData(themeValue);
    themeCombo_->setCurrentIndex(themeIndex >= 0 ? themeIndex : 0);

    const Result<QString> level =
        context_.settings().getString(QString::fromUtf8(config::keys::kLogLevel),
                                      QStringLiteral("info"));
    const int levelIndex = logLevelCombo_->findData(level ? level.value() : QStringLiteral("info"));
    logLevelCombo_->setCurrentIndex(levelIndex >= 0 ? levelIndex : 2);

    const Result<bool> confirm = context_.settings().getBoolean(
        QString::fromUtf8(config::keys::kConfirmDestructiveActions), true);
    confirmDestructive_->setChecked(confirm ? confirm.value() : true);

    const Result<int> schemaVersion = context_.database().schemaVersion();

    const Result<bool> minimize = context_.settings().getBoolean(
        QString::fromUtf8(config::keys::kMinimizeToTray), false);
    minimizeToTrayCheck_->setChecked(minimize && minimize.value());
    const Result<bool> notifications = context_.settings().getBoolean(
        QString::fromUtf8(config::keys::kNotificationsEnabled), true);
    notificationsCheck_->setChecked(notifications && notifications.value());
    refreshAgentStatus();

    dataRootValue_->setText(Paths::describeResolution());
    databaseValue_->setText(QStringLiteral("%1 (schema %2 of %3)")
                                .arg(context_.database().filePath())
                                .arg(schemaVersion ? schemaVersion.value() : -1)
                                .arg(db::Database::latestSchemaVersion()));
    logFileValue_->setText(context_.logFilePath());
}


void SettingsPage::buildBehaviourSection() {
    QVBoxLayout* cardLayout = nullptr;
    QFrame* card = makeCard(this, QStringLiteral("Window behaviour"), cardLayout);

    minimizeToTrayCheck_ = new QCheckBox(
        QStringLiteral("Keep running in the tray when the window is closed"), card);
    notificationsCheck_ = new QCheckBox(QStringLiteral("Show desktop notifications for runs"), card);
    cardLayout->addWidget(minimizeToTrayCheck_);
    cardLayout->addWidget(notificationsCheck_);

    auto* hint = new QLabel(
        QStringLiteral("Scheduled runs are executed by the background agent, not by this window. "
                       "Closing the window or the tray icon does not stop scheduled backups."),
        card);
    hint->setObjectName(QStringLiteral("pageSubtitle"));
    hint->setWordWrap(true);
    cardLayout->addWidget(hint);

    connect(minimizeToTrayCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (context_.settings()
                .setBoolean(QString::fromUtf8(config::keys::kMinimizeToTray), checked)
                .ok()) {
            showSaved();
        }
    });
    connect(notificationsCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (context_.settings()
                .setBoolean(QString::fromUtf8(config::keys::kNotificationsEnabled), checked)
                .ok()) {
            showSaved();
        }
    });

    body()->addWidget(card);
}

void SettingsPage::buildToolsSection() {
    QVBoxLayout* cardLayout = nullptr;
    QFrame* card = makeCard(this, QStringLiteral("External tools"), cardLayout);

    toolsStatusValue_ = new QLabel(card);
    toolsStatusValue_->setObjectName(QStringLiteral("pageSubtitle"));
    toolsStatusValue_->setWordWrap(true);
    toolsStatusValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cardLayout->addWidget(toolsStatusValue_);

    auto* hint = new QLabel(
        QStringLiteral("Nesva calls <b>rclone</b> for cloud destinations and <b>restic</b> for "
                       "versioned backups, and never bundles either one. The installer downloads "
                       "the release over HTTPS, verifies its published SHA-256 and copies a single "
                       "binary: no shell, and nothing runs with elevated privileges unless you "
                       "choose an all-users install and confirm the step."),
        card);
    hint->setObjectName(QStringLiteral("pageSubtitle"));
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::RichText);
    cardLayout->addWidget(hint);

    auto* buttons = new QHBoxLayout();
    auto* refreshButton = new QPushButton(QStringLiteral("Check again"), card);
    auto* installButton = new QPushButton(QStringLiteral("Install Rclone..."), card);
    buttons->addWidget(refreshButton);
    buttons->addWidget(installButton);
    buttons->addStretch(1);
    cardLayout->addLayout(buttons);

    connect(refreshButton, &QPushButton::clicked, this, [this] { refreshToolsStatus(); });
    connect(installButton, &QPushButton::clicked, this, [this] {
        ToolInstallDialog dialog(context_, this);
        dialog.exec();
        if (!dialog.installedPath().isEmpty()) {
            refreshToolsStatus();
        }
    });

    body()->addWidget(card);
    refreshToolsStatus();
}

void SettingsPage::refreshToolsStatus() {
    if (toolsStatusValue_ == nullptr) {
        return;
    }
    QStringList lines;
    const struct {
        const char* key;
        const char* name;
    } tools[] = {{config::keys::kRcloneBinaryPath, "rclone"},
                 {config::keys::kResticBinaryPath, "restic"}};
    for (const auto& tool : tools) {
        const QString configured =
            context_.settings()
                .getString(QString::fromUtf8(tool.key), QString::fromUtf8(tool.name))
                .valueOr(QString::fromUtf8(tool.name));
        const Result<ExternalToolInfo> probe =
            probeExternalTool(configured, {QStringLiteral("version")}, 8000);
        if (probe) {
            lines.append(QStringLiteral("<b>%1</b> %2 &mdash; %3")
                             .arg(QString::fromUtf8(tool.name), probe.value().versionText,
                                  probe.value().executable.toHtmlEscaped()));
        } else {
            lines.append(QStringLiteral("<b>%1</b> &mdash; not installed")
                             .arg(QString::fromUtf8(tool.name)));
        }
    }
    toolsStatusValue_->setText(lines.join(QStringLiteral("<br>")));
    toolsStatusValue_->setTextFormat(Qt::RichText);
}

void SettingsPage::buildAgentSection() {
    QVBoxLayout* cardLayout = nullptr;
    QFrame* card = makeCard(this, QStringLiteral("Background agent"), cardLayout);

    agentStatusValue_ = new QLabel(card);
    agentStatusValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    agentStatusValue_->setWordWrap(true);
    cardLayout->addWidget(agentStatusValue_);

    auto* hint = new QLabel(
        QStringLiteral("The agent (<code>nesva-agent</code>) runs scheduled jobs without this "
                       "window. Install it as a systemd user service, or start it from a terminal "
                       "with <code>nesva-agent</code>. See docs/developer-guide/agent.md."),
        card);
    hint->setObjectName(QStringLiteral("pageSubtitle"));
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::RichText);
    cardLayout->addWidget(hint);

    auto* buttons = new QHBoxLayout();
    auto* refreshButton = new QPushButton(QStringLiteral("Refresh status"), card);
    auto* stopButton = new QPushButton(QStringLiteral("Stop agent"), card);
    buttons->addWidget(refreshButton);
    buttons->addWidget(stopButton);
    buttons->addStretch(1);
    cardLayout->addLayout(buttons);

    connect(refreshButton, &QPushButton::clicked, this, [this] { refreshAgentStatus(); });
    connect(stopButton, &QPushButton::clicked, this, [this] {
        agent::AgentClient client;
        const Status stopped = client.shutdown();
        if (!stopped.ok()) {
            QMessageBox::information(this, QStringLiteral("Background agent"),
                                     QString::fromStdString(stopped.error().toDisplayString()));
        } else {
            QMessageBox::information(this, QStringLiteral("Background agent"),
                                     QStringLiteral("The agent is shutting down."));
        }
        refreshAgentStatus();
    });

    body()->addWidget(card);
}

void SettingsPage::refreshAgentStatus() {
    if (agentStatusValue_ == nullptr) {
        return;
    }
    agent::AgentClient client;
    Result<agent::AgentStatus> status = client.status(600);
    if (status) {
        const agent::AgentStatus& state = status.value();
        agentStatusValue_->setText(
            QStringLiteral("Running (PID %1, %2) - %3 scheduled job(s), next run %4")
                .arg(state.pid)
                .arg(state.state)
                .arg(state.scheduledJobs)
                .arg(state.nextRunUtc.isValid()
                         ? state.nextRunUtc.toLocalTime().toString(Qt::ISODate)
                         : QStringLiteral("-")));
    } else {
        const agent::AgentStatus heartbeat = agent::AgentClient::lastKnownStatus();
        agentStatusValue_->setText(
            QStringLiteral("Not running. Last known state: %1 (%2). Start it with "
                           "nesva-agent or the systemd user service.")
                .arg(heartbeat.state, heartbeat.updatedAtUtc.toLocalTime().toString(Qt::ISODate)));
    }
}


} // namespace nesva::ui
