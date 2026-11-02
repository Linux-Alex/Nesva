// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/scheduler/schedule.hpp"

#include <QtCore/QLocale>

namespace nesva::sched {
namespace {

constexpr int kMaxRetryBackoffShift = 6; // cap the exponential growth

QString weekdayName(int isoDay) {
    switch (isoDay) {
    case 1:
        return QStringLiteral("Monday");
    case 2:
        return QStringLiteral("Tuesday");
    case 3:
        return QStringLiteral("Wednesday");
    case 4:
        return QStringLiteral("Thursday");
    case 5:
        return QStringLiteral("Friday");
    case 6:
        return QStringLiteral("Saturday");
    case 7:
        return QStringLiteral("Sunday");
    default:
        return QStringLiteral("Monday");
    }
}

/// Builds a wall-clock time in \p zone, moving forward out of a DST gap.
QDateTime atLocalTime(const QDate& date, const QTime& time, const QTimeZone& zone) {
    QDateTime candidate(date, time, zone);
    if (candidate.isValid()) {
        return candidate;
    }
    // Spring-forward gap: the wall-clock time does not exist that day.
    for (int hours = 1; hours <= 3; ++hours) {
        candidate = QDateTime(date, time.addSecs(hours * 3600), zone);
        if (candidate.isValid()) {
            return candidate;
        }
    }
    return QDateTime();
}

} // namespace

QString toToken(TriggerKind kind) {
    switch (kind) {
    case TriggerKind::Daily:
        return QStringLiteral("daily");
    case TriggerKind::Weekly:
        return QStringLiteral("weekly");
    case TriggerKind::Monthly:
        return QStringLiteral("monthly");
    case TriggerKind::OnLogin:
        return QStringLiteral("on_login");
    case TriggerKind::OnStartup:
        return QStringLiteral("on_startup");
    case TriggerKind::Manual:
        break;
    }
    return QStringLiteral("manual");
}

std::optional<TriggerKind> triggerKindFromString(const QString& value) {
    const QString normalised = value.trimmed().toLower();
    if (normalised == QStringLiteral("daily")) {
        return TriggerKind::Daily;
    }
    if (normalised == QStringLiteral("weekly")) {
        return TriggerKind::Weekly;
    }
    if (normalised == QStringLiteral("monthly")) {
        return TriggerKind::Monthly;
    }
    if (normalised == QStringLiteral("on_login")) {
        return TriggerKind::OnLogin;
    }
    if (normalised == QStringLiteral("on_startup")) {
        return TriggerKind::OnStartup;
    }
    if (normalised == QStringLiteral("manual")) {
        return TriggerKind::Manual;
    }
    return std::nullopt;
}

QString toToken(DueReason reason) {
    switch (reason) {
    case DueReason::Scheduled:
        return QStringLiteral("scheduled");
    case DueReason::Missed:
        return QStringLiteral("missed");
    case DueReason::Retry:
        return QStringLiteral("retry");
    case DueReason::NotDue:
        break;
    }
    return QStringLiteral("not_due");
}

std::optional<QString> ScheduleSpec::validate() const {
    if (!QTimeZone(timezone).isValid()) {
        return QStringLiteral("unknown timezone: %1").arg(QString::fromLatin1(timezone));
    }
    if (!timeOfDay.isValid()) {
        return QStringLiteral("invalid time of day");
    }
    if (dayOfWeek < 1 || dayOfWeek > 7) {
        return QStringLiteral("day of week must be between 1 (Monday) and 7 (Sunday)");
    }
    if (dayOfMonth < 1 || dayOfMonth > 28) {
        return QStringLiteral("day of month must be between 1 and 28");
    }
    if (maxDurationSeconds < 0) {
        return QStringLiteral("maximum duration must not be negative");
    }
    if (retryMaxAttempts < 0 || retryMaxAttempts > 10) {
        return QStringLiteral("retry attempts must be between 0 and 10");
    }
    if (retryBackoffSeconds < 0) {
        return QStringLiteral("retry backoff must not be negative");
    }
    return std::nullopt;
}

QTimeZone ScheduleSpec::zone() const {
    const QByteArray id = timezone.isEmpty() ? QByteArrayLiteral("UTC") : timezone;
    QTimeZone zone(id);
    return zone.isValid() ? zone : QTimeZone::systemTimeZone();
}

QDateTime computeNextRun(const ScheduleSpec& spec, const QDateTime& afterUtc) {
    if (!spec.isRecurring()) {
        return QDateTime();
    }
    const QTimeZone zone = spec.zone();
    const QDateTime after = afterUtc.toUTC();

    for (int dayOffset = 0; dayOffset <= 400; ++dayOffset) {
        const QDateTime local = after.toTimeZone(zone).addDays(dayOffset);
        const QDate date = local.date();

        if (spec.kind == TriggerKind::Weekly && date.dayOfWeek() != spec.dayOfWeek) {
            continue;
        }
        if (spec.kind == TriggerKind::Monthly && date.day() != spec.dayOfMonth) {
            continue;
        }

        const QDateTime candidate = atLocalTime(date, spec.timeOfDay, zone);
        if (!candidate.isValid()) {
            continue;
        }
        if (candidate.toUTC() > after) {
            return candidate.toUTC();
        }
    }
    return QDateTime();
}

DueDecision evaluateDue(const ScheduleSpec& spec, const ScheduleState& state,
                        const QDateTime& nowUtc, const QDateTime& agentStartedUtc,
                        int maxCatchUpAgeHours) {
    DueDecision decision;
    const QDateTime now = nowUtc.toUTC();
    const QDateTime started = agentStartedUtc.toUTC();

    // A pending retry takes precedence over the regular slot.
    if (state.retryAttemptCount > 0 && state.retryNextUtc && *state.retryNextUtc <= now) {
        decision.reason = DueReason::Retry;
        decision.retryAttempt = state.retryAttemptCount + 1;
        decision.nextRunUtc = computeNextRun(spec, now);
        decision.explanation =
            QStringLiteral("retry %1 of %2").arg(decision.retryAttempt).arg(spec.retryMaxAttempts + 1);
        return decision;
    }

    // Startup/login triggers fire once per agent session: they run when the
    // agent starts, not on every tick.
    if (spec.kind == TriggerKind::OnStartup || spec.kind == TriggerKind::OnLogin) {
        const bool alreadyRan = state.lastRunUtc.has_value() && *state.lastRunUtc >= started;
        decision.nextRunUtc = QDateTime();
        if (!alreadyRan) {
            decision.reason = DueReason::Scheduled;
            decision.explanation = QStringLiteral("agent start trigger");
        }
        return decision;
    }

    if (!spec.isRecurring()) {
        decision.nextRunUtc = QDateTime();
        return decision;
    }

    QDateTime slot = state.nextRunUtc.value_or(QDateTime());
    if (!slot.isValid()) {
        // First evaluation: schedule from now, nothing is due yet.
        decision.nextRunUtc = computeNextRun(spec, now);
        decision.explanation = QStringLiteral("first evaluation");
        return decision;
    }

    slot = slot.toUTC();
    if (slot > now) {
        decision.nextRunUtc = slot;
        return decision;
    }

    // The slot is in the past.
    if (slot < started) {
        // It passed before this agent started: a missed run.
        const int missedAgeHours = static_cast<int>(slot.secsTo(started) / 3600);
        if (!spec.catchUpMissed) {
            decision.nextRunUtc = computeNextRun(spec, now);
            decision.explanation = QStringLiteral(
                "missed slot %1 skipped because catch-up is disabled")
                                       .arg(slot.toString(Qt::ISODate));
            return decision;
        }
        if (missedAgeHours > maxCatchUpAgeHours) {
            decision.nextRunUtc = computeNextRun(spec, now);
            decision.explanation = QStringLiteral("missed slot %1 is too old to catch up")
                                       .arg(slot.toString(Qt::ISODate));
            return decision;
        }
        decision.reason = DueReason::Missed;
        decision.nextRunUtc = computeNextRun(spec, now);
        decision.explanation =
            QStringLiteral("catching up missed slot %1").arg(slot.toString(Qt::ISODate));
        return decision;
    }

    // Normal due slot while the agent was running.
    decision.reason = DueReason::Scheduled;
    decision.nextRunUtc = computeNextRun(spec, now);
    decision.explanation = QStringLiteral("scheduled slot %1").arg(slot.toString(Qt::ISODate));
    return decision;
}

OutcomePlan planAfterRun(const ScheduleSpec& spec, const QDateTime& nowUtc, bool succeeded,
                         int attemptsUsedSoFar) {
    OutcomePlan plan;
    plan.nextRunUtc = computeNextRun(spec, nowUtc);

    if (succeeded) {
        // A successful run clears the retry state.
        plan.retryAttemptCount = 0;
        return plan;
    }

    const int used = attemptsUsedSoFar < 0 ? 0 : attemptsUsedSoFar;
    if (used >= spec.retryMaxAttempts) {
        plan.retryAttemptCount = used;
        return plan;
    }

    // Geometric backoff, capped so a misconfigured job cannot wait forever.
    const int shift = used < kMaxRetryBackoffShift ? used : kMaxRetryBackoffShift;
    const qint64 delaySeconds =
        static_cast<qint64>(spec.retryBackoffSeconds) * (1LL << shift);
    plan.retryAttemptCount = used + 1;
    plan.willRetry = true;
    plan.retryNextUtc = nowUtc.toUTC().addSecs(delaySeconds);
    return plan;
}

QString describe(const ScheduleSpec& spec) {
    switch (spec.kind) {
    case TriggerKind::Manual:
        return QStringLiteral("Manual only");
    case TriggerKind::Daily:
        return QStringLiteral("Daily at %1 (%2)")
            .arg(QLocale().toString(spec.timeOfDay, QLocale::ShortFormat),
                 QString::fromLatin1(spec.timezone));
    case TriggerKind::Weekly:
        return QStringLiteral("Every %1 at %2 (%3)")
            .arg(weekdayName(spec.dayOfWeek), QLocale().toString(spec.timeOfDay, QLocale::ShortFormat),
                 QString::fromLatin1(spec.timezone));
    case TriggerKind::Monthly:
        return QStringLiteral("Monthly on day %1 at %2 (%3)")
            .arg(spec.dayOfMonth)
            .arg(QLocale().toString(spec.timeOfDay, QLocale::ShortFormat),
                 QString::fromLatin1(spec.timezone));
    case TriggerKind::OnLogin:
        return QStringLiteral("When the user logs in");
    case TriggerKind::OnStartup:
        return QStringLiteral("When the agent starts");
    }
    return QStringLiteral("Manual only");
}

} // namespace nesva::sched
