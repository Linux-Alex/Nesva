// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/providers/provider_factory.hpp"

#include "nesva/providers/local_provider.hpp"
#include "nesva/providers/rclone_provider.hpp"

namespace nesva::providers {

Result<std::unique_ptr<StorageProvider>> createProvider(const ProviderSelection& selection,
                                                        const QString& rcloneExecutable) {
    if (selection.usesRclone()) {
        return std::unique_ptr<StorageProvider>(
            std::make_unique<RcloneProvider>(selection.rcloneRemote, selection.basePath,
                                             rcloneExecutable));
    }

    if (selection.localRoot.trimmed().isEmpty()) {
        return makeErrorValue(ErrorCode::ConfigurationError,
                              "no destination configured for this job");
    }
    return std::unique_ptr<StorageProvider>(
        std::make_unique<LocalFilesystemProvider>(selection.localRoot));
}

} // namespace nesva::providers
