// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>
#include <memory>

#include "nesva/core/cancellation.hpp"
#include "nesva/providers/storage_provider.hpp"

namespace nesva::providers {

/// Result of running an external program.
struct ProcessOutcome {
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
    bool started = true;
    bool timedOut = false;
    bool cancelled = false;
    QString startError;
};

/// Abstraction over "run a program with an argument array".
///
/// Injected into \ref RcloneProvider so the argument construction, output
/// parsing, retry and error mapping can be tested deterministically, without a
/// live remote and without pretending that a cloud transfer happened.
class ExternalRunner {
public:
    virtual ~ExternalRunner() = default;

    virtual ProcessOutcome run(const QString& program, const QStringList& arguments,
                               const CancellationToken& cancellation, int timeoutMs) = 0;

    /// Runs a program and reports its output as it arrives, so a long transfer
    /// can show live progress. The default implementation calls run() and
    /// delivers the output once at the end, which is enough for scripted tests.
    virtual ProcessOutcome runStreaming(const QString& program, const QStringList& arguments,
                                        const CancellationToken& cancellation, int timeoutMs,
                                        const std::function<void(const QByteArray&)>& onOutput) {
        ProcessOutcome outcome = run(program, arguments, cancellation, timeoutMs);
        if (onOutput && !outcome.standardOutput.isEmpty()) {
            onOutput(outcome.standardOutput);
        }
        return outcome;
    }
};

/// Production runner: QProcess, separate channels, no shell, kill on cancel.
class ProcessRunner final : public ExternalRunner {
public:
    ProcessOutcome run(const QString& program, const QStringList& arguments,
                       const CancellationToken& cancellation, int timeoutMs) override;

    ProcessOutcome runStreaming(const QString& program, const QStringList& arguments,
                                const CancellationToken& cancellation, int timeoutMs,
                                const std::function<void(const QByteArray&)>& onOutput) override;
};

/// Destination backed by an Rclone remote (SFTP, S3-compatible, WebDAV,
/// iCloud Drive, ...).
///
/// Nesva never implements provider APIs itself: it drives the user's installed
/// Rclone through argument arrays and parses Rclone's machine-readable JSON.
class RcloneProvider final : public StorageProvider {
public:
    /// \param remoteSpec remote name with trailing ':' (for example "icloud:")
    /// \param basePath   directory inside the remote (for example "Nesva/Archive")
    /// \param executable rclone binary (absolute path or name on PATH)
    /// \param runner     process runner; defaults to ProcessRunner
    RcloneProvider(QString remoteSpec, QString basePath, QString executable,
                   std::shared_ptr<ExternalRunner> runner = nullptr);

    QString kind() const override { return QStringLiteral("rclone"); }
    QString location() const override;
    ProviderCapabilities capabilities() const override;

    Status testConnection() override;
    Status ensureContainer() override;

    Result<TransferResult> uploadFile(const QString& localPath, const QString& relativePath,
                                      const TransferOptions& options,
                                      const CancellationToken& cancellation,
                                      const ProgressCallback& progress = {}) override;

    Result<TransferResult> downloadFile(const QString& relativePath, const QString& localPath,
                                        const TransferOptions& options,
                                        const CancellationToken& cancellation,
                                        const ProgressCallback& progress = {}) override;

    Result<bool> fileExists(const QString& relativePath) override;
    Result<QVector<RemoteEntry>> listFiles(const QString& directoryRelativePath,
                                           bool recursive) override;
    Result<QString> checksum(const QString& relativePath) override;
    Result<qint64> freeSpaceBytes() override;
    Status uploadTree(const QString& localRoot, const QString& remoteRelativeBase,
                      const TransferOptions& options, const CancellationToken& cancellation,
                      const ProgressCallback& progress = {}) override;
    Status checkTree(const QString& localRoot, const QString& remoteRelativeBase, bool sizeOnly,
                     const CancellationToken& cancellation) override;

    /// Remote target string for a provider-relative path, e.g.
    /// "icloud:Nesva/Archive/Documents/a.txt". Exposed for CLI output and tests.
    Result<QString> remoteTargetFor(const QString& relativePath) const;

    /// Full argument vector used to upload a file, without running it.
    /// Exposed so tests can assert the exact argv (no shell, no quoting).
    static QStringList uploadArguments(const QString& localPath, const QString& remoteTarget,
                                       const TransferOptions& options);

    /// Parses the JSON emitted by "rclone lsjson".
    static Result<QVector<RemoteEntry>> parseListing(const QByteArray& json);

private:
    Result<ProcessOutcome> runRclone(const QStringList& arguments,
                                     const CancellationToken& cancellation, int timeoutMs);

    QString remoteSpec_;
    QString basePath_;
    QString executable_;
    std::shared_ptr<ExternalRunner> runner_;
};

} // namespace nesva::providers
