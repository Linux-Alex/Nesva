// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/agent/client.hpp"

#include <QtCore/QJsonDocument>
#include <QtNetwork/QLocalSocket>

#include <utility>

namespace nesva::agent {

AgentClient::AgentClient(QString socketName)
    : socketName_(socketName.trimmed().isEmpty() ? agent::socketName() : std::move(socketName)) {}

Result<QJsonObject> AgentClient::send(const QJsonObject& request, int timeoutMs) {
    QLocalSocket socket;
    socket.connectToServer(socketName_);
    if (!socket.waitForConnected(timeoutMs)) {
        return makeErrorValue(ErrorCode::NotFound, "the background agent is not running",
                              socket.errorString().toStdString());
    }

    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(payload) != payload.size()) {
        return makeErrorValue(ErrorCode::IoError, "cannot send the request to the agent",
                              socket.errorString().toStdString());
    }
    socket.flush();

    QByteArray response;
    while (!response.endsWith('\n')) {
        if (!socket.waitForReadyRead(timeoutMs)) {
            return makeErrorValue(ErrorCode::Timeout, "the agent did not answer in time");
        }
        response += socket.readAll();
    }
    socket.disconnectFromServer();

    const QJsonDocument document = QJsonDocument::fromJson(response.trimmed());
    if (!document.isObject()) {
        return makeErrorValue(ErrorCode::Internal, "the agent returned an invalid response");
    }
    const QJsonObject object = document.object();

    if (!object.value(QStringLiteral("ok")).toBool(false)) {
        const QJsonObject error = object.value(QStringLiteral("error")).toObject();
        const QString message = error.value(QStringLiteral("message")).toString();
        return makeErrorValue(ErrorCode::Internal,
                              message.isEmpty() ? "the agent rejected the request"
                                                : message.toStdString(),
                              error.value(QStringLiteral("code")).toString().toStdString());
    }
    return object;
}

bool AgentClient::isReachable(int timeoutMs) {
    return send(QJsonObject{{QStringLiteral("command"), QString::fromUtf8(command::kStatus)}},
                timeoutMs)
        .hasValue();
}

Result<AgentStatus> AgentClient::status(int timeoutMs) {
    const Result<QJsonObject> response =
        send(QJsonObject{{QStringLiteral("command"), QString::fromUtf8(command::kStatus)}},
             timeoutMs);
    if (!response) {
        return response.error();
    }
    Result<AgentStatus> parsed = statusFromJson(response.value());
    if (parsed) {
        parsed.value().running = true;
    }
    return parsed;
}

Result<QString> AgentClient::runJob(qint64 jobId, bool dryRun, const QString& trigger,
                                    int timeoutMs) {
    Request request;
    request.command = QString::fromUtf8(command::kRun);
    request.jobId = jobId;
    request.dryRun = dryRun;
    request.trigger = trigger;

    const Result<QJsonObject> response = send(requestToJson(request), timeoutMs);
    if (!response) {
        return response.error();
    }
    return QString::fromUtf8(command::kRun);
}

Status AgentClient::cancelJob(qint64 jobId, int timeoutMs) {
    Request request;
    request.command = QString::fromUtf8(command::kCancel);
    request.jobId = jobId;
    const Result<QJsonObject> response = send(requestToJson(request), timeoutMs);
    if (!response) {
        return Status(response.error());
    }
    return Status();
}

Status AgentClient::reload(int timeoutMs) {
    const Result<QJsonObject> response =
        send(QJsonObject{{QStringLiteral("command"), QString::fromUtf8(command::kReload)}},
             timeoutMs);
    if (!response) {
        return Status(response.error());
    }
    return Status();
}

Status AgentClient::shutdown(int timeoutMs) {
    const Result<QJsonObject> response =
        send(QJsonObject{{QStringLiteral("command"), QString::fromUtf8(command::kShutdown)}},
             timeoutMs);
    if (!response) {
        return Status(response.error());
    }
    return Status();
}

AgentStatus AgentClient::lastKnownStatus() {
    return readHeartbeat();
}

} // namespace nesva::agent
