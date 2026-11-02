// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/core/external_tool.hpp"

#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>

namespace nesva {

QString resolveExecutable(const QString& executable) {
    if (executable.trimmed().isEmpty()) {
        return QString();
    }
    const QFileInfo info(executable);
    if (info.isAbsolute()) {
        return info.exists() && info.isFile() ? info.absoluteFilePath() : QString();
    }
    return QStandardPaths::findExecutable(executable);
}

Result<ExternalToolInfo> probeExternalTool(const QString& executable,
                                           const QStringList& versionArguments, int timeoutMs) {
    const QString resolved = resolveExecutable(executable);
    if (resolved.isEmpty()) {
        return makeErrorValue(ErrorCode::NotFound, "external tool not found",
                              executable.toStdString());
    }

    QProcess process;
    process.setProgram(resolved);
    process.setArguments(versionArguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    // Never merge channels: stderr must stay separable for error diagnosis and
    // must not be parsed as machine-readable output.
    process.start();

    if (!process.waitForStarted(timeoutMs)) {
        return makeErrorValue(ErrorCode::ExternalToolFailed, "external tool failed to start",
                              QStringLiteral("%1: %2").arg(resolved, process.errorString()).toStdString());
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        return makeErrorValue(ErrorCode::Timeout, "external tool version probe timed out",
                              resolved.toStdString());
    }

    const QString standardOutput = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString standardError = QString::fromLocal8Bit(process.readAllStandardError());

    if (process.exitStatus() != QProcess::NormalExit) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "external tool terminated abnormally", resolved.toStdString());
    }
    if (process.exitCode() != 0) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "external tool exited with a non-zero status",
                              QStringLiteral("%1: exit %2 %3")
                                  .arg(resolved)
                                  .arg(process.exitCode())
                                  .arg(standardError.trimmed())
                                  .toStdString());
    }

    ExternalToolInfo info;
    info.executable = resolved;
    info.exitCode = process.exitCode();

    const QStringList lines = standardOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            info.versionText = trimmed;
            break;
        }
    }
    if (info.versionText.isEmpty()) {
        info.versionText = QStringLiteral("(no version output)");
    }
    return info;
}

} // namespace nesva
