// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include "nesva/agent/protocol.hpp"
#include "nesva/core/result.hpp"

namespace nesva::agent {

/// Synchronous client for the local agent socket.
///
/// Every call has a short timeout: the UI polls agent state on a timer, so a
/// missing or hung agent must degrade to "not running" quickly instead of
/// blocking the window.
class AgentClient {
public:
    /// Defaults to the socket for the current data directory.
    explicit AgentClient(QString socketName = {});

    /// True when a request can be delivered to a listening agent.
    bool isReachable(int timeoutMs = 800);

    Result<AgentStatus> status(int timeoutMs = 2000);

    /// Starts a job in the agent. Fails when the agent is not running or busy.
    Result<QString> runJob(qint64 jobId, bool dryRun = false,
                           const QString& trigger = QStringLiteral("desktop"),
                           int timeoutMs = 3000);

    Status cancelJob(qint64 jobId, int timeoutMs = 3000);
    Status reload(int timeoutMs = 3000);
    Status shutdown(int timeoutMs = 3000);

    /// Reads the heartbeat file. Works without a running agent and reports the
    /// last known state (stopped when the agent never ran).
    static AgentStatus lastKnownStatus();

private:
    Result<QJsonObject> send(const QJsonObject& request, int timeoutMs);

    QString socketName_;
};

} // namespace nesva::agent
