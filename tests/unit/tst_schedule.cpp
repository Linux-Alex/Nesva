// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include "nesva/scheduler/schedule.hpp"

using namespace nesva::sched;

namespace {

QDateTime utc(const QString& iso) {
    return QDateTime::fromString(iso, Qt::ISODate).toUTC();
}

ScheduleSpec daily(const QTime& time, const QByteArray& zone = QByteArrayLiteral("UTC")) {
    ScheduleSpec spec;
    spec.kind = TriggerKind::Daily;
    spec.timeOfDay = time;
    spec.timezone = zone;
    return spec;
}

} // namespace

class ScheduleTest : public QObject {
    Q_OBJECT

private slots:
    void triggerTokensRoundTrip();
    void dailyComputesNextOccurrence();
    void dailyRollsToNextDayWhenTimePassed();
    void weeklyMatchesWeekday();
    void weeklyRollsToNextWeek();
    void monthlyMatchesDay();
    void monthlyRollsToNextMonth();
    void recurringRespectsTimezone();
    void daylightSavingGapIsHandled();
    void manualScheduleHasNoNextRun();
    void validationRejectsBadValues();
    void describeMentionsKind();

    void dueWhenSlotArrived();
    void notDueBeforeSlot();
    void missedSlotIsCaughtUpOnce();
    void missedSlotSkippedWhenCatchUpDisabled();
    void ancientMissedSlotIsSkipped();
    void retryTakesPrecedence();
    void firstEvaluationOnlySchedules();

    void successClearsRetries();
    void failureSchedulesRetryWithBackoff();
    void retryExhaustionStops();
};

void ScheduleTest::triggerTokensRoundTrip() {
    QCOMPARE(toToken(TriggerKind::Daily), QStringLiteral("daily"));
    QCOMPARE(toToken(TriggerKind::OnLogin), QStringLiteral("on_login"));
    QCOMPARE(static_cast<int>(triggerKindFromString(QStringLiteral("weekly")).value_or(TriggerKind::Manual)),
             static_cast<int>(TriggerKind::Weekly));
    QVERIFY(!triggerKindFromString(QStringLiteral("nonsense")).has_value());
    QCOMPARE(toToken(DueReason::Missed), QStringLiteral("missed"));
}

void ScheduleTest::dailyComputesNextOccurrence() {
    const ScheduleSpec spec = daily(QTime(2, 30));
    const QDateTime next = computeNextRun(spec, utc(QStringLiteral("2026-03-10T01:00:00Z")));
    QVERIFY(next.isValid());
    QCOMPARE(next, utc(QStringLiteral("2026-03-10T02:30:00Z")));
}

void ScheduleTest::dailyRollsToNextDayWhenTimePassed() {
    const ScheduleSpec spec = daily(QTime(2, 30));
    const QDateTime next = computeNextRun(spec, utc(QStringLiteral("2026-03-10T03:00:00Z")));
    QCOMPARE(next, utc(QStringLiteral("2026-03-11T02:30:00Z")));
}

void ScheduleTest::weeklyMatchesWeekday() {
    ScheduleSpec spec;
    spec.kind = TriggerKind::Weekly;
    spec.timeOfDay = QTime(4, 0);
    spec.dayOfWeek = 3; // Wednesday
    // 2026-03-10 is a Tuesday.
    const QDateTime next = computeNextRun(spec, utc(QStringLiteral("2026-03-10T12:00:00Z")));
    QVERIFY(next.isValid());
    QCOMPARE(next, utc(QStringLiteral("2026-03-11T04:00:00Z")));
    QCOMPARE(next.toUTC().date().dayOfWeek(), 3);
}

void ScheduleTest::weeklyRollsToNextWeek() {
    ScheduleSpec spec;
    spec.kind = TriggerKind::Weekly;
    spec.timeOfDay = QTime(4, 0);
    spec.dayOfWeek = 3;
    // Wednesday 05:00 is already past the 04:00 slot.
    const QDateTime next = computeNextRun(spec, utc(QStringLiteral("2026-03-11T05:00:00Z")));
    QCOMPARE(next, utc(QStringLiteral("2026-03-18T04:00:00Z")));
}

void ScheduleTest::monthlyMatchesDay() {
    ScheduleSpec spec;
    spec.kind = TriggerKind::Monthly;
    spec.timeOfDay = QTime(1, 15);
    spec.dayOfMonth = 5;
    const QDateTime next = computeNextRun(spec, utc(QStringLiteral("2026-03-05T00:00:00Z")));
    QCOMPARE(next, utc(QStringLiteral("2026-03-05T01:15:00Z")));

    const QDateTime after = computeNextRun(spec, utc(QStringLiteral("2026-03-05T02:00:00Z")));
    QCOMPARE(after, utc(QStringLiteral("2026-04-05T01:15:00Z")));
}

void ScheduleTest::monthlyRollsToNextMonth() {
    ScheduleSpec spec;
    spec.kind = TriggerKind::Monthly;
    spec.timeOfDay = QTime(1, 15);
    spec.dayOfMonth = 28;
    const QDateTime next = computeNextRun(spec, utc(QStringLiteral("2026-02-28T02:00:00Z")));
    QCOMPARE(next, utc(QStringLiteral("2026-03-28T01:15:00Z")));
}

void ScheduleTest::recurringRespectsTimezone() {
    // 02:30 Europe/Ljubljana is 01:30 UTC in winter (CET, UTC+1).
    const ScheduleSpec spec = daily(QTime(2, 30), QByteArrayLiteral("Europe/Ljubljana"));
    const QDateTime next = computeNextRun(spec, utc(QStringLiteral("2026-01-15T00:00:00Z")));
    QCOMPARE(next, utc(QStringLiteral("2026-01-15T01:30:00Z")));

    // In summer (CEST, UTC+2) the same wall-clock time is 00:30 UTC.
    const QDateTime summer = computeNextRun(spec, utc(QStringLiteral("2026-07-15T00:00:00Z")));
    QCOMPARE(summer, utc(QStringLiteral("2026-07-15T00:30:00Z")));
}

void ScheduleTest::daylightSavingGapIsHandled() {
    // 2026-03-29: Europe/Ljubljana jumps 02:00 -> 03:00, so 02:30 does not exist.
    const ScheduleSpec spec = daily(QTime(2, 30), QByteArrayLiteral("Europe/Ljubljana"));
    const QDateTime next = computeNextRun(spec, utc(QStringLiteral("2026-03-29T00:00:00Z")));
    QVERIFY2(next.isValid(), "a DST gap must not produce an invalid run time");
    QCOMPARE(next.toUTC().date(), QDate(2026, 3, 29));
    // Moved forward within the same day (03:30 local == 01:30 UTC after the jump).
    QVERIFY(next > utc(QStringLiteral("2026-03-29T00:00:00Z")));
}

void ScheduleTest::manualScheduleHasNoNextRun() {
    ScheduleSpec spec;
    spec.kind = TriggerKind::Manual;
    QVERIFY(!computeNextRun(spec, utc(QStringLiteral("2026-01-01T00:00:00Z"))).isValid());
}

void ScheduleTest::validationRejectsBadValues() {
    ScheduleSpec spec = daily(QTime(1, 0));
    QVERIFY(!spec.validate().has_value());

    spec.timezone = QByteArrayLiteral("Mars/Olympus");
    QVERIFY(spec.validate().has_value());

    spec = daily(QTime(1, 0));
    spec.dayOfWeek = 9;
    QVERIFY(spec.validate().has_value());

    spec = daily(QTime(1, 0));
    spec.dayOfMonth = 31;
    QVERIFY(spec.validate().has_value());

    spec = daily(QTime(1, 0));
    spec.retryMaxAttempts = 99;
    QVERIFY(spec.validate().has_value());
}

void ScheduleTest::describeMentionsKind() {
    QVERIFY(describe(daily(QTime(3, 0))).contains(QStringLiteral("Daily")));
    ScheduleSpec weekly;
    weekly.kind = TriggerKind::Weekly;
    weekly.dayOfWeek = 5;
    QVERIFY(describe(weekly).contains(QStringLiteral("Friday")));
    ScheduleSpec manual;
    QVERIFY(describe(manual).contains(QStringLiteral("Manual")));
}

void ScheduleTest::dueWhenSlotArrived() {
    const ScheduleSpec spec = daily(QTime(2, 0));
    ScheduleState state;
    state.nextRunUtc = utc(QStringLiteral("2026-03-10T02:00:00Z"));
    const QDateTime now = utc(QStringLiteral("2026-03-10T02:00:05Z"));
    const QDateTime started = utc(QStringLiteral("2026-03-09T00:00:00Z"));

    const DueDecision decision = evaluateDue(spec, state, now, started);
    QVERIFY(decision.shouldRun());
    QCOMPARE(static_cast<int>(decision.reason), static_cast<int>(DueReason::Scheduled));
    QCOMPARE(decision.nextRunUtc, utc(QStringLiteral("2026-03-11T02:00:00Z")));
}

void ScheduleTest::notDueBeforeSlot() {
    const ScheduleSpec spec = daily(QTime(2, 0));
    ScheduleState state;
    state.nextRunUtc = utc(QStringLiteral("2026-03-10T02:00:00Z"));
    const DueDecision decision = evaluateDue(spec, state, utc(QStringLiteral("2026-03-10T01:00:00Z")),
                                             utc(QStringLiteral("2026-03-09T00:00:00Z")));
    QVERIFY(!decision.shouldRun());
    QCOMPARE(decision.nextRunUtc, utc(QStringLiteral("2026-03-10T02:00:00Z")));
}

void ScheduleTest::missedSlotIsCaughtUpOnce() {
    const ScheduleSpec spec = daily(QTime(2, 0));
    ScheduleState state;
    state.nextRunUtc = utc(QStringLiteral("2026-03-10T02:00:00Z"));
    const QDateTime now = utc(QStringLiteral("2026-03-10T09:00:00Z"));
    const QDateTime started = utc(QStringLiteral("2026-03-10T08:59:00Z")); // agent started after the slot

    const DueDecision decision = evaluateDue(spec, state, now, started);
    QVERIFY(decision.shouldRun());
    QCOMPARE(static_cast<int>(decision.reason), static_cast<int>(DueReason::Missed));
    // Exactly one catch-up run, then the regular slot.
    QCOMPARE(decision.nextRunUtc, utc(QStringLiteral("2026-03-11T02:00:00Z")));
    QVERIFY(decision.explanation.contains(QStringLiteral("catching up")));
}

void ScheduleTest::missedSlotSkippedWhenCatchUpDisabled() {
    ScheduleSpec spec = daily(QTime(2, 0));
    spec.catchUpMissed = false;
    ScheduleState state;
    state.nextRunUtc = utc(QStringLiteral("2026-03-10T02:00:00Z"));
    const DueDecision decision = evaluateDue(spec, state, utc(QStringLiteral("2026-03-10T09:00:00Z")),
                                             utc(QStringLiteral("2026-03-10T08:59:00Z")));
    QVERIFY(!decision.shouldRun());
    QCOMPARE(decision.nextRunUtc, utc(QStringLiteral("2026-03-11T02:00:00Z")));
    QVERIFY(decision.explanation.contains(QStringLiteral("catch-up is disabled")));
}

void ScheduleTest::ancientMissedSlotIsSkipped() {
    const ScheduleSpec spec = daily(QTime(2, 0));
    ScheduleState state;
    state.nextRunUtc = utc(QStringLiteral("2026-01-01T02:00:00Z"));
    const QDateTime started = utc(QStringLiteral("2026-03-10T08:00:00Z"));
    const DueDecision decision =
        evaluateDue(spec, state, utc(QStringLiteral("2026-03-10T09:00:00Z")), started, 168);
    QVERIFY(!decision.shouldRun());
    QVERIFY(decision.explanation.contains(QStringLiteral("too old")));
}

void ScheduleTest::retryTakesPrecedence() {
    ScheduleSpec spec = daily(QTime(2, 0));
    spec.retryMaxAttempts = 3;
    spec.retryBackoffSeconds = 60;
    ScheduleState state;
    state.nextRunUtc = utc(QStringLiteral("2026-03-11T02:00:00Z"));
    state.retryAttemptCount = 1;
    state.retryNextUtc = utc(QStringLiteral("2026-03-10T10:00:00Z"));

    const DueDecision decision = evaluateDue(spec, state, utc(QStringLiteral("2026-03-10T10:00:30Z")),
                                             utc(QStringLiteral("2026-03-10T09:00:00Z")));
    QVERIFY(decision.shouldRun());
    QCOMPARE(static_cast<int>(decision.reason), static_cast<int>(DueReason::Retry));
    QCOMPARE(decision.retryAttempt, 2);
}

void ScheduleTest::firstEvaluationOnlySchedules() {
    const ScheduleSpec spec = daily(QTime(2, 0));
    ScheduleState state; // no next run yet
    const DueDecision decision = evaluateDue(spec, state, utc(QStringLiteral("2026-03-10T12:00:00Z")),
                                             utc(QStringLiteral("2026-03-10T12:00:00Z")));
    QVERIFY(!decision.shouldRun());
    QCOMPARE(decision.nextRunUtc, utc(QStringLiteral("2026-03-11T02:00:00Z")));
}

void ScheduleTest::successClearsRetries() {
    ScheduleSpec spec = daily(QTime(2, 0));
    spec.retryMaxAttempts = 3;
    const OutcomePlan plan = planAfterRun(spec, utc(QStringLiteral("2026-03-10T02:00:00Z")), true, 2);
    QCOMPARE(plan.retryAttemptCount, 0);
    QVERIFY(!plan.willRetry);
    QVERIFY(!plan.retryNextUtc.has_value());
    QCOMPARE(plan.nextRunUtc, utc(QStringLiteral("2026-03-11T02:00:00Z")));
}

void ScheduleTest::failureSchedulesRetryWithBackoff() {
    ScheduleSpec spec = daily(QTime(2, 0));
    spec.retryMaxAttempts = 3;
    spec.retryBackoffSeconds = 60;
    const QDateTime now = utc(QStringLiteral("2026-03-10T02:00:00Z"));

    const OutcomePlan first = planAfterRun(spec, now, false, 0);
    QVERIFY(first.willRetry);
    QCOMPARE(first.retryAttemptCount, 1);
    QCOMPARE(first.retryNextUtc.value(), now.addSecs(60));

    const OutcomePlan second = planAfterRun(spec, now, false, 1);
    QCOMPARE(second.retryNextUtc.value(), now.addSecs(120));
}

void ScheduleTest::retryExhaustionStops() {
    ScheduleSpec spec = daily(QTime(2, 0));
    spec.retryMaxAttempts = 2;
    const OutcomePlan plan =
        planAfterRun(spec, utc(QStringLiteral("2026-03-10T02:00:00Z")), false, 2);
    QVERIFY(!plan.willRetry);
    QVERIFY(!plan.retryNextUtc.has_value());
    QCOMPARE(plan.nextRunUtc, utc(QStringLiteral("2026-03-11T02:00:00Z")));
}

QTEST_GUILESS_MAIN(ScheduleTest)
#include "tst_schedule.moc"
