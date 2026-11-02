// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDir>
#include <QtCore/QString>
#include <QtCore/QTemporaryDir>

#include <stdexcept>

#include "nesva/core/paths.hpp"

namespace nesva::test {

/// RAII temporary directory that restores the previous data-root override when
/// destroyed. Tests never touch a user's real Nesva data directory.
class ScopedDataRoot {
public:
    ScopedDataRoot() : previous_(Paths::dataRootOverride()) {
        if (!directory_.isValid()) {
            throw std::runtime_error("could not create temporary directory");
        }
        Paths::setDataRootOverride(directory_.path());
    }

    ~ScopedDataRoot() { Paths::setDataRootOverride(previous_); }

    ScopedDataRoot(const ScopedDataRoot&) = delete;
    ScopedDataRoot& operator=(const ScopedDataRoot&) = delete;

    QString path() const { return directory_.path(); }

private:
    QTemporaryDir directory_;
    QString previous_;
};

} // namespace nesva::test
