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

// Declared before the tool specs that use them.
QString releaseArchitecture();
QString parseVersion(const QString& output);

constexpr const char* kRcloneReleaseBase = "https://downloads.rclone.org";
constexpr const char* kResticReleaseBase = "https://github.com/restic/restic/releases/download";
constexpr const char* kResticApiLatest = "https://api.github.com/repos/restic/restic/releases/latest";

/// Where a tool's release lives and how its asset is named. The two projects
/// differ in all three: rclone publishes a per-version directory with a
/// "version.txt", restic publishes GitHub release assets compressed with zip on
/// Windows and bzip2 on Linux.
struct ToolSpec {
    ToolId id;
    QString token;
    QString display;
    QString versionUrl;
    QString downloadBase;  ///< prefix of the asset URL (version appended by caller)
    QString checksumUrl;
    bool bzipped = false;  ///< asset is a lone .bz2 stream rather than a zip
};

QString releasePlatformFor(const ToolSpec& spec) {
#if defined(Q_OS_WIN)
    Q_UNUSED(spec);
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return spec.id == ToolId::Rclone ? QStringLiteral("osx") : QStringLiteral("darwin");
#else
    Q_UNUSED(spec);
    return QStringLiteral("linux");
#endif
}

ToolSpec specFor(ToolId tool) {
    ToolSpec spec;
    spec.id = tool;
    if (tool == ToolId::Restic) {
        spec.token = QStringLiteral("restic");
        spec.display = QStringLiteral("Restic");
        spec.versionUrl = QString::fromUtf8(kResticApiLatest);
        spec.downloadBase = QString::fromUtf8(kResticReleaseBase);
#if !defined(Q_OS_WIN)
        spec.bzipped = true;
#endif
    } else {
        spec.token = QStringLiteral("rclone");
        spec.display = QStringLiteral("Rclone");
        spec.versionUrl = QStringLiteral("%1/version.txt").arg(kRcloneReleaseBase);
        spec.downloadBase = QString::fromUtf8(kRcloneReleaseBase);
        spec.checksumUrl = QString();
    }
    return spec;
}

/// Release asset name for a resolved version.
QString assetNameFor(const ToolSpec& spec, const QString& version) {
    if (spec.id == ToolId::Rclone) {
        return QStringLiteral("rclone-v%1-%2-%3.zip")
            .arg(version, releasePlatformFor(spec), releaseArchitecture());
    }
#if defined(Q_OS_WIN)
    return QStringLiteral("restic_%1_%2_%3.zip")
        .arg(version, releasePlatformFor(spec), releaseArchitecture());
#else
    return QStringLiteral("restic_%1_%2_%3.bz2")
        .arg(version, releasePlatformFor(spec), releaseArchitecture());
#endif
}

QString downloadUrlFor(const ToolSpec& spec, const QString& version) {
    const QString asset = assetNameFor(spec, version);
    if (spec.id == ToolId::Rclone) {
        return QStringLiteral("%1/v%2/%3").arg(spec.downloadBase, version, asset);
    }
    return QStringLiteral("%1/v%2/%3").arg(spec.downloadBase, version, asset);
}

QString checksumUrlFor(const ToolSpec& spec, const QString& version) {
    if (spec.id == ToolId::Rclone) {
        return QStringLiteral("%1/v%2/SHA256SUMS").arg(spec.downloadBase, version);
    }
    return QStringLiteral("%1/v%2/SHA256SUMS").arg(spec.downloadBase, version);
}

/// Version reported by the release endpoint.
QString parseReleaseVersion(const ToolSpec& spec, const QByteArray& body) {
    if (spec.id == ToolId::Rclone) {
        return parseVersion(QString::fromUtf8(body));
    }
    // GitHub: {"tag_name": "v0.19.1", ...}
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    QString tag = object.value(QStringLiteral("tag_name")).toString().trimmed();
    if (tag.startsWith(QLatin1Char('v'))) {
        tag.remove(0, 1);
    }
    return tag;
}

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

QString binaryNameFor(const ToolId tool) {
    const QString base = tool == ToolId::Restic ? QStringLiteral("restic")
                                                : QStringLiteral("rclone");
#if defined(Q_OS_WIN)
    return base + QStringLiteral(".exe");
#else
    return base;
#endif
}

/// Command that unpacks the release archive, or an empty list when no usable
/// unpacker is installed. Every candidate is a normal program with an argument
/// vector; none of them is a shell.
QStringList extractionCommand(const QString& archivePath, const QString& destination,
                              bool bzipped = false) {
    struct Candidate {
        const char* program;
        QStringList arguments;
    };
    QVector<Candidate> candidates;
    if (bzipped) {
        // A lone bzip2 stream: one binary, no archive. Every candidate keeps the
        // input and writes the decompressed file next to it.
        candidates.append({"bunzip2", {QStringLiteral("-k"), QStringLiteral("-f"), archivePath}});
        candidates.append(
            {"bsdtar", {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), destination}});
        candidates.append({"7z", {QStringLiteral("x"), QStringLiteral("-y"),
                                  QStringLiteral("-o") + destination, archivePath}});
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
QString findExtractedBinary(const QString& directory, const ToolId tool) {
    QDirIterator iterator(directory, QStringList{binaryNameFor(tool)}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        return iterator.next();
    }
    return {};
}

QString parseVersion(const QString& output) {
    // "rclone v1.75.2" and "restic 0.19.1 compiled with go1.24..." -> "1.75.2" / "0.19.1"
    const QString first = output.section(QLatin1Char('\n'), 0, 0).trimmed();
    QString value = first;
    value.remove(QStringLiteral("rclone"));
    value.remove(QStringLiteral("restic"));
    value = value.trimmed();
    if (value.startsWith(QLatin1Char('v'))) {
        value.remove(0, 1);
    }
    // Keep only the leading version token.
    static const QRegularExpression versionPattern(QStringLiteral("^[0-9]+\\.[0-9]+(\\.[0-9]+)?"));
    const QRegularExpressionMatch match = versionPattern.match(value);
    return match.hasMatch() ? match.captured(0) : value.section(QLatin1Char(' '), 0, 0);
}

} // namespace

QString toToken(InstallScope scope) {
    return scope == InstallScope::System ? QStringLiteral("system") : QStringLiteral("user");
}

QString toToken(const ToolId tool) {
    return specFor(tool).token;
}

QString displayName(const ToolId tool) {
    return specFor(tool).display;
}

QString userInstallDirectory(const ToolId tool) {
#if defined(Q_OS_WIN)
    const QString localAppData =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (localAppData.isEmpty()) {
        return {};
    }
    return localAppData + QStringLiteral("/Programs/") + specFor(tool).token;
#else
    Q_UNUSED(tool);
    const QString home = QDir::homePath();
    if (home.isEmpty()) {
        return {};
    }
    return home + QStringLiteral("/.local/bin");
#endif
}

QString userInstallDirectory() {
#if defined(Q_OS_WIN)
    const QString localAppData =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return localAppData.isEmpty() ? QString() : localAppData + QStringLiteral("/Programs");
#else
    const QString home = QDir::homePath();
    return home.isEmpty() ? QString() : home + QStringLiteral("/.local/bin");
#endif
}

QString toolTargetPath(const ToolId tool, const InstallScope scope) {
    if (scope == InstallScope::User) {
        const QString directory = userInstallDirectory(tool);
        return directory.isEmpty() ? QString()
                                   : directory + QLatin1Char('/') + binaryNameFor(tool);
    }
#if defined(Q_OS_WIN)
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    const QString base = programFiles.isEmpty() ? QStringLiteral("C:/Program Files") : programFiles;
    return base + QLatin1Char('/') + specFor(tool).token + QLatin1Char('/') +
           binaryNameFor(tool);
#else
    return QStringLiteral("/usr/local/bin/") + binaryNameFor(tool);
#endif
}

QString rcloneTargetPath(const InstallScope scope) {
    return toolTargetPath(ToolId::Rclone, scope);
}

QString resticTargetPath(const InstallScope scope) {
    return toolTargetPath(ToolId::Restic, scope);
}

Result<ToolInstallPlan> planToolInstall(const ToolId tool, const InstallScope scope,
                                        net::HttpTransport& transport) {
    const ToolSpec spec = specFor(tool);

    ToolInstallPlan plan;
    plan.tool = tool;
    plan.toolToken = spec.token;
    plan.toolDisplayName = spec.display;
    plan.scope = scope;
    plan.requiresElevation = scope == InstallScope::System;
    plan.targetPath = toolTargetPath(tool, scope);
    if (plan.targetPath.isEmpty()) {
        return makeErrorValue(ErrorCode::ConfigurationError,
                              "this system has no per-user program directory to install into");
    }

    // rclone serves a plain-text version file, the GitHub API only answers JSON
    // clients and rejects anything else with HTTP 415.
    const QString accept = spec.id == ToolId::Rclone
                               ? QStringLiteral("text/plain")
                               : QStringLiteral("application/vnd.github+json");
    const net::HttpResponse version = transport.send(
        net::HttpVerb::Get, QUrl(spec.versionUrl),
        {{QStringLiteral("Accept"), accept}}, {}, 20000);
    if (version.statusCode == 0) {
        return makeErrorValue(
            ErrorCode::ExternalToolFailed,
            QStringLiteral("cannot reach the %1 download server").arg(spec.token).toStdString(),
            version.error.toStdString());
    }
    if (!version.isSuccess()) {
        return makeErrorValue(
            ErrorCode::ExternalToolFailed,
            QStringLiteral("the %1 download server refused the request").arg(spec.token).toStdString(),
            QStringLiteral("%1: %2")
                .arg(version.statusCode)
                .arg(QString::fromUtf8(version.body.left(200)))
                .toStdString());
    }
    plan.version = parseReleaseVersion(spec, version.body);
    if (plan.version.isEmpty()) {
        return makeErrorValue(
            ErrorCode::ExternalToolFailed,
            QStringLiteral("the %1 download server returned no usable version")
                .arg(spec.token)
                .toStdString(),
            QString::fromUtf8(version.body.left(120)).toStdString());
    }

    plan.archiveName = assetNameFor(spec, plan.version);
    plan.downloadUrl = downloadUrlFor(spec, plan.version);
    plan.checksumUrl = checksumUrlFor(spec, plan.version);

    InstallStep download;
    download.title = QStringLiteral("Download %1 %2 to a private temporary directory")
                         .arg(spec.display, plan.version);
    download.note = plan.downloadUrl;
    plan.steps.append(download);

    InstallStep verify;
    verify.title = QStringLiteral("Compute its SHA-256 and compare it with the published checksum");
    verify.note = plan.checksumUrl;
    plan.steps.append(verify);

    InstallStep extract;
    extract.title = spec.bzipped ? QStringLiteral("Decompress the bzip2 archive")
                                 : QStringLiteral("Unpack the archive");
    const QStringList extractCommand = extractionCommand(
        QStringLiteral("<archive>"), QStringLiteral("<temporary directory>"), spec.bzipped);
    extract.command = extractCommand;
    if (extractCommand.isEmpty()) {
        extract.note = spec.bzipped
                           ? QStringLiteral("No decompressor found: install bzip2 (bunzip2) or "
                                            "bsdtar first.")
                           : QStringLiteral("No unpacker found: install one of unzip, bsdtar, "
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
                                      "Administrator terminal and copy the extracted %1 to %2 "
                                      "yourself.")
                           .arg(binaryNameFor(tool), plan.targetPath);
#else
        install.command = {QStringLiteral("pkexec"), QStringLiteral("install"),
                           QStringLiteral("-m"), QStringLiteral("0755"),
                           QStringLiteral("<verified binary>"), plan.targetPath};
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
                       "compromised server. %1 publishes no signature Nesva can verify.")
            .arg(spec.display));
    plan.notes.append(
        QStringLiteral("Nesva stores the installed path, so the tool works even if %1 is not on "
                       "your PATH.")
            .arg(QFileInfo(plan.targetPath).absolutePath()));
#if defined(Q_OS_WIN)
    plan.notes.append(QStringLiteral("%1 publishes unsigned Windows builds; SmartScreen may warn "
                                     "the first time it runs.")
                          .arg(spec.display));
#endif
    if (tool == ToolId::Restic) {
        plan.notes.append(QStringLiteral("Restic needs a password for every repository; Nesva keeps "
                                         "it in the system keyring and passes it to restic through a "
                                         "temporary file that is deleted immediately."));
    }
    return plan;
}

Result<ToolInstallPlan> planRcloneInstall(const InstallScope scope, net::HttpTransport& transport) {
    return planToolInstall(ToolId::Rclone, scope, transport);
}

Result<ToolInstallPlan> planResticInstall(const InstallScope scope, net::HttpTransport& transport) {
    return planToolInstall(ToolId::Restic, scope, transport);
}

QString describeInstallPlan(const ToolInstallPlan& plan) {
    QString text = QStringLiteral("%1 %2 -> %3 (%4 scope)")
                       .arg(plan.toolDisplayName, plan.version, plan.targetPath,
                            toToken(plan.scope));
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

Result<InstallOutcome> executeToolInstall(const ToolInstallPlan& plan,
                                          const std::function<void(const QString&)>& progress,
                                          net::HttpTransport* transport) {
    const ToolSpec spec = specFor(plan.tool);
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
    const QStringList extractCommand =
        extractionCommand(archivePath, extractDirectory, spec.bzipped);
    if (extractCommand.isEmpty()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "no archive unpacker is available",
                              spec.bzipped
                                  ? "install bzip2 (bunzip2) or bsdtar and try again"
                                  : "install unzip, bsdtar or python3 and try again");
    }
    const CommandOutcome extracted =
        runCommand(extractCommand.constFirst(), extractCommand.mid(1), {}, 180000);
    if (!extracted.started || extracted.exitCode != 0) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "cannot unpack the archive",
                              extracted.error.toStdString());
    }
    QString binary = findExtractedBinary(extractDirectory, plan.tool);
    if (binary.isEmpty() && spec.bzipped) {
        // A decompressor writes the payload next to the archive, still named after
        // the release asset ("restic_0.19.1_linux_amd64"), not after the tool.
        binary = findExtractedBinary(workspace.path(), plan.tool);
        if (binary.isEmpty()) {
            const QString decompressed = workspace.filePath(
                QFileInfo(plan.archiveName).completeBaseName());
            if (QFileInfo(decompressed).isFile()) {
                binary = decompressed;
            }
        }
    }
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
    logInfo("tools.install", QStringLiteral("%1 installed").arg(spec.token),
            QJsonObject{{QStringLiteral("version"), outcome.version},
                        {QStringLiteral("path"), outcome.executablePath},
                        {QStringLiteral("scope"), toToken(plan.scope)}});
    return outcome;
}

Result<InstallOutcome> executeRcloneInstall(const ToolInstallPlan& plan,
                                            const std::function<void(const QString&)>& progress,
                                            net::HttpTransport* transport) {
    return executeToolInstall(plan, progress, transport);
}

} // namespace nesva::tools