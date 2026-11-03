// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>

#include <functional>
#include <memory>

#include "nesva/core/hashing.hpp"
#include "nesva/core/http.hpp"
#include "nesva/tools/tool_installer.hpp"

using namespace nesva;

namespace {

/// Scripted download server: answers by URL substring so the installer's
/// download, checksum and failure paths can be exercised without a network.
class ScriptedTransport final : public net::HttpTransport {
public:
    QByteArray versionBody = QByteArrayLiteral("rclone v9.9.9\n");
    QByteArray archiveBody = QByteArrayLiteral("not really a zip");
    QByteArray checksumBody; ///< empty means "derive a correct one"
    int archiveStatus = 200;
    int checksumStatus = 200;
    QVector<QString> requested;

    net::HttpResponse send(net::HttpVerb verb, const QUrl& url,
                           const QHash<QString, QString>& headers, const QByteArray& body,
                           int timeoutMs) override {
        Q_UNUSED(verb);
        Q_UNUSED(headers);
        Q_UNUSED(body);
        Q_UNUSED(timeoutMs);
        requested.append(url.toString());
        net::HttpResponse response;
        const QString target = url.toString();
        if (target.endsWith(QStringLiteral("/version.txt"))) {
            response.statusCode = 200;
            response.body = versionBody;
            return response;
        }
        if (target.endsWith(QStringLiteral("/SHA256SUMS"))) {
            response.statusCode = checksumStatus;
            if (checksumStatus == 200) {
                response.body = checksumBody.isEmpty()
                                    ? QStringLiteral("%1  %2\n")
                                          .arg(sha256OfBytes(archiveBody),
                                               QString::fromUtf8(kArchiveName))
                                          .toUtf8()
                                    : checksumBody;
            }
            return response;
        }
        response.statusCode = archiveStatus;
        if (archiveStatus == 200) {
            response.body = archiveBody;
        }
        return response;
    }

    static constexpr const char* kArchiveName = "rclone-v9.9.9-linux-amd64.zip";
};

} // namespace

class ToolInstallerTest : public QObject {
    Q_OBJECT

private slots:
    void planDescribesTheExactCommands();
    void systemPlanShowsTheElevatedCommand();
    void installRefusesAnUnverifiedArchive();
    void installReportsADownloadFailure();
    void rendererPrintsCommandsVerbatim();
};

void ToolInstallerTest::planDescribesTheExactCommands() {
    ScriptedTransport transport;
    const Result<tools::ToolInstallPlan> plan =
        tools::planRcloneInstall(tools::InstallScope::User, transport);
    QVERIFY2(plan.hasValue(), plan.hasValue() ? "" : plan.error().toDisplayString().c_str());

    QCOMPARE(plan.value().version, QStringLiteral("9.9.9"));
    QVERIFY(plan.value().downloadUrl.endsWith(QStringLiteral("/v9.9.9/") +
                                              QString::fromUtf8(ScriptedTransport::kArchiveName)));
    QVERIFY(plan.value().checksumUrl.endsWith(QStringLiteral("/v9.9.9/SHA256SUMS")));
    QVERIFY(plan.value().targetPath.endsWith(QStringLiteral("rclone")) ||
            plan.value().targetPath.endsWith(QStringLiteral("rclone.exe")));
    QVERIFY(!plan.value().requiresElevation);
    QVERIFY(plan.value().automatic);

    // Every step that runs a program must show its full argument vector, and no
    // step may be a shell string.
    QVERIFY(plan.value().steps.size() >= 5);
    for (const tools::InstallStep& step : plan.value().steps) {
        for (const QString& argument : step.command) {
            QVERIFY2(!argument.contains(QStringLiteral("&&")) &&
                         !argument.contains(QStringLiteral("|")) &&
                         !argument.contains(QStringLiteral(";")),
                     qPrintable(argument));
        }
    }
    QCOMPARE(plan.value().steps.last().command.constFirst(), plan.value().targetPath);

    // The plan is honest about checksum trust.
    bool mentionsChecksumLimit = false;
    for (const QString& note : plan.value().notes) {
        if (note.contains(QStringLiteral("same HTTPS host"))) {
            mentionsChecksumLimit = true;
        }
    }
    QVERIFY(mentionsChecksumLimit);
}

void ToolInstallerTest::systemPlanShowsTheElevatedCommand() {
    ScriptedTransport transport;
    const Result<tools::ToolInstallPlan> plan =
        tools::planRcloneInstall(tools::InstallScope::System, transport);
    QVERIFY(plan.hasValue());
    QVERIFY(plan.value().requiresElevation);
    QVERIFY(plan.value().targetPath.startsWith(QStringLiteral("/usr/")) ||
            plan.value().targetPath.contains(QStringLiteral("Program Files")));

#if !defined(Q_OS_WIN)
    // Linux: the preview contains the exact pkexec command, so the user sees
    // what will run with elevated privileges before approving it.
    bool foundElevatedStep = false;
    for (const tools::InstallStep& step : plan.value().steps) {
        if (!step.command.isEmpty() && step.command.constFirst() == QStringLiteral("pkexec")) {
            foundElevatedStep = true;
            QVERIFY(step.command.contains(QStringLiteral("install")));
            QVERIFY(step.command.contains(plan.value().targetPath));
        }
    }
    QVERIFY(foundElevatedStep);
#endif
}

void ToolInstallerTest::installRefusesAnUnverifiedArchive() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    ScriptedTransport transport;
    transport.checksumBody = QByteArrayLiteral(
        "0000000000000000000000000000000000000000000000000000000000000000  "
        "rclone-v9.9.9-linux-amd64.zip\n");

    const Result<tools::ToolInstallPlan> plan =
        tools::planRcloneInstall(tools::InstallScope::User, transport);
    QVERIFY(plan.hasValue());

    tools::ToolInstallPlan target = plan.value();
    // Install into the temporary directory so the test can never touch the real
    // user's tools.
    target.targetPath = directory.filePath(QStringLiteral("rclone"));
    target.scope = tools::InstallScope::User;

    const Result<tools::InstallOutcome> outcome =
        tools::executeRcloneInstall(target, {}, &transport);
    QVERIFY(!outcome);
    QCOMPARE(static_cast<int>(outcome.error().code()),
             static_cast<int>(ErrorCode::IntegrityError));
    QVERIFY2(!QFileInfo::exists(target.targetPath),
             "nothing may be installed when the checksum does not match");
}

void ToolInstallerTest::installReportsADownloadFailure() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    ScriptedTransport transport;
    transport.archiveStatus = 503;

    const Result<tools::ToolInstallPlan> plan =
        tools::planRcloneInstall(tools::InstallScope::User, transport);
    QVERIFY(plan.hasValue());
    tools::ToolInstallPlan target = plan.value();
    target.targetPath = directory.filePath(QStringLiteral("rclone"));

    const Result<tools::InstallOutcome> outcome =
        tools::executeRcloneInstall(target, {}, &transport);
    QVERIFY(!outcome);
    QCOMPARE(static_cast<int>(outcome.error().code()),
             static_cast<int>(ErrorCode::ExternalToolFailed));
    QVERIFY(!QFileInfo::exists(target.targetPath));
}

void ToolInstallerTest::rendererPrintsCommandsVerbatim() {
    ScriptedTransport transport;
    const Result<tools::ToolInstallPlan> plan =
        tools::planRcloneInstall(tools::InstallScope::System, transport);
    QVERIFY(plan.hasValue());
    const QString text = tools::describeInstallPlan(plan.value());
    QVERIFY(text.contains(plan.value().downloadUrl));
    QVERIFY(text.contains(plan.value().targetPath));
#if !defined(Q_OS_WIN)
    QVERIFY(text.contains(QStringLiteral("pkexec install -m 0755")));
#endif
}

QTEST_GUILESS_MAIN(ToolInstallerTest)
#include "tst_tool_installer.moc"
