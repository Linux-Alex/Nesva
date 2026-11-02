// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QTime>
#include <QtCore/QTimeZone>

#include <optional>

namespace nesva::sched {

/// How a job is triggered.
enum class TriggerKind {
    Manual,     ///< Only when a user starts it.
    Daily,
    Weekly,
    Monthly,
    OnLogin,    ///< When the user logs into the desktop session.
    OnStartup,  ///< When the agent (or system) starts.
};

QString toToken(TriggerKind kind);
std::optional<TriggerKind> triggerKindFromString(const QString& value);

/// A stored schedule. Times are local to \ref timezone; storage is UTC.
struct ScheduleSpec {
    TriggerKind kind = TriggerKind::Manual;
    QTime timeOfDay{2, 30};
    /// ISO day of week, Monday = 1 ... Sunday = 7.
    int dayOfWeek = 1;
    /// 1..28 (kept below 29 so every month has the day).
    int dayOfMonth = 1;
    /// IANA timezone identifier, for example "Europe/Ljubljana".
    QByteArray timezone{"UTC"};
    /// Cancel the run after this many seconds; 0 means no limit.
    int maxDurationSeconds = 0;
    /// Extra attempts after a failed run.
    int retryMaxAttempts = 0;
    /// Delay before a retry; multiplied by 2 for each further attempt.
    int retryBackoffSeconds = 60;
    /// Run once if the agent was not running when the slot passed.
    bool catchUpMissed = true;

    bool isRecurring() const {
        return kind == TriggerKind::Daily || kind == TriggerKind::Weekly ||
               kind == TriggerKind::Monthly;
    }

    /// Validates ranges and the timezone identifier.
    std::optional<QString> validate() const;

    QTimeZone zone() const;
};

/// Next occurrence strictly after \p afterUtc, or an invalid QDateTime for
/// non-recurring schedules.
///
/// The computation is timezone-aware: a slot that would fall into a
/// daylight-saving gap is moved forward to the next valid wall-clock time
/// within the same day, and a repeated hour never produces two runs.
QDateTime computeNextRun(const ScheduleSpec& spec, const QDateTime& afterUtc);

/// Why a job is being started, used for the run's trigger column and logs.
enum class DueReason {
    NotDue,
    Scheduled,
    Missed,   ///< The slot passed while the agent was not running.
    Retry,    ///< The previous attempt failed and the retry is due.
};

QString toToken(DueReason reason);

/// Persistent bits of schedule state loaded from the database.
struct ScheduleState {
    std::optional<QDateTime> nextRunUtc;
    std::optional<QDateTime> lastRunUtc;
    int retryAttemptCount = 0;
    std::optional<QDateTime> retryNextUtc;
};

struct DueDecision {
    DueReason reason = DueReason::NotDue;
    /// New value to persist for next_run_at (may be empty to clear it).
    QDateTime nextRunUtc;
    /// Attempt number to report when this is a retry.
    int retryAttempt = 0;
    QString explanation;

    bool shouldRun() const { return reason != DueReason::NotDue; }
};

/// Decides whether a job must run now.
///
/// \param nowUtc        current time (UTC)
/// \param agentStartedUtc when this agent process started; a slot that passed
///        before that moment counts as missed
/// \param maxCatchUpAgeHours a missed slot older than this is skipped instead of
///        run, so a machine that was off for a month does not start a burst
DueDecision evaluateDue(const ScheduleSpec& spec, const ScheduleState& state,
                        const QDateTime& nowUtc, const QDateTime& agentStartedUtc,
                        int maxCatchUpAgeHours = 168);

/// Computes the state after a run finished.
/// On success the retry counter resets; on failure (when retries remain) a
/// retry time is produced instead of the next scheduled slot.
struct OutcomePlan {
    QDateTime nextRunUtc;
    int retryAttemptCount = 0;
    std::optional<QDateTime> retryNextUtc;
    bool willRetry = false;
};

/// param attemptsUsedSoFar retries already consumed for this job (0 on a
///        first failure), so the backoff grows geometrically.
OutcomePlan planAfterRun(const ScheduleSpec& spec, const QDateTime& nowUtc, bool succeeded,
                         int attemptsUsedSoFar = 0);

/// Human readable summary, used by the schedule editor and CLI.
QString describe(const ScheduleSpec& spec);

} // namespace nesva::sched
