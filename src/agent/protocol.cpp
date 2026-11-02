// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/agent/protocol.hpp"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>

#include "nesva/core/app_info.hpp"
#include "nesva/core/paths.hpp"

namespace nesva::agent {
namespace {

QString timestampOf(const QDateTime& value) {
    return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime parseTimestamp(const QString& value) {
    if (value.isEmpty()) {
        return QDateTime();
    }
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    return parsed.isValid() ? parsed.toUTC() : QDateTime();
}

} // namespace

Result<Request> parseRequest(const QJsonObject& object) {
    const QString command = object.value(QStringLiteral("command")).toString().trimmed().toLower();
    if (command.isEmpty()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "request has no command");
    }

    static const QStringList known = {QString::fromUtf8(command::kStatus),
                                      QString::fromUtf8(command::kRun),
                                      QString::fromUtf8(command::kCancel),
                                      QString::fromUtf8(command::kReload),
                                      QString::fromUtf8(command::kShutdown)};
    if (!known.contains(command)) {
        return makeErrorValue(ErrorCode::InvalidArgument,
                              "unknown command: " + command.toStdString());
    }

    Request request;
    request.command = command;
    request.jobId = static_cast<qint64>(object.value(QStringLiteral("job_id")).toDouble());
    request.dryRun = object.value(QStringLiteral("dry_run")).toBool(false);
    const QString trigger = object.value(QStringLiteral("trigger")).toString();
    if (!trigger.trimmed().isEmpty()) {
        request.trigger = trigger.trimmed();
    }

    if ((command == QString::fromUtf8(command::kRun) ||
         command == QString::fromUtf8(command::kCancel)) &&
        request.jobId <= 0) {
        return makeErrorValue(ErrorCode::InvalidArgument, "the command requires a job id");
    }
    return request;
}

QJsonObject requestToJson(const Request& request) {
    QJsonObject object;
    object.insert(QStringLiteral("command"), request.command);
    if (request.jobId > 0) {
        object.insert(QStringLiteral("job_id"), request.jobId);
    }
    object.insert(QStringLiteral("dry_run"), request.dryRun);
    object.insert(QStringLiteral("trigger"), request.trigger);
    return object;
}

QJsonObject statusToJson(const AgentStatus& status) {
    QJsonObject object;
    object.insert(QStringLiteral("running"), status.running);
    object.insert(QStringLiteral("pid"), status.pid);
    object.insert(QStringLiteral("version"), status.version);
    object.insert(QStringLiteral("state"), status.state);
    object.insert(QStringLiteral("started_at"), timestampOf(status.startedAtUtc));
    object.insert(QStringLiteral("updated_at"), timestampOf(status.updatedAtUtc));
    object.insert(QStringLiteral("running_job_id"), status.runningJobId);
    object.insert(QStringLiteral("running_job_name"), status.runningJobName);
    object.insert(QStringLiteral("scheduled_jobs"), status.scheduledJobs);
    object.insert(QStringLiteral("next_run_at"), timestampOf(status.nextRunUtc));
    object.insert(QStringLiteral("last_message"), status.lastMessage);
    return object;
}

Result<AgentStatus> statusFromJson(const QJsonObject& object) {
    AgentStatus status;
    status.running = object.value(QStringLiteral("running")).toBool(false);
    status.pid = static_cast<qint64>(object.value(QStringLiteral("pid")).toDouble());
    status.version = object.value(QStringLiteral("version")).toString();
    const QString state = object.value(QStringLiteral("state")).toString();
    status.state = state.isEmpty() ? QStringLiteral("unknown") : state;
    status.startedAtUtc = parseTimestamp(object.value(QStringLiteral("started_at")).toString());
    status.updatedAtUtc = parseTimestamp(object.value(QStringLiteral("updated_at")).toString());
    status.runningJobId =
        static_cast<qint64>(object.value(QStringLiteral("running_job_id")).toDouble());
    status.runningJobName = object.value(QStringLiteral("running_job_name")).toString();
    status.scheduledJobs = object.value(QStringLiteral("scheduled_jobs")).toInt(0);
    status.nextRunUtc = parseTimestamp(object.value(QStringLiteral("next_run_at")).toString());
    status.lastMessage = object.value(QStringLiteral("last_message")).toString();
    return status;
}

QString socketName() {
    const QByteArray root = Paths::dataRoot().toUtf8();
    const QByteArray digest =
        QCryptographicHash::hash(root, QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("nesva-agent-") + QString::fromLatin1(digest);
}

QString heartbeatPath() {
    return QDir(Paths::dataRoot()).absoluteFilePath(QStringLiteral("agent.state.json"));
}

QString lockFilePath() {
    return QDir(Paths::dataRoot()).absoluteFilePath(QStringLiteral("agent.lock"));
}

AgentStatus readHeartbeat() {
    QFile file(heartbeatPath());
    if (!file.open(QIODevice::ReadOnly)) {
        AgentStatus status;
        status.running = false;
        status.state = QStringLiteral("stopped");
        return status;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        AgentStatus status;
        status.running = false;
        status.state = QStringLiteral("stopped");
        return status;
    }
    Result<AgentStatus> parsed = statusFromJson(document.object());
    if (!parsed) {
        AgentStatus status;
        status.running = false;
        status.state = QStringLiteral("stopped");
        return status;
    }
    return parsed.value();
}

Result<QString> writeHeartbeat(const AgentStatus& status) {
    QSaveFile file(heartbeatPath());
    if (!file.open(QIODevice::WriteOnly)) {
        return makeErrorValue(ErrorCode::IoError, "cannot write the agent heartbeat",
                              file.errorString().toStdString());
    }
    const QByteArray payload = QJsonDocument(statusToJson(status)).toJson(QJsonDocument::Indented);
    file.write(payload);
    if (!file.commit()) {
        return makeErrorValue(ErrorCode::IoError, "cannot finalise the agent heartbeat",
                              file.errorString().toStdString());
    }
    return heartbeatPath();
}

} // namespace nesva::agent
