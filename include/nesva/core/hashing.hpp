// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QString>

#include "nesva/core/result.hpp"

namespace nesva {

/// SHA-256 of a file as lower-case hex. Streams the file; never loads it whole.
Result<QString> sha256File(const QString& path);

/// SHA-256 of the given bytes as lower-case hex.
QString sha256OfBytes(const QByteArray& data);

} // namespace nesva
