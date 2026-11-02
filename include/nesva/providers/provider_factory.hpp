// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>

#include <memory>

#include "nesva/core/result.hpp"
#include "nesva/providers/storage_provider.hpp"

namespace nesva::providers {

/// Everything needed to build a concrete provider instance. Exactly one of
/// \ref localRoot and \ref rcloneRemote is used.
struct ProviderSelection {
    QString displayName;
    QString localRoot;    ///< absolute directory for a local destination
    QString rcloneRemote; ///< remote name with trailing ':' for rclone destinations
    QString basePath;     ///< directory inside an rclone remote

    bool usesRclone() const { return !rcloneRemote.trimmed().isEmpty(); }
};

/// Creates the provider described by \p selection.
///
/// \param rcloneExecutable path or name of the rclone binary; empty means
///        "rclone on PATH". Never downloaded, never bundled.
Result<std::unique_ptr<StorageProvider>> createProvider(const ProviderSelection& selection,
                                                        const QString& rcloneExecutable = {});

} // namespace nesva::providers
