// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>

#include "nesva/providers/storage_provider.hpp"

namespace nesva::providers {

/// Destination on the local filesystem (internal disk or mounted volume).
///
/// Transfers are atomic: a file is written to a temporary sibling and only
/// renamed into place after the data is flushed, so an interrupted run cannot
/// leave a truncated file that looks complete. Size is always verified and
/// SHA-256 is verified when requested.
class LocalFilesystemProvider final : public StorageProvider {
public:
    explicit LocalFilesystemProvider(QString rootPath);

    QString kind() const override { return QStringLiteral("local"); }
    QString location() const override { return rootPath_; }
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

    /// Absolute path of a provider-relative path; used by tests and by the
    /// restore preview to show the real target.
    Result<QString> absolutePathFor(const QString& relativePath) const;

private:
    QString rootPath_;
};

} // namespace nesva::providers
