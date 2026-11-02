// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/core/hashing.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>

namespace nesva {
namespace {
constexpr qint64 kChunkSize = 256 * 1024;
}

Result<QString> sha256File(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return makeErrorValue(ErrorCode::IoError, "cannot open file for hashing",
                              file.errorString().toStdString());
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kChunkSize);
        if (chunk.isEmpty() && file.error() != QFile::NoError) {
            return makeErrorValue(ErrorCode::IoError, "error while reading file for hashing",
                                  file.errorString().toStdString());
        }
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString sha256OfBytes(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

} // namespace nesva
