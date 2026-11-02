// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "nesva/core/result.hpp"

namespace nesva::agent {

/// Commands accepted over the local socket.
namespace command {
inline constexpr const char* kStatus = "status";
inline constexpr const char* kRun = "run";
inline constexpr const char* kCancel = "cancel";
inline constexpr const char* kReload = "reload";
inline constexpr const char* kShutdown = "shutdown";
} // namespace command

/// One request line: {"command":"run","job_id":3,"dry_run":false}
struct Request {
    QString command;
    qint64 jobId = 0;
    bool dryRun = false;
    QString trigger = QStringLiteral("agent");
};

/// Parses a request; failures carry a human-readable reason.
Result<Request> parseRequest(const QJsonObject& object);
QJsonObject requestToJson(const Request& request);

/// Snapshot returned by the "status" command, and persisted as the agent
/// heartbeat so the desktop can show agent state even when it is not connected.
struct AgentStatus {
    bool running = false;
    qint64 pid = 0;
    QString version;
    QString state = QStringLiteral("stopped"); ///< starting | idle | running | stopping | stopped
    QDateTime startedAtUtc;
    QDateTime updatedAtUtc;
    qint64 runningJobId = 0;
    QString runningJobName;
    int scheduledJobs = 0;
    QDateTime nextRunUtc;
    QString lastMessage;
};

QJsonObject statusToJson(const AgentStatus& status);
Result<AgentStatus> statusFromJson(const QJsonObject& object);

/// Local socket name, namespaced per data directory so two Nesva installations
/// (or a test run) never talk to each other's agent.
QString socketName();

/// Path of the heartbeat file inside the data directory.
QString heartbeatPath();

/// Path of the single-instance lock file.
QString lockFilePath();

/// Reads the heartbeat file; returns a stopped status when it is absent.
AgentStatus readHeartbeat();
Result<QString> writeHeartbeat(const AgentStatus& status);

} // namespace nesva::agent
