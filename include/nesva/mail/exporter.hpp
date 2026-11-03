// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>
#include <memory>

#include "nesva/core/cancellation.hpp"
#include "nesva/core/result.hpp"
#include "nesva/mail/graph.hpp"

namespace nesva::db {
class Database;
}

namespace nesva::mail {

struct MailExportOptions {
    QString destinationDirectory;
    /// Empty means every folder with sync enabled.
    QStringList folderRemoteIds;
    /// Ignore the stored delta cursor and walk the folder from scratch.
    bool fullResync = false;
    /// Safety valves for the first live runs (0 = unlimited).
    int maxMessages = 0;
    int maxPagesPerFolder = 0;
};

struct MailExportReport {
    int folders = 0;
    int messagesSeen = 0;
    int exported = 0;
    int skipped = 0;
    int removed = 0;
    int failed = 0;
    qint64 bytesWritten = 0;
    QStringList errors;

    /// How much mail the run had in front of it, from Microsoft's own folder
    /// counters, and how long it took. Used for progress and for estimating the
    /// next run; never as a decision.
    qint64 totalMessages = 0;
    double elapsedSeconds = 0;
    /// True when only changes were fetched (every folder had a delta cursor),
    /// where a percentage of the whole mailbox would be meaningless.
    bool incremental = false;

    bool succeeded() const { return failed == 0; }
    double messagesPerSecond() const {
        return elapsedSeconds > 0 ? static_cast<double>(messagesSeen) / elapsedSeconds : 0.0;
    }
};

/// Live progress for a long first sync.
struct MailProgress {
    QString folder;
    int folderIndex = 0;
    int folderCount = 0;
    int messagesSeen = 0;
    int exported = 0;
    int failed = 0;

    /// Messages examined so far, and the size of the mailbox in scope (0 when
    /// the run is incremental, so no honest percentage exists).
    qint64 done = 0;
    qint64 total = 0;
    qint64 bytesWritten = 0;
    double messagesPerSecond = 0;
    qint64 etaSeconds = -1; ///< -1 when it cannot be estimated
    bool incremental = false;

    /// Percentage of the scope, or -1 when it is unknown.
    double percent() const {
        return total > 0 ? 100.0 * static_cast<double>(done) / static_cast<double>(total) : -1.0;
    }
};

/// Exports Microsoft 365 mail as individual .eml files.
///
/// Incremental by construction: each folder keeps a delta cursor in SQLite, so
/// an interrupted run resumes where it stopped and an unchanged message is not
/// downloaded again. Nothing is ever deleted from Microsoft 365, and a message
/// that disappears from the mailbox keeps its local copy (only its state is
/// recorded).
class MailExporter {
public:
    MailExporter(db::Database& database, std::shared_ptr<GraphClient> graph,
                 std::function<void(const MailProgress&)> progress = {});

    Result<MailExportReport> run(qint64 accountId, const MailExportOptions& options,
                                 const CancellationToken& cancellation);

    /// Replaces characters that are invalid on Windows/Unix and collapses
    /// whitespace; never returns an empty name.
    static QString sanitiseFileName(const QString& value, int maxLength = 100);

    /// Destination-relative path: folder/YYYY/MM/subject-id.eml
    static QString buildEmlRelativePath(const QString& folderPath,
                                        const QDateTime& receivedUtc,
                                        const QString& subject, const QString& messageKey);

private:
    db::Database& database_;
    std::shared_ptr<GraphClient> graph_;
    std::function<void(const MailProgress&)> progress_;
};

} // namespace nesva::mail
