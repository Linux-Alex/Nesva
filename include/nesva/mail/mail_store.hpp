// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>

#include <optional>

#include "nesva/core/result.hpp"
#include "nesva/mail/graph.hpp"

namespace nesva::db {
class Database;
}

namespace nesva::mail {

struct MailAccount {
    qint64 id = 0;
    QString displayName;
    QString tenantId;
    QString clientId;
    QString accountIdentifier; ///< user principal name
    QString scopes;
    QString status = QStringLiteral("disconnected");
    QString lastSyncAt;

    /// Measurements of the last completed sync. They are the only speed data
    /// available before a run starts, so the next run's duration can be
    /// estimated instead of guessed.
    qint64 lastSyncMessages = 0;
    qint64 lastSyncBytes = 0;
    double lastSyncSeconds = 0;

    bool hasSpeed() const { return lastSyncMessages > 0 && lastSyncSeconds > 0; }
    double messagesPerSecond() const {
        return hasSpeed() ? static_cast<double>(lastSyncMessages) / lastSyncSeconds : 0.0;
    }
    qint64 bytesPerMessage() const {
        return lastSyncMessages > 0 ? lastSyncBytes / lastSyncMessages : 0;
    }
};

/// How much mail a folder holds, and how much of it is already archived. The
/// total comes from Microsoft (`totalItemCount`) and can lag behind the server
/// by a little; it is used for progress, never as a correctness decision.
struct MailFolderStats {
    qint64 folderRowId = 0;
    QString displayName;
    qint64 totalItems = 0;
    qint64 exportedItems = 0;
    bool syncEnabled = true;
    bool hasCursor = false;

    qint64 remainingItems() const { return qMax<qint64>(0, totalItems - exportedItems); }
};

struct MailFolderRecord {
    qint64 id = 0;
    QString remoteId;
    QString displayName;
    QString parentRemoteId;
    QString deltaLink; ///< persisted delta cursor, the basis of incremental sync
    bool syncEnabled = true;
    qint64 totalItemCount = 0; ///< reported by Microsoft, for progress only
};

struct MailMessageRecord {
    qint64 id = 0;
    QString remoteId;
    QString internetMessageId;
    QString subject;
    QString sender;
    QString receivedAt;
    QString messageKey; ///< stable identity: internetMessageId, else remote id
    QString emlPath;
    QString emlSha256;
    QString exportState = QStringLiteral("pending");
    QString lastModifiedAt;
    qint64 sizeBytes = 0; ///< size of the archived .eml
};

/// Persistence for Microsoft 365 archival metadata.
///
/// Secrets never live here: the account row keeps tenant/client/UPN, and the
/// refresh token stays in the OS keyring (see mail::tokenReferenceFor).
class MailStore {
public:
    explicit MailStore(db::Database& database);

    Result<qint64> upsertAccount(const MailAccount& account);
    Result<QVector<MailAccount>> accounts() const;
    Result<MailAccount> account(qint64 id) const;
    Status updateAccountStatus(qint64 id, const QString& status, const QString& lastSyncAt = {});

    /// Stores/updates the folder tree of an account.
    Status storeFolders(qint64 accountId, const QVector<MailFolderInfo>& folders);
    Result<QVector<MailFolderRecord>> folders(qint64 accountId) const;

    /// Per-folder message counts: Microsoft's total and how many are archived.
    Result<QVector<MailFolderStats>> folderStats(qint64 accountId) const;

    Status setFolderSyncEnabled(qint64 folderRowId, bool enabled);
    Status setFolderDeltaLink(qint64 folderRowId, const QString& deltaLink);

    /// Records the speed of a finished sync (messages, bytes, wall time).
    Status recordSyncStats(qint64 accountId, qint64 messages, qint64 bytes, double seconds);

    Result<std::optional<MailMessageRecord>> findMessage(qint64 accountId,
                                                         const QString& remoteId) const;
    Status upsertMessage(qint64 accountId, qint64 folderRowId, const MailMessageRecord& message);
    Status setMessageState(qint64 accountId, const QString& remoteId, const QString& state,
                           const QString& lastModifiedAt = {});

    Result<QVector<MailMessageRecord>> messages(qint64 accountId, int limit = 500) const;

private:
    db::Database& database_;
};

} // namespace nesva::mail
