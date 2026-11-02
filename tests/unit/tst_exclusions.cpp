// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include "nesva/scan/exclusions.hpp"

using namespace nesva;
using nesva::scan::ExclusionRule;
using nesva::scan::ExclusionRules;
using nesva::scan::PatternKind;

class ExclusionsTest : public QObject {
    Q_OBJECT

private slots:
    void globConvertsToAnchoredRegex();
    void globMatchesFileNamesOnlyWithoutSlash();
    void globWithSlashMatchesFullPath();
    void doubleStarMatchesNestedPaths();
    void questionMarkAndClasses();
    void literalRules();
    void regexRulesAreAnchored();
    void invalidRegexIsRejected();
    void emptyRulesNeverMatch();
    void emptyPatternsAreIgnored();
};

void ExclusionsTest::globConvertsToAnchoredRegex() {
    const QRegularExpression expression = ExclusionRules::globToRegularExpression(QStringLiteral("*.log"));
    QVERIFY(expression.isValid());
    QVERIFY(expression.match(QStringLiteral("error.log")).hasMatch());
    QVERIFY(!expression.match(QStringLiteral("error.txt")).hasMatch());
    // Anchored: a suffix must not match.
    QVERIFY(!expression.match(QStringLiteral("error.log.bak")).hasMatch());
}

void ExclusionsTest::globMatchesFileNamesOnlyWithoutSlash() {
    const Result<ExclusionRules> rules =
        ExclusionRules::compile({{QStringLiteral("*.tmp"), PatternKind::Glob}});
    QVERIFY(rules);
    QVERIFY(rules.value().matches(QStringLiteral("a/b/c/file.tmp"), QStringLiteral("file.tmp")));
    QVERIFY(!rules.value().matches(QStringLiteral("a/b/c/file.txt"), QStringLiteral("file.txt")));
}

void ExclusionsTest::globWithSlashMatchesFullPath() {
    const Result<ExclusionRules> rules =
        ExclusionRules::compile({{QStringLiteral("cache/*.bin"), PatternKind::Glob}});
    QVERIFY(rules);
    QVERIFY(rules.value().matches(QStringLiteral("cache/data.bin"), QStringLiteral("data.bin")));
    QVERIFY(!rules.value().matches(QStringLiteral("nested/cache/data.bin"),
                                   QStringLiteral("data.bin")));
}

void ExclusionsTest::doubleStarMatchesNestedPaths() {
    const Result<ExclusionRules> rules =
        ExclusionRules::compile({{QStringLiteral("build/**"), PatternKind::Glob}});
    QVERIFY(rules);
    QVERIFY(rules.value().matches(QStringLiteral("build/out/app.exe"), QStringLiteral("app.exe")));
    QVERIFY(rules.value().matches(QStringLiteral("build/tmp/x/y/z.o"), QStringLiteral("z.o")));
    QVERIFY(!rules.value().matches(QStringLiteral("src/build/app.cpp"), QStringLiteral("app.cpp")));
}

void ExclusionsTest::questionMarkAndClasses() {
    const Result<ExclusionRules> rules = ExclusionRules::compile({
        {QStringLiteral("file?.txt"), PatternKind::Glob},
        {QStringLiteral("*.bak[0-9]"), PatternKind::Glob},
    });
    QVERIFY(rules);
    QVERIFY(rules.value().matches(QStringLiteral("file1.txt"), QStringLiteral("file1.txt")));
    QVERIFY(!rules.value().matches(QStringLiteral("file10.txt"), QStringLiteral("file10.txt")));
    QVERIFY(rules.value().matches(QStringLiteral("x.bak7"), QStringLiteral("x.bak7")));
    QVERIFY(!rules.value().matches(QStringLiteral("x.bakx"), QStringLiteral("x.bakx")));
}

void ExclusionsTest::literalRules() {
    const Result<ExclusionRules> rules =
        ExclusionRules::compile({{QStringLiteral("notes.tmp"), PatternKind::Literal}});
    QVERIFY(rules);
    QVERIFY(rules.value().matches(QStringLiteral("a/notes.tmp"), QStringLiteral("notes.tmp")));
    // Literal means literal: no wildcard interpretation.
    const Result<ExclusionRules> star = ExclusionRules::compile(
        {{QStringLiteral("*.tmp"), PatternKind::Literal}});
    QVERIFY(star);
    QVERIFY(!star.value().matches(QStringLiteral("x.tmp"), QStringLiteral("x.tmp")));
}

void ExclusionsTest::regexRulesAreAnchored() {
    const Result<ExclusionRules> rules =
        ExclusionRules::compile({{QStringLiteral(".*\\.log$"), PatternKind::Regex}});
    QVERIFY(rules);
    QVERIFY(rules.value().matches(QStringLiteral("logs/app.log"), QStringLiteral("app.log")));
    QVERIFY(!rules.value().matches(QStringLiteral("logs/app.txt"), QStringLiteral("app.txt")));
}

void ExclusionsTest::invalidRegexIsRejected() {
    const Result<ExclusionRules> rules =
        ExclusionRules::compile({{QStringLiteral("([unclosed"), PatternKind::Regex}});
    QVERIFY(!rules);
    QCOMPARE(static_cast<int>(rules.error().code()), static_cast<int>(ErrorCode::ConfigurationError));
}

void ExclusionsTest::emptyRulesNeverMatch() {
    const Result<ExclusionRules> rules = ExclusionRules::compile({});
    QVERIFY(rules);
    QVERIFY(rules.value().isEmpty());
    QVERIFY(!rules.value().matches(QStringLiteral("anything"), QStringLiteral("anything")));
}

void ExclusionsTest::emptyPatternsAreIgnored() {
    const Result<ExclusionRules> rules = ExclusionRules::compile({
        {QStringLiteral("   "), PatternKind::Glob},
        {QString(), PatternKind::Glob},
        {QStringLiteral("*.tmp"), PatternKind::Glob},
    });
    QVERIFY(rules);
    QCOMPARE(rules.value().ruleCount(), 1);
}

QTEST_GUILESS_MAIN(ExclusionsTest)
#include "tst_exclusions.moc"
