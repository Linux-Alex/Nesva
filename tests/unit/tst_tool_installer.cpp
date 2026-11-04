// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSysInfo>
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
        if (target.contains(QStringLiteral("api.github.com"))) {
            // The GitHub release API only answers JSON clients.
            response.statusCode = 200;
            response.body = QByteArrayLiteral("{\"tag_name\": \"v9.9.9\"}");
            return response;
        }
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
                                               archiveName())
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

    /// The release file the installer asks for on this platform. Hardcoding
    /// "linux" made every assertion fail on Windows and macOS.
    static QString archiveName() {
#if defined(Q_OS_WIN)
        const QString platform = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
        const QString platform = QStringLiteral("osx");
#else
        const QString platform = QStringLiteral("linux");
#endif
        QString arch = QSysInfo::currentCpuArchitecture();
        if (arch == QStringLiteral("x86_64")) {
            arch = QStringLiteral("amd64");
        } else if (arch == QStringLiteral("i386")) {
            arch = QStringLiteral("386");
        }
        return QStringLiteral("rclone-v9.9.9-%1-%2.zip").arg(platform, arch);
    }

    /// restic publishes a lone bzip2 stream on Linux and macOS and a zip on
    /// Windows, named after the tool rather than after its release directory.
    static QString resticArchiveName() {
#if defined(Q_OS_WIN)
        const QString platform = QStringLiteral("windows");
        const QString suffix = QStringLiteral("zip");
#elif defined(Q_OS_MACOS)
        const QString platform = QStringLiteral("darwin");
        const QString suffix = QStringLiteral("bz2");
#else
        const QString platform = QStringLiteral("linux");
        const QString suffix = QStringLiteral("bz2");
#endif
        QString arch = QSysInfo::currentCpuArchitecture();
        if (arch == QStringLiteral("x86_64")) {
            arch = QStringLiteral("amd64");
        } else if (arch == QStringLiteral("i386")) {
            arch = QStringLiteral("386");
        }
        return QStringLiteral("restic_9.9.9_%1_%2.%3").arg(platform, arch, suffix);
    }
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
    void resticPlanUsesTheGithubRelease();
    void resticInstallRefusesAnUnverifiedArchive();
    void payloadIsFoundUnderItsReleaseAssetName();
};

void ToolInstallerTest::planDescribesTheExactCommands() {
    ScriptedTransport transport;
    const Result<tools::ToolInstallPlan> plan =
        tools::planRcloneInstall(tools::InstallScope::User, transport);
    QVERIFY2(plan.hasValue(), plan.hasValue() ? "" : plan.error().toDisplayString().c_str());

    QCOMPARE(plan.value().version, QStringLiteral("9.9.9"));
    QVERIFY(plan.value().downloadUrl.endsWith(QStringLiteral("/v9.9.9/") +
                                              ScriptedTransport::archiveName()));
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

void ToolInstallerTest::resticPlanUsesTheGithubRelease() {
    ScriptedTransport transport;
    const Result<tools::ToolInstallPlan> plan =
        tools::planToolInstall(tools::ToolId::Restic, tools::InstallScope::User, transport);
    QVERIFY2(plan.hasValue(), plan.hasValue() ? "" : plan.error().toDisplayString().c_str());

    QCOMPARE(plan.value().tool, tools::ToolId::Restic);
    QCOMPARE(plan.value().toolToken, QStringLiteral("restic"));
    QCOMPARE(plan.value().toolDisplayName, QStringLiteral("Restic"));
    QCOMPARE(plan.value().version, QStringLiteral("9.9.9"));
    // The version comes from the GitHub API, so the plan must have asked it.
    bool askedGithub = false;
    for (const QString& url : transport.requested) {
        if (url.contains(QStringLiteral("api.github.com"))) {
            askedGithub = true;
        }
    }
    QVERIFY(askedGithub);
    QVERIFY(plan.value().downloadUrl.endsWith(
        QStringLiteral("/v9.9.9/") + ScriptedTransport::resticArchiveName()));
    QVERIFY(plan.value().checksumUrl.endsWith(QStringLiteral("/v9.9.9/SHA256SUMS")));
    QVERIFY(plan.value().targetPath.endsWith(QStringLiteral("restic")) ||
            plan.value().targetPath.endsWith(QStringLiteral("restic.exe")));
    QVERIFY(!plan.value().requiresElevation);

    // Restic needs a repository password; the plan has to say where it goes.
    bool mentionsKeyring = false;
    for (const QString& note : plan.value().notes) {
        if (note.contains(QStringLiteral("keyring"))) {
            mentionsKeyring = true;
        }
    }
    QVERIFY(mentionsKeyring);

    QCOMPARE(tools::toToken(tools::ToolId::Restic), QStringLiteral("restic"));
    QCOMPARE(tools::displayName(tools::ToolId::Restic), QStringLiteral("Restic"));
    QCOMPARE(tools::toToken(tools::ToolId::Rclone), QStringLiteral("rclone"));
    QCOMPARE(tools::displayName(tools::ToolId::Rclone), QStringLiteral("Rclone"));
    QVERIFY(tools::toolTargetPath(tools::ToolId::Restic, tools::InstallScope::User)
                .endsWith(QStringLiteral("restic")) ||
            tools::toolTargetPath(tools::ToolId::Restic, tools::InstallScope::User)
                .endsWith(QStringLiteral("restic.exe")));
}

void ToolInstallerTest::resticInstallRefusesAnUnverifiedArchive() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    ScriptedTransport transport;
    transport.checksumBody = QByteArrayLiteral(
        "1111111111111111111111111111111111111111111111111111111111111111  "
        "restic_9.9.9_linux_amd64.bz2\n");

    const Result<tools::ToolInstallPlan> plan =
        tools::planToolInstall(tools::ToolId::Restic, tools::InstallScope::User, transport);
    QVERIFY(plan.hasValue());

    tools::ToolInstallPlan target = plan.value();
    target.targetPath = directory.filePath(QStringLiteral("restic"));

    const Result<tools::InstallOutcome> outcome =
        tools::executeToolInstall(target, {}, &transport);
    QVERIFY(!outcome);
    QCOMPARE(static_cast<int>(outcome.error().code()),
             static_cast<int>(ErrorCode::IntegrityError));
    QVERIFY(!QFileInfo::exists(target.targetPath));
}

void ToolInstallerTest::payloadIsFoundUnderItsReleaseAssetName() {
    // restic's Windows zip contains "restic_0.19.1_windows_amd64.exe" and its
    // Linux bzip2 stream decompresses to "restic_0.19.1_linux_amd64" - neither is
    // called "restic", so an exact-name search found nothing and the install
    // failed after a successful download and checksum check.
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString assetName = QStringLiteral("restic_0.19.1_windows_amd64.zip");
    const QString payload =
        directory.filePath(QStringLiteral("restic_0.19.1_windows_amd64.exe"));
    {
        QFile file(payload);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("MZ binary");
    }
    {
        QFile readme(directory.filePath(QStringLiteral("README.txt")));
        QVERIFY(readme.open(QIODevice::WriteOnly));
        readme.write("restic documentation");
    }

    QCOMPARE(tools::findToolBinary(directory.path(), assetName, tools::ToolId::Restic), payload);

    // The bzip2 payload has no suffix at all.
    const QString bz2Payload = directory.filePath(QStringLiteral("restic_0.19.1_linux_amd64"));
    {
        QFile file(bz2Payload);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("ELF binary");
    }
    QCOMPARE(tools::findToolBinary(directory.path(),
                                   QStringLiteral("restic_0.19.1_linux_amd64.bz2"),
                                   tools::ToolId::Restic),
             bz2Payload);

    // A binary already carrying the tool's name always wins over the asset name.
    const QString exact = directory.filePath(QStringLiteral("restic"));
    {
        QFile file(exact);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("ELF binary");
    }
    QCOMPARE(tools::findToolBinary(directory.path(), assetName, tools::ToolId::Restic), exact);

    // An unpack directory holding only documentation yields nothing rather than
    // a README copied into place as the tool.
    QTemporaryDir docs;
    QVERIFY(docs.isValid());
    QFile readme(docs.filePath(QStringLiteral("README.md")));
    QVERIFY(readme.open(QIODevice::WriteOnly));
    readme.write("restic");
    QVERIFY(tools::findToolBinary(docs.path(), assetName, tools::ToolId::Restic).isEmpty());
}

QTEST_GUILESS_MAIN(ToolInstallerTest)
#include "tst_tool_installer.moc"
