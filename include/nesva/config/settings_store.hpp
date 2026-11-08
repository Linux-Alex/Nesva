// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QMap>
#include <QtCore/QString>

#include "nesva/core/result.hpp"

namespace nesva::db {
class Database;
}

namespace nesva::config {

/// Typed key/value access to the "settings" table.
///
/// Settings are read on demand (no caching) so that the GUI, the CLI and the
/// background agent always observe the same values. Values that fail to parse
/// fall back to the caller-supplied default and are reported as warnings by the
/// caller, never silently rewritten.
class SettingsStore {
public:
    explicit SettingsStore(db::Database& database);

    Result<QString> getString(const QString& key, const QString& defaultValue = {}) const;
    Result<qint64> getInteger(const QString& key, qint64 defaultValue = 0) const;
    Result<bool> getBoolean(const QString& key, bool defaultValue = false) const;

    Status setString(const QString& key, const QString& value);
    Status setInteger(const QString& key, qint64 value);
    Status setBoolean(const QString& key, bool value);

    Result<bool> contains(const QString& key) const;
    Status remove(const QString& key);

    /// All settings as string values; typed ordering is lost, so this is meant
    /// for diagnostics and export only.
    Result<QMap<QString, QString>> all() const;

private:
    Status setValueWithType(const QString& key, const QString& value, const QString& valueType);

    db::Database& database_;
};

/// Well-known setting keys. Using constants prevents typo-driven divergence
/// between the GUI, the CLI and the agent.
namespace keys {
inline constexpr const char* kThemeMode = "ui.theme";
inline constexpr const char* kUiShowActivityPanel = "ui.activity_panel";
inline constexpr const char* kUiWindowGeometry = "ui.window_geometry";
inline constexpr const char* kLogLevel = "logging.level";
inline constexpr const char* kConfirmDestructiveActions = "safety.confirm_destructive_actions";
inline constexpr const char* kRcloneBinaryPath = "tools.rclone_path";
inline constexpr const char* kResticBinaryPath = "tools.restic_path";
inline constexpr const char* kNetworkChecksEnabled = "scheduling.network_checks";
inline constexpr const char* kMinimizeToTray = "ui.minimize_to_tray";
inline constexpr const char* kNotificationsEnabled = "ui.notifications";
inline constexpr const char* kBackupPasswordSource = "backup.password_source";
inline constexpr const char* kBackupPasswordFile = "backup.password_file";
/// Timezone used to display timestamps; empty means "follow the system".
inline constexpr const char* kDisplayTimezone = "ui.timezone";
/// Entra ID application registration used to sign in to Microsoft 365.
inline constexpr const char* kMailTenantId = "mail.tenant_id";
inline constexpr const char* kMailClientId = "mail.client_id";
/// Default directory for archived .eml files.
inline constexpr const char* kMailDestination = "mail.destination";
/// Check the published release when the window starts (default: on).
inline constexpr const char* kUpdateCheckOnStart = "update.check_on_start";
/// Install the weekly update unattended (default: on, Sunday 18:00 local).
inline constexpr const char* kUpdateWeeklyAutoUpdate = "update.weekly_auto_update";
/// ISO-8601 UTC timestamp of the last release check.
inline constexpr const char* kUpdateLastCheckUtc = "update.last_check_utc";
/// ISO-8601 UTC timestamp of the last update attempt.
inline constexpr const char* kUpdateLastInstallUtc = "update.last_install_utc";
/// Version the last check found available; empty when the installation is current.
inline constexpr const char* kUpdateAvailableVersion = "update.available_version";
} // namespace keys

} // namespace nesva::config
