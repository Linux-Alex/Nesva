// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <functional>

#include "nesva/core/cancellation.hpp"
#include "nesva/jobs/archive_runner.hpp"
#include "nesva/jobs/job_store.hpp"

namespace nesva::db {
class Database;
}

namespace nesva::jobs {

/// Exports a Microsoft 365 mailbox into the job's destination directory as one
/// \c .eml file per message.
///
/// Archive semantics: the mailbox is only ever read, and each folder keeps its
/// delta cursor so every run after the first is incremental. Because this is an
/// ordinary job, scheduling, retries, run history, notifications and the
/// database-enforced single-run guard all apply without special cases.
class MailJobRunner {
public:
    MailJobRunner(db::Database& database, RunProgressCallback progress = {});

    /// Runs the job. Never throws; every failure is part of the report.
    RunReport run(const ArchiveJob& job, const RunOptions& options,
                  const CancellationToken& cancellation, const QString& trigger);

private:
    db::Database& database_;
    RunProgressCallback progress_;
};

} // namespace nesva::jobs
