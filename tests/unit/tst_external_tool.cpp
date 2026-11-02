// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QFileInfo>

#include "nesva/core/external_tool.hpp"

using namespace nesva;

class ExternalToolTest : public QObject {
    Q_OBJECT

private slots:
    void resolvesAbsolutePaths();
    void resolvesNamesOnPath();
    void reportsMissingExecutables();
    void probesVersionOutput();
    void probesNonZeroExit();
    void probesMissingProgram();
    void neverInvokesAShell();
};

void ExternalToolTest::resolvesAbsolutePaths() {
#ifdef Q_OS_WIN
    QSKIP("Unix-specific executable paths");
#else
    QCOMPARE(resolveExecutable(QStringLiteral("/bin/sh")), QStringLiteral("/bin/sh"));
    QVERIFY(resolveExecutable(QStringLiteral("/definitely/not/here")).isEmpty());
#endif
}

void ExternalToolTest::resolvesNamesOnPath() {
#ifdef Q_OS_WIN
    QSKIP("Unix-specific executable names");
#else
    const QString resolved = resolveExecutable(QStringLiteral("sh"));
    QVERIFY(!resolved.isEmpty());
    QVERIFY(QFileInfo(resolved).isAbsolute());
#endif
}

void ExternalToolTest::reportsMissingExecutables() {
    const Result<ExternalToolInfo> probe =
        probeExternalTool(QStringLiteral("nesva-not-a-real-binary-42"), {QStringLiteral("version")});
    QVERIFY(!probe);
    QCOMPARE(static_cast<int>(probe.error().code()), static_cast<int>(ErrorCode::NotFound));
}

void ExternalToolTest::probesVersionOutput() {
#ifdef Q_OS_WIN
    QSKIP("Unix-specific probe");
#else
    const Result<ExternalToolInfo> probe =
        probeExternalTool(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("echo 1.2.3")});
    QVERIFY(probe);
    QCOMPARE(probe.value().versionText, QStringLiteral("1.2.3"));
    QCOMPARE(probe.value().exitCode, 0);
    QVERIFY(probe.value().executable.endsWith(QStringLiteral("sh")));
#endif
}

void ExternalToolTest::probesNonZeroExit() {
#ifdef Q_OS_WIN
    QSKIP("Unix-specific probe");
#else
    const Result<ExternalToolInfo> probe = probeExternalTool(
        QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 7")});
    QVERIFY(!probe);
    QCOMPARE(static_cast<int>(probe.error().code()), static_cast<int>(ErrorCode::ExternalToolFailed));
    QVERIFY(probe.error().detail().find("exit 7") != std::string::npos);
#endif
}

void ExternalToolTest::probesMissingProgram() {
    const Result<ExternalToolInfo> probe = probeExternalTool(
        QStringLiteral("/nonexistent/nesva-tool"), {QStringLiteral("--version")}, 1000);
    QVERIFY(!probe);
    QCOMPARE(static_cast<int>(probe.error().code()), static_cast<int>(ErrorCode::NotFound));
}

void ExternalToolTest::neverInvokesAShell() {
#ifdef Q_OS_WIN
    QSKIP("Unix-specific probe");
#else
    // Arguments are passed as an array, so shell metacharacters are literal
    // characters and must not be interpreted.
    const Result<ExternalToolInfo> probe = probeExternalTool(
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"), QStringLiteral("printf '%s' 'a;b|c$(d)'")});
    QVERIFY(probe);
    QCOMPARE(probe.value().versionText, QStringLiteral("a;b|c$(d)"));
#endif
}

QTEST_GUILESS_MAIN(ExternalToolTest)
#include "tst_external_tool.moc"
