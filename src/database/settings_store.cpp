// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/config/settings_store.hpp"

#include <QtCore/QVariant>

#include <utility>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include "nesva/db/database.hpp"

namespace nesva::config {

SettingsStore::SettingsStore(db::Database& database) : database_(database) {}

Result<QString> SettingsStore::getString(const QString& key, const QString& defaultValue) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral("SELECT value FROM settings WHERE key = ?"), {QVariant(key)});
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    if (!rows.next()) {
        return defaultValue;
    }
    return rows.value(0).toString();
}

Result<qint64> SettingsStore::getInteger(const QString& key, qint64 defaultValue) const {
    const Result<QString> value = getString(key);
    if (!value) {
        return value.error();
    }
    if (value.value().isEmpty()) {
        return defaultValue;
    }
    bool parsed = false;
    const qint64 number = value.value().toLongLong(&parsed);
    if (!parsed) {
        return defaultValue;
    }
    return number;
}

Result<bool> SettingsStore::getBoolean(const QString& key, bool defaultValue) const {
    const Result<QString> value = getString(key);
    if (!value) {
        return value.error();
    }
    const QString text = value.value().trimmed().toLower();
    if (text.isEmpty()) {
        return defaultValue;
    }
    if (text == QStringLiteral("1") || text == QStringLiteral("true") ||
        text == QStringLiteral("yes") || text == QStringLiteral("on")) {
        return true;
    }
    if (text == QStringLiteral("0") || text == QStringLiteral("false") ||
        text == QStringLiteral("no") || text == QStringLiteral("off")) {
        return false;
    }
    return defaultValue;
}

Status SettingsStore::setValueWithType(const QString& key, const QString& value,
                                       const QString& valueType) {
    if (key.trimmed().isEmpty()) {
        return makeError(ErrorCode::InvalidArgument, "setting key must not be empty");
    }

    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "INSERT INTO settings (key, value, value_type, updated_at) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, "
        "value_type = excluded.value_type, updated_at = excluded.updated_at"));
    query.addBindValue(key);
    query.addBindValue(value);
    query.addBindValue(valueType);
    query.addBindValue(db::Database::currentTimestampUtc());
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot write setting",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Status SettingsStore::setString(const QString& key, const QString& value) {
    return setValueWithType(key, value, QStringLiteral("string"));
}

Status SettingsStore::setInteger(const QString& key, qint64 value) {
    return setValueWithType(key, QString::number(value), QStringLiteral("integer"));
}

Status SettingsStore::setBoolean(const QString& key, bool value) {
    return setValueWithType(key, value ? QStringLiteral("true") : QStringLiteral("false"),
                            QStringLiteral("boolean"));
}

Result<bool> SettingsStore::contains(const QString& key) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral("SELECT 1 FROM settings WHERE key = ?"), {QVariant(key)});
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    return rows.next();
}

Status SettingsStore::remove(const QString& key) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral("DELETE FROM settings WHERE key = ?"));
    query.addBindValue(key);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot delete setting",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Result<QMap<QString, QString>> SettingsStore::all() const {
    Result<QSqlQuery> query =
        database_.select(QStringLiteral("SELECT key, value FROM settings ORDER BY key"));
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    QMap<QString, QString> values;
    while (rows.next()) {
        values.insert(rows.value(0).toString(), rows.value(1).toString());
    }
    return values;
}

} // namespace nesva::config
