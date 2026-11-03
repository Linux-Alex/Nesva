// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/mail/mail_store.hpp"

#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <utility>

#include "nesva/db/database.hpp"

namespace nesva::mail {

MailStore::MailStore(db::Database& database) : database_(database) {}

Result<qint64> MailStore::upsertAccount(const MailAccount& account) {
    if (account.accountIdentifier.trimmed().isEmpty()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "the account identifier is required");
    }
    const QString timestamp = db::Database::currentTimestampUtc();
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "INSERT INTO mail_accounts (display_name, tenant_id, client_id, account_identifier, "
        "scopes, status, last_sync_at, created_at, updated_at) "
        "VALUES (COALESCE(?, ''), COALESCE(?, ''), COALESCE(?, ''), ?, COALESCE(?, ''), "
        "COALESCE(?, ''), ?, ?, ?) "
        "ON CONFLICT(account_identifier) DO UPDATE SET display_name = excluded.display_name, "
        "tenant_id = excluded.tenant_id, client_id = excluded.client_id, "
        "scopes = excluded.scopes, status = excluded.status, updated_at = excluded.updated_at"));
    query.addBindValue(account.displayName);
    query.addBindValue(account.tenantId);
    query.addBindValue(account.clientId);
    query.addBindValue(account.accountIdentifier);
    query.addBindValue(account.scopes);
    query.addBindValue(account.status);
    query.addBindValue(account.lastSyncAt.isEmpty() ? QVariant() : QVariant(account.lastSyncAt));
    query.addBindValue(timestamp);
    query.addBindValue(timestamp);
    if (!query.exec()) {
        return makeErrorValue(ErrorCode::DatabaseError, "cannot store the mail account",
                              query.lastError().text().toStdString());
    }

    Result<QSqlQuery> lookup = database_.select(
        QStringLiteral("SELECT id FROM mail_accounts WHERE account_identifier = ?"),
        {QVariant(account.accountIdentifier)});
    if (!lookup) {
        return lookup.error();
    }
    QSqlQuery rows = std::move(lookup).take();
    if (!rows.next()) {
        return makeErrorValue(ErrorCode::DatabaseError, "the account row disappeared after upsert");
    }
    return rows.value(0).toLongLong();
}

Result<QVector<MailAccount>> MailStore::accounts() const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral("SELECT id, display_name, tenant_id, client_id, account_identifier, "
                       "scopes, status, COALESCE(last_sync_at, '') FROM mail_accounts "
                       "ORDER BY display_name"));
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    QVector<MailAccount> accounts;
    while (rows.next()) {
        MailAccount account;
        account.id = rows.value(0).toLongLong();
        account.displayName = rows.value(1).toString();
        account.tenantId = rows.value(2).toString();
        account.clientId = rows.value(3).toString();
        account.accountIdentifier = rows.value(4).toString();
        account.scopes = rows.value(5).toString();
        account.status = rows.value(6).toString();
        account.lastSyncAt = rows.value(7).toString();
        accounts.append(account);
    }
    return accounts;
}

Result<MailAccount> MailStore::account(qint64 id) const {
    Result<QVector<MailAccount>> all = accounts();
    if (!all) {
        return all.error();
    }
    for (const MailAccount& account : all.value()) {
        if (account.id == id) {
            return account;
        }
    }
    return makeErrorValue(ErrorCode::NotFound, "no such mail account", std::to_string(id));
}

Status MailStore::updateAccountStatus(qint64 id, const QString& status, const QString& lastSyncAt) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "UPDATE mail_accounts SET status = ?, last_sync_at = COALESCE(?, last_sync_at), "
        "updated_at = ? WHERE id = ?"));
    query.addBindValue(status);
    query.addBindValue(lastSyncAt.isEmpty() ? QVariant() : QVariant(lastSyncAt));
    query.addBindValue(db::Database::currentTimestampUtc());
    query.addBindValue(id);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot update the mail account",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Status MailStore::storeFolders(qint64 accountId, const QVector<MailFolderInfo>& folders) {
    const QString timestamp = db::Database::currentTimestampUtc();
    db::Transaction transaction(database_);
    for (const MailFolderInfo& folder : folders) {
        QSqlQuery query(database_.handle());
        query.prepare(QStringLiteral(
            "INSERT INTO mail_folders (mail_account_id, remote_folder_id, display_name, "
            "parent_remote_folder_id, sync_enabled, created_at) VALUES (?, ?, ?, ?, 1, ?) "
            "ON CONFLICT(mail_account_id, remote_folder_id) DO UPDATE SET "
            "display_name = excluded.display_name, "
            "parent_remote_folder_id = excluded.parent_remote_folder_id"));
        query.addBindValue(accountId);
        query.addBindValue(folder.id);
        query.addBindValue(folder.path.isEmpty() ? folder.displayName : folder.path);
        query.addBindValue(folder.parentId.isEmpty() ? QVariant() : QVariant(folder.parentId));
        query.addBindValue(timestamp);
        if (!query.exec()) {
            return makeError(ErrorCode::DatabaseError, "cannot store a mail folder",
                             query.lastError().text().toStdString());
        }
    }
    return transaction.commit();
}

Result<QVector<MailFolderRecord>> MailStore::folders(qint64 accountId) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral("SELECT id, remote_folder_id, display_name, "
                       "COALESCE(parent_remote_folder_id, ''), COALESCE(delta_link, ''), "
                       "sync_enabled FROM mail_folders WHERE mail_account_id = ? "
                       "ORDER BY display_name"),
        {QVariant(accountId)});
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    QVector<MailFolderRecord> folders;
    while (rows.next()) {
        MailFolderRecord folder;
        folder.id = rows.value(0).toLongLong();
        folder.remoteId = rows.value(1).toString();
        folder.displayName = rows.value(2).toString();
        folder.parentRemoteId = rows.value(3).toString();
        folder.deltaLink = rows.value(4).toString();
        folder.syncEnabled = rows.value(5).toBool();
        folders.append(folder);
    }
    return folders;
}

Status MailStore::setFolderSyncEnabled(qint64 folderRowId, bool enabled) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral("UPDATE mail_folders SET sync_enabled = ? WHERE id = ?"));
    query.addBindValue(enabled ? 1 : 0);
    query.addBindValue(folderRowId);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot update the folder",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Status MailStore::setFolderDeltaLink(qint64 folderRowId, const QString& deltaLink) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "UPDATE mail_folders SET delta_link = ?, last_sync_at = ? WHERE id = ?"));
    query.addBindValue(deltaLink);
    query.addBindValue(db::Database::currentTimestampUtc());
    query.addBindValue(folderRowId);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot store the delta cursor",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Result<std::optional<MailMessageRecord>> MailStore::findMessage(qint64 accountId,
                                                               const QString& remoteId) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral("SELECT id, remote_message_id, COALESCE(internet_message_id, ''), "
                       "COALESCE(subject, ''), COALESCE(sender, ''), COALESCE(received_at, ''), "
                       "message_key, COALESCE(eml_path, ''), COALESCE(eml_sha256, ''), "
                       "export_state, COALESCE(last_modified_at, '') FROM mail_messages "
                       "WHERE mail_account_id = ? AND remote_message_id = ?"),
        {QVariant(accountId), QVariant(remoteId)});
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    if (!rows.next()) {
        return std::optional<MailMessageRecord>{};
    }
    MailMessageRecord message;
    message.id = rows.value(0).toLongLong();
    message.remoteId = rows.value(1).toString();
    message.internetMessageId = rows.value(2).toString();
    message.subject = rows.value(3).toString();
    message.sender = rows.value(4).toString();
    message.receivedAt = rows.value(5).toString();
    message.messageKey = rows.value(6).toString();
    message.emlPath = rows.value(7).toString();
    message.emlSha256 = rows.value(8).toString();
    message.exportState = rows.value(9).toString();
    message.lastModifiedAt = rows.value(10).toString();
    return std::optional<MailMessageRecord>(message);
}

Status MailStore::upsertMessage(qint64 accountId, qint64 folderRowId,
                                const MailMessageRecord& message) {
    const QString timestamp = db::Database::currentTimestampUtc();
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "INSERT INTO mail_messages (mail_account_id, mail_folder_id, remote_message_id, "
        "internet_message_id, subject, sender, received_at, message_key, eml_path, eml_sha256, "
        "export_state, last_modified_at, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(mail_account_id, remote_message_id) DO UPDATE SET "
        "mail_folder_id = excluded.mail_folder_id, "
        "internet_message_id = excluded.internet_message_id, subject = excluded.subject, "
        "sender = excluded.sender, received_at = excluded.received_at, "
        "message_key = excluded.message_key, eml_path = excluded.eml_path, "
        "eml_sha256 = excluded.eml_sha256, export_state = excluded.export_state, "
        "last_modified_at = excluded.last_modified_at, updated_at = excluded.updated_at"));
    query.addBindValue(accountId);
    query.addBindValue(folderRowId > 0 ? folderRowId : QVariant());
    query.addBindValue(message.remoteId);
    query.addBindValue(message.internetMessageId.isEmpty() ? QVariant()
                                                           : QVariant(message.internetMessageId));
    query.addBindValue(message.subject);
    query.addBindValue(message.sender);
    query.addBindValue(message.receivedAt.isEmpty() ? QVariant() : QVariant(message.receivedAt));
    query.addBindValue(message.messageKey);
    query.addBindValue(message.emlPath.isEmpty() ? QVariant() : QVariant(message.emlPath));
    query.addBindValue(message.emlSha256.isEmpty() ? QVariant() : QVariant(message.emlSha256));
    query.addBindValue(message.exportState);
    query.addBindValue(message.lastModifiedAt.isEmpty() ? QVariant()
                                                        : QVariant(message.lastModifiedAt));
    query.addBindValue(timestamp);
    query.addBindValue(timestamp);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot store the message metadata",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Status MailStore::setMessageState(qint64 accountId, const QString& remoteId,
                                  const QString& state, const QString& lastModifiedAt) {
    QSqlQuery query(database_.handle());
    query.prepare(QStringLiteral(
        "UPDATE mail_messages SET export_state = ?, last_modified_at = COALESCE(?, "
        "last_modified_at), updated_at = ? WHERE mail_account_id = ? AND remote_message_id = ?"));
    query.addBindValue(state);
    query.addBindValue(lastModifiedAt.isEmpty() ? QVariant() : QVariant(lastModifiedAt));
    query.addBindValue(db::Database::currentTimestampUtc());
    query.addBindValue(accountId);
    query.addBindValue(remoteId);
    if (!query.exec()) {
        return makeError(ErrorCode::DatabaseError, "cannot update the message state",
                         query.lastError().text().toStdString());
    }
    return Status();
}

Result<QVector<MailMessageRecord>> MailStore::messages(qint64 accountId, int limit) const {
    Result<QSqlQuery> query = database_.select(
        QStringLiteral("SELECT id, remote_message_id, COALESCE(internet_message_id, ''), "
                       "COALESCE(subject, ''), COALESCE(sender, ''), COALESCE(received_at, ''), "
                       "message_key, COALESCE(eml_path, ''), COALESCE(eml_sha256, ''), "
                       "export_state, COALESCE(last_modified_at, '') FROM mail_messages "
                       "WHERE mail_account_id = ? ORDER BY received_at DESC LIMIT ?"),
        {QVariant(accountId), QVariant(limit)});
    if (!query) {
        return query.error();
    }
    QSqlQuery rows = std::move(query).take();
    QVector<MailMessageRecord> messages;
    while (rows.next()) {
        MailMessageRecord message;
        message.id = rows.value(0).toLongLong();
        message.remoteId = rows.value(1).toString();
        message.internetMessageId = rows.value(2).toString();
        message.subject = rows.value(3).toString();
        message.sender = rows.value(4).toString();
        message.receivedAt = rows.value(5).toString();
        message.messageKey = rows.value(6).toString();
        message.emlPath = rows.value(7).toString();
        message.emlSha256 = rows.value(8).toString();
        message.exportState = rows.value(9).toString();
        message.lastModifiedAt = rows.value(10).toString();
        messages.append(message);
    }
    return messages;
}

} // namespace nesva::mail
