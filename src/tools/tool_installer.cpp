// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/tools/tool_installer.hpp"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QSysInfo>
#include <QtCore/QTemporaryDir>

#include <utility>

#include "nesva/core/external_tool.hpp"
#include "nesva/core/hashing.hpp"
#include "nesva/core/logging.hpp"

namespace nesva::tools {
namespace {

constexpr const char* kReleaseBase = "https://downloads.rclone.org";

struct CommandOutcome {
    int exitCode = -1;
    QString output;
    QString error;
    bool started = false;
};

/// Runs a program with an argument vector. There is no shell anywhere: the
/// arguments reach the process exactly as they were built here.
CommandOutcome runCommand(const QString& program, const QStringList& arguments,
                          const QString& workingDirectory = {}, int timeoutMs = 120000) {
    CommandOutcome outcome;
    const QString resolved = resolveExecutable(program);
    if (resolved.isEmpty()) {
        outcome.error = QStringLiteral("%1 was not found").arg(program);
        return outcome;
    }
    QProcess process;
    if (!workingDirectory.isEmpty()) {
        process.setWorkingDirectory(workingDirectory);
    }
    process.start(resolved, arguments);
    if (!process.waitForStarted(10000)) {
        outcome.error = process.errorString();
        return outcome;
    }
    outcome.started = true;
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        outcome.error = QStringLiteral("%1 did not finish in time").arg(program);
        return outcome;
    }
    outcome.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    outcome.output = QString::fromLocal8Bit(process.readAllStandardOutput());
    outcome.error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (outcome.exitCode != 0 && outcome.error.isEmpty()) {
        outcome.error = QStringLiteral("%1 exited with code %2").arg(program, QString::number(outcome.exitCode));
    }
    return outcome;
}

QString releasePlatform() {
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("osx");
#else
    return QStringLiteral("linux");
#endif
}

QString releaseArchitecture() {
    const QString architecture = QSysInfo::currentCpuArchitecture();
    if (architecture == QStringLiteral("x86_64") || architecture == QStringLiteral("amd64")) {
        return QStringLiteral("amd64");
    }
    if (architecture == QStringLiteral("arm64") || architecture == QStringLiteral("aarch64")) {
        return QStringLiteral("arm64");
    }
    if (architecture == QStringLiteral("arm")) {
        return QStringLiteral("arm");
    }
    if (architecture == QStringLiteral("i386")) {
        return QStringLiteral("386");
    }
    return architecture;
}

QString binaryName() {
#if defined(Q_OS_WIN)
    return QStringLiteral("rclone.exe");
#else
    return QStringLiteral("rclone");
#endif
}

/// Command that unpacks the release archive, or an empty list when no usable
/// unpacker is installed. Every candidate is a normal program with an argument
/// vector; none of them is a shell.
QStringList extractionCommand(const QString& archivePath, const QString& destination) {
    struct Candidate {
        const char* program;
        QStringList arguments;
    };
    QVector<Candidate> candidates;
#if defined(Q_OS_WIN)
    // tar.exe ships with Windows 10 1803 and later and reads zip archives.
    candidates.append(
        {"tar", {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), destination}});
    candidates.append(
        {"unzip", {QStringLiteral("-o"), archivePath, QStringLiteral("-d"), destination}});
#else
    candidates.append(
        {"unzip", {QStringLiteral("-o"), archivePath, QStringLiteral("-d"), destination}});
    candidates.append(
        {"bsdtar", {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), destination}});
    candidates.append({"python3",
                       {QStringLiteral("-m"), QStringLiteral("zipfile"), QStringLiteral("-e"),
                        archivePath, destination}});
    candidates.append({"7z", {QStringLiteral("x"), QStringLiteral("-y"),
                              QStringLiteral("-o") + destination, archivePath}});
#endif
    for (const Candidate& candidate : candidates) {
        if (!resolveExecutable(QString::fromUtf8(candidate.program)).isEmpty()) {
            QStringList command;
            command.append(QString::fromUtf8(candidate.program));
            command.append(candidate.arguments);
            return command;
        }
    }
    return {};
}

/// Finds the extracted binary anywhere below \p directory.
QString findExtractedBinary(const QString& directory) {
    QDirIterator iterator(directory, QStringList{binaryName()}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        return iterator.next();
    }
    return {};
}

QString parseVersion(const QString& output) {
    // "rclone v1.75.2" -> "1.75.2"
    const QString first = output.section(QLatin1Char('\n'), 0, 0).trimmed();
    QString value = first;
    value.remove(QStringLiteral("rclone"));
    value = value.trimmed();
    if (value.startsWith(QLatin1Char('v'))) {
        value.remove(0, 1);
    }
    return value;
}

} // namespace

QString toToken(InstallScope scope) {
    return scope == InstallScope::System ? QStringLiteral("system") : QStringLiteral("user");
}

QString userInstallDirectory() {
#if defined(Q_OS_WIN)
    const QString localAppData =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (localAppData.isEmpty()) {
        return {};
    }
    return localAppData + QStringLiteral("/Programs/rclone");
#else
    const QString home = QDir::homePath();
    if (home.isEmpty()) {
        return {};
    }
    return home + QStringLiteral("/.local/bin");
#endif
}

QString rcloneTargetPath(InstallScope scope) {
    if (scope == InstallScope::User) {
        const QString directory = userInstallDirectory();
        return directory.isEmpty() ? QString() : directory + QLatin1Char('/') + binaryName();
    }
#if defined(Q_OS_WIN)
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    const QString base = programFiles.isEmpty() ? QStringLiteral("C:/Program Files") : programFiles;
    return base + QStringLiteral("/rclone/") + binaryName();
#else
    return QStringLiteral("/usr/local/bin/") + binaryName();
#endif
}

Result<ToolInstallPlan> planRcloneInstall(InstallScope scope, net::HttpTransport& transport) {
    ToolInstallPlan plan;
    plan.scope = scope;
    plan.requiresElevation = scope == InstallScope::System;
    plan.targetPath = rcloneTargetPath(scope);
    if (plan.targetPath.isEmpty()) {
        return makeErrorValue(ErrorCode::ConfigurationError,
                              "this system has no per-user program directory to install into");
    }

    const net::HttpResponse version = transport.send(
        net::HttpVerb::Get, QUrl(QStringLiteral("%1/version.txt").arg(kReleaseBase)),
        {{QStringLiteral("Accept"), QStringLiteral("text/plain")}}, {}, 20000);
    if (version.statusCode == 0) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "cannot reach the rclone download server", version.error.toStdString());
    }
    if (!version.isSuccess()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "the rclone download server refused the request",
                              QStringLiteral("%1: %2")
                                  .arg(version.statusCode)
                                  .arg(QString::fromUtf8(version.body.left(200)))
                                  .toStdString());
    }
    plan.version = parseVersion(QString::fromUtf8(version.body));
    if (plan.version.isEmpty()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "the rclone download server returned no usable version",
                              QString::fromUtf8(version.body.left(120)).toStdString());
    }

    plan.archiveName = QStringLiteral("rclone-v%1-%2-%3.zip")
                           .arg(plan.version, releasePlatform(), releaseArchitecture());
    plan.downloadUrl =
        QStringLiteral("%1/v%2/%3").arg(kReleaseBase, plan.version, plan.archiveName);
    plan.checksumUrl = QStringLiteral("%1/v%2/SHA256SUMS").arg(kReleaseBase, plan.version);

    InstallStep download;
    download.title = QStringLiteral("Download rclone %1 to a private temporary directory")
                         .arg(plan.version);
    download.note = plan.downloadUrl;
    plan.steps.append(download);

    InstallStep verify;
    verify.title = QStringLiteral("Compute its SHA-256 and compare it with the published checksum");
    verify.note = plan.checksumUrl;
    plan.steps.append(verify);

    InstallStep extract;
    extract.title = QStringLiteral("Unpack the archive");
    const QStringList extractCommand = extractionCommand(QStringLiteral("<archive>"),
                                                         QStringLiteral("<temporary directory>"));
    extract.command = extractCommand;
    if (extractCommand.isEmpty()) {
        extract.note = QStringLiteral("No unpacker found: install one of unzip, bsdtar, "
                                      "python3 or 7z first.");
        plan.automatic = false;
    } else {
        extract.note = QStringLiteral("Uses the first unpacker already present on this system.");
    }
    plan.steps.append(extract);

    InstallStep install;
    install.title = QStringLiteral("Install the binary to %1").arg(plan.targetPath);
    if (scope == InstallScope::System) {
#if defined(Q_OS_WIN)
        plan.automatic = false;
        install.note = QStringLiteral("This build does not elevate on Windows. Run an "
                                      "Administrator terminal and copy the extracted rclone.exe "
                                      "to %1 yourself.")
                           .arg(plan.targetPath);
#else
        install.command = {QStringLiteral("pkexec"), QStringLiteral("install"),
                           QStringLiteral("-m"), QStringLiteral("0755"),
                           QStringLiteral("<verified rclone>"), plan.targetPath};
        install.note = QStringLiteral("pkexec asks for your password and runs install(1) with "
                                      "exactly these arguments: no shell, no download as root.");
#endif
    } else {
        install.note = QStringLiteral("A plain file copy inside your own home directory: no "
                                      "password and no elevated privileges.");
    }
    plan.steps.append(install);

    InstallStep finalCheck;
    finalCheck.title = QStringLiteral("Run the installed binary and record its path");
    finalCheck.command = {plan.targetPath, QStringLiteral("version")};
    plan.steps.append(finalCheck);

    plan.notes.append(
        QStringLiteral("The checksum list is fetched from the same HTTPS host as the archive: it "
                       "catches a truncated or corrupted download and a broken mirror, not a "
                       "compromised server. rclone publishes no signature."));
    plan.notes.append(
        QStringLiteral("Nesva stores the installed path, so the tool works even if %1 is not on "
                       "your PATH.")
            .arg(QFileInfo(plan.targetPath).absolutePath()));
#if defined(Q_OS_WIN)
    plan.notes.append(QStringLiteral("rclone publishes unsigned Windows builds; SmartScreen may "
                                     "warn the first time it runs."));
#endif
    return plan;
}

QString describeInstallPlan(const ToolInstallPlan& plan) {
    QString text = QStringLiteral("Rclone %1 -> %2 (%3 scope)")
                       .arg(plan.version, plan.targetPath, toToken(plan.scope));
    text += QLatin1Char('\n');
    int index = 0;
    for (const InstallStep& step : plan.steps) {
        text += QStringLiteral("%1. %2\n").arg(++index).arg(step.title);
        if (!step.command.isEmpty()) {
            text += QStringLiteral("     $ %1\n").arg(step.command.join(QLatin1Char(' ')));
        }
        if (!step.note.isEmpty()) {
            text += QStringLiteral("     %1\n").arg(step.note);
        }
    }
    for (const QString& note : plan.notes) {
        text += QStringLiteral("Note: %1\n").arg(note);
    }
    return text;
}

Result<InstallOutcome> executeRcloneInstall(const ToolInstallPlan& plan,
                                            const std::function<void(const QString&)>& progress,
                                            net::HttpTransport* transport) {
    const auto report = [&progress](const QString& line) {
        if (progress) {
            progress(line);
        }
    };
    if (!plan.automatic) {
        return makeErrorValue(ErrorCode::NotImplemented,
                              "this installation cannot be performed automatically here",
                              QStringLiteral("run the shown commands by hand").toStdString());
    }
    if (plan.targetPath.isEmpty() || plan.downloadUrl.isEmpty()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "the installation plan is incomplete");
    }

    QTemporaryDir workspace(QDir::tempPath() + QStringLiteral("/nesva-install-XXXXXX"));
    if (!workspace.isValid()) {
        return makeErrorValue(ErrorCode::IoError, "cannot create a temporary directory");
    }
    net::QNetworkHttpTransport defaultTransport;
    net::HttpTransport& channel = transport != nullptr ? *transport : defaultTransport;

    report(QStringLiteral("Downloading %1").arg(plan.archiveName));
    const net::HttpResponse archive =
        channel.send(net::HttpVerb::Get, QUrl(plan.downloadUrl),
                       {{QStringLiteral("Accept"), QStringLiteral("application/zip")}}, {}, 300000);
    if (archive.statusCode == 0) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "the download failed",
                              archive.error.toStdString());
    }
    if (!archive.isSuccess()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "the download was refused",
                              QStringLiteral("%1: %2")
                                  .arg(archive.statusCode)
                                  .arg(QString::fromUtf8(archive.body.left(200)))
                                  .toStdString());
    }
    if (archive.body.isEmpty()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "the download was empty");
    }

    report(QStringLiteral("Verifying the SHA-256 checksum"));
    const net::HttpResponse checksums =
        channel.send(net::HttpVerb::Get, QUrl(plan.checksumUrl),
                       {{QStringLiteral("Accept"), QStringLiteral("text/plain")}}, {}, 20000);
    if (!checksums.isSuccess()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "cannot read the published checksum list");
    }
    QString expected;
    const QStringList lines = QString::fromUtf8(checksums.body).split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QStringList parts =
            line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() == 2 && parts.at(1).trimmed() == plan.archiveName) {
            expected = parts.at(0).trimmed().toLower();
            break;
        }
    }
    if (expected.isEmpty()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "the archive is not listed in the published checksums",
                              plan.archiveName.toStdString());
    }
    const QString actual = sha256OfBytes(archive.body).toLower();
    if (actual != expected) {
        return makeErrorValue(
            ErrorCode::IntegrityError, "the downloaded archive does not match its checksum",
            QStringLiteral("expected %1, got %2").arg(expected, actual).toStdString());
    }

    const QString archivePath = workspace.filePath(plan.archiveName);
    {
        QSaveFile file(archivePath);
        if (!file.open(QIODevice::WriteOnly) || file.write(archive.body) != archive.body.size() ||
            !file.commit()) {
            return makeErrorValue(ErrorCode::IoError, "cannot store the downloaded archive");
        }
    }

    report(QStringLiteral("Unpacking the archive"));
    const QString extractDirectory = workspace.filePath(QStringLiteral("unpacked"));
    if (!QDir().mkpath(extractDirectory)) {
        return makeErrorValue(ErrorCode::IoError, "cannot create the extraction directory");
    }
    const QStringList extractCommand = extractionCommand(archivePath, extractDirectory);
    if (extractCommand.isEmpty()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "no archive unpacker is available",
                              "install unzip, bsdtar or python3 and try again");
    }
    const CommandOutcome extracted =
        runCommand(extractCommand.constFirst(), extractCommand.mid(1), {}, 180000);
    if (!extracted.started || extracted.exitCode != 0) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "cannot unpack the archive",
                              extracted.error.toStdString());
    }
    const QString binary = findExtractedBinary(extractDirectory);
    if (binary.isEmpty()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "the archive did not contain the expected binary", plan.archiveName.toStdString());
    }

    const QString targetDirectory = QFileInfo(plan.targetPath).absolutePath();
    if (!QDir().mkpath(targetDirectory)) {
        return makeErrorValue(ErrorCode::IoError, "cannot create the installation directory",
                              targetDirectory.toStdString());
    }

    if (plan.scope == InstallScope::System) {
#if defined(Q_OS_WIN)
        return makeErrorValue(ErrorCode::NotImplemented,
                              "elevated installation is not automatic on Windows");
#else
        report(QStringLiteral("Installing to %1 (elevation requested)").arg(plan.targetPath));
        // The verified binary is copied by install(1) under pkexec; the archive
        // is never unpacked as root.
        const CommandOutcome installed =
            runCommand(QStringLiteral("pkexec"),
                       {QStringLiteral("install"), QStringLiteral("-m"), QStringLiteral("0755"),
                        binary, plan.targetPath},
                       {}, 180000);
        if (!installed.started || installed.exitCode != 0) {
            return makeErrorValue(
                ErrorCode::PermissionDenied, "the elevated installation did not complete",
                installed.error.isEmpty()
                    ? QStringLiteral("cancelled, or no authentication agent answered")
                          .toStdString()
                    : installed.error.toStdString());
        }
#endif
    } else {
        QFile::remove(plan.targetPath);
        if (!QFile::copy(binary, plan.targetPath)) {
            return makeErrorValue(ErrorCode::IoError, "cannot write the binary",
                                  plan.targetPath.toStdString());
        }
        QFile::setPermissions(plan.targetPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                   QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                                   QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                   QFileDevice::ExeOther);
    }

    report(QStringLiteral("Checking the installed binary"));
    const CommandOutcome version =
        runCommand(plan.targetPath, {QStringLiteral("version")}, {}, 20000);
    if (!version.started || version.exitCode != 0) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "the installed binary does not run", version.error.toStdString());
    }

    InstallOutcome outcome;
    outcome.executablePath = plan.targetPath;
    outcome.version = parseVersion(version.output);
    outcome.bytesDownloaded = archive.body.size();
    outcome.sha256 = actual;
    logInfo("tools.install", QStringLiteral("rclone installed"),
            QJsonObject{{QStringLiteral("version"), outcome.version},
                        {QStringLiteral("path"), outcome.executablePath},
                        {QStringLiteral("scope"), toToken(plan.scope)}});
    return outcome;
}

} // namespace nesva::tools
