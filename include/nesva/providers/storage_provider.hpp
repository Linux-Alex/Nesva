// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <functional>

#include "nesva/core/cancellation.hpp"
#include "nesva/core/result.hpp"

namespace nesva::providers {

/// What a provider can actually do. Never assume all providers are equal:
/// Nesva must degrade gracefully instead of offering operations that cannot
/// work on a given backend.
struct ProviderCapabilities {
    bool canList = false;
    bool canUpload = false;
    bool canDownload = false;
    bool canDelete = false;
    bool canRename = false;
    bool canVerifyHash = false;
    bool supportsAtomicMove = false;
    bool supportsVersioning = false;
    bool requiresInteractiveAuth = false;
};

struct TransferOptions {
    qint64 bandwidthLimitKiB = 0; ///< 0 = unlimited (enforced by providers that can)
    int maxAttempts = 3;          ///< total attempts, including the first
    int retryBackoffMs = 400;     ///< base delay; doubled per retry
    bool verifyChecksum = true;   ///< read back and compare SHA-256 where supported
    QString category = QStringLiteral("provider"); ///< log category for diagnostics
};

struct TransferProgress {
    qint64 bytesDone = 0;
    qint64 bytesTotal = 0;
};

/// Outcome of a completed transfer.
struct TransferResult {
    qint64 bytesTransferred = 0;
    /// SHA-256 of the transferred payload, or empty when the provider cannot
    /// compute one. An empty hash means "unverified", never "verified".
    QString sha256;
};

using ProgressCallback = std::function<void(const TransferProgress&)>;

struct RemoteEntry {
    QString relativePath; ///< Provider-relative, '/' separated.
    qint64 sizeBytes = 0;
    bool isDirectory = false;
    QDateTime modifiedUtc;
};

/// Abstraction over a destination. Implementations must:
///  * treat paths as provider-relative and reject traversal outside the root;
///  * honour the CancellationToken at least between chunks/files;
///  * report failures as Error values, never by dropping data silently;
///  * never delete or overwrite unrelated files.
class StorageProvider {
public:
    virtual ~StorageProvider() = default;

    /// Short kind used in logs and UI ("local", "rclone").
    virtual QString kind() const = 0;

    /// Human readable location ("/srv/archive", "icloud:Nesva/Archive").
    virtual QString location() const = 0;

    virtual ProviderCapabilities capabilities() const = 0;

    /// Verifies that the destination is usable. Must not modify anything.
    virtual Status testConnection() = 0;

    /// Creates the destination container when the provider supports it.
    virtual Status ensureContainer() = 0;

    virtual Result<TransferResult> uploadFile(const QString& localPath,
                                              const QString& relativePath,
                                              const TransferOptions& options,
                                              const CancellationToken& cancellation,
                                              const ProgressCallback& progress = {}) = 0;

    virtual Result<TransferResult> downloadFile(const QString& relativePath,
                                                const QString& localPath,
                                                const TransferOptions& options,
                                                const CancellationToken& cancellation,
                                                const ProgressCallback& progress = {}) = 0;

    virtual Result<bool> fileExists(const QString& relativePath) = 0;

    virtual Result<QVector<RemoteEntry>> listFiles(const QString& directoryRelativePath,
                                                   bool recursive) = 0;

    /// SHA-256 of a remote file, or an empty string when the provider cannot
    /// compute it. Callers must treat emptiness as "unverified", never as "ok".
    virtual Result<QString> checksum(const QString& relativePath) = 0;
};

/// Normalises a provider-relative path: forward slashes, no leading '/', no
/// "." or ".." components, no empty segments. Returns ErrorCode::InvalidArgument
/// for anything that could escape the destination root.
Result<QString> normaliseRelativePath(const QString& path);

/// Joins two provider-relative paths, skipping empty parts.
QString joinRelative(const QString& base, const QString& child);

/// Converts a filesystem path (which may be absolute) into a safe relative path.
QString sanitiseForRemote(const QString& path);

} // namespace nesva::providers
