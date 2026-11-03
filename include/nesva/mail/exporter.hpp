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

    bool succeeded() const { return failed == 0; }
};

struct MailProgress {
    QString folder;
    int messagesSeen = 0;
    int exported = 0;
    int failed = 0;
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
