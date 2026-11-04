// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/providers/local_provider.hpp"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryFile>

#include "nesva/core/logging.hpp"

namespace nesva::providers {
namespace {

constexpr qint64 kChunkSize = 256 * 1024;

/// Preserves the modification time so the archived copy keeps its identity.
/// Permissions/ACLs are deliberately not copied: an archive copy must stay
/// writable so a later run can replace it, and ACL fidelity needs the metadata
/// work planned for a later phase.
void preserveModificationTime(const QString& targetPath, const QFileInfo& sourceInfo) {
    QFile target(targetPath);
    if (target.open(QIODevice::ReadWrite)) {
        target.setFileTime(sourceInfo.lastModified(), QFileDevice::FileModificationTime);
        target.close();
    }
}

Result<QString> hashFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return makeErrorValue(ErrorCode::IoError, "cannot open file for hashing",
                              file.errorString().toStdString());
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return makeErrorValue(ErrorCode::IoError, "cannot read file for hashing",
                              path.toStdString());
    }
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace

LocalFilesystemProvider::LocalFilesystemProvider(QString rootPath)
    : rootPath_(QDir::cleanPath(QDir(rootPath).absolutePath())) {}

ProviderCapabilities LocalFilesystemProvider::capabilities() const {
    ProviderCapabilities capabilities;
    capabilities.canList = true;
    capabilities.canUpload = true;
    capabilities.canDownload = true;
    capabilities.canDelete = true;
    capabilities.canRename = true;
    capabilities.canVerifyHash = true;
    capabilities.supportsAtomicMove = true;
    capabilities.supportsVersioning = false;
    capabilities.requiresInteractiveAuth = false;
    return capabilities;
}

Result<QString> LocalFilesystemProvider::absolutePathFor(const QString& relativePath) const {
    const Result<QString> normalised = normaliseRelativePath(relativePath);
    if (!normalised) {
        return normalised.error();
    }
    return QDir(rootPath_).absoluteFilePath(normalised.value());
}

Status LocalFilesystemProvider::testConnection() {
    const QFileInfo root(rootPath_);
    if (!root.exists()) {
        return makeError(ErrorCode::NotFound, "destination directory does not exist",
                         rootPath_.toStdString());
    }
    if (!root.isDir()) {
        return makeError(ErrorCode::InvalidArgument, "destination is not a directory",
                         rootPath_.toStdString());
    }
    // Write a probe file so a read-only destination fails here rather than
    // halfway through a transfer.
    QTemporaryFile probe(QDir(rootPath_).absoluteFilePath(QStringLiteral(".nesva-probe-XXXXXX")));
    if (!probe.open()) {
        return makeError(ErrorCode::PermissionDenied, "destination directory is not writable",
                         probe.errorString().toStdString());
    }
    probe.write("nesva");
    probe.flush();
    const QString name = probe.fileName();
    probe.close();
    QFile::remove(name);
    return Status();
}

Status LocalFilesystemProvider::ensureContainer() {
    QDir directory(rootPath_);
    if (directory.exists()) {
        return Status();
    }
    if (!directory.mkpath(QStringLiteral("."))) {
        return makeError(ErrorCode::DestinationUnavailable, "cannot create destination directory",
                         rootPath_.toStdString());
    }
    return Status();
}

Result<TransferResult> LocalFilesystemProvider::uploadFile(const QString& localPath,
                                                           const QString& relativePath,
                                                   const TransferOptions& options,
                                                   const CancellationToken& cancellation,
                                                   const ProgressCallback& progress) {
    const Result<QString> target = absolutePathFor(relativePath);
    if (!target) {
        return target.error();
    }

    const QFileInfo sourceInfo(localPath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return makeErrorValue(ErrorCode::NotFound, "source file disappeared before transfer",
                              localPath.toStdString());
    }

    const QDir parentDirectory = QFileInfo(target.value()).absoluteDir();
    if (!parentDirectory.exists() && !parentDirectory.mkpath(QStringLiteral("."))) {
        return makeErrorValue(ErrorCode::DestinationUnavailable,
                              "cannot create destination directory",
                              parentDirectory.absolutePath().toStdString());
    }

    QFile source(localPath);
    if (!source.open(QIODevice::ReadOnly)) {
        return makeErrorValue(ErrorCode::IoError, "cannot open source file for reading",
                              source.errorString().toStdString());
    }

    // QSaveFile writes to a temporary file and renames on commit(): an
    // interrupted transfer leaves the destination untouched, never truncated.
    QSaveFile destination(target.value());
    if (!destination.open(QIODevice::WriteOnly)) {
        return makeErrorValue(ErrorCode::DestinationUnavailable,
                              "cannot open destination file for writing",
                              destination.errorString().toStdString());
    }

    QCryptographicHash sourceHash(QCryptographicHash::Sha256);
    const qint64 total = sourceInfo.size();
    qint64 written = 0;

    while (!source.atEnd()) {
        if (cancellation.isCancelled()) {
            destination.cancelWriting();
            return makeErrorValue(ErrorCode::Cancelled, "transfer cancelled");
        }
        const QByteArray chunk = source.read(kChunkSize);
        if (chunk.isEmpty() && source.error() != QFile::NoError) {
            destination.cancelWriting();
            return makeErrorValue(ErrorCode::IoError, "error while reading source file",
                                  source.errorString().toStdString());
        }
        sourceHash.addData(chunk);
        if (destination.write(chunk) != chunk.size()) {
            destination.cancelWriting();
            return makeErrorValue(ErrorCode::DestinationUnavailable,
                                  "error while writing destination file",
                                  destination.errorString().toStdString());
        }
        written += chunk.size();
        if (progress) {
            progress(TransferProgress{written, total, {}, 0, -1});
        }
    }

    if (!destination.commit()) {
        return makeErrorValue(ErrorCode::DestinationUnavailable,
                              "cannot finalise destination file",
                              destination.errorString().toStdString());
    }

    const QFileInfo destinationInfo(target.value());
    if (destinationInfo.size() != total) {
        // Size mismatch means the copy is not trustworthy: remove it so it
        // cannot be mistaken for a complete archive object.
        QFile::remove(target.value());
        return makeErrorValue(
            ErrorCode::IntegrityError, "size mismatch after transfer",
            QStringLiteral("expected %1 bytes, wrote %2").arg(total).arg(destinationInfo.size())
                .toStdString());
    }

    const QString expectedHash = QString::fromLatin1(sourceHash.result().toHex());
    if (options.verifyChecksum) {
        const Result<QString> destinationHash = hashFile(target.value());
        if (!destinationHash) {
            return destinationHash.error();
        }
        if (destinationHash.value() != expectedHash) {
            QFile::remove(target.value());
            return makeErrorValue(ErrorCode::IntegrityError,
                                  "checksum mismatch after transfer",
                                  relativePath.toStdString());
        }
    }

    preserveModificationTime(target.value(), sourceInfo);

    logDebug(options.category.toStdString(), QStringLiteral("file transferred"),
             QJsonObject{{QStringLiteral("path"), relativePath},
                         {QStringLiteral("bytes"), written}});

    TransferResult result;
    result.bytesTransferred = written;
    result.sha256 = options.verifyChecksum ? expectedHash : QString();
    return result;
}

Result<TransferResult> LocalFilesystemProvider::downloadFile(const QString& relativePath,
                                                             const QString& localPath,
                                                     const TransferOptions& options,
                                                     const CancellationToken& cancellation,
                                                     const ProgressCallback& progress) {
    const Result<QString> source = absolutePathFor(relativePath);
    if (!source) {
        return source.error();
    }

    const QFileInfo sourceInfo(source.value());
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return makeErrorValue(ErrorCode::NotFound, "archive object not found",
                              relativePath.toStdString());
    }

    const QDir parentDirectory = QFileInfo(localPath).absoluteDir();
    if (!parentDirectory.exists() && !parentDirectory.mkpath(QStringLiteral("."))) {
        return makeErrorValue(ErrorCode::DestinationUnavailable,
                              "cannot create destination directory",
                              parentDirectory.absolutePath().toStdString());
    }

    QFile input(source.value());
    if (!input.open(QIODevice::ReadOnly)) {
        return makeErrorValue(ErrorCode::IoError, "cannot open archive object for reading",
                              input.errorString().toStdString());
    }

    QSaveFile output(localPath);
    if (!output.open(QIODevice::WriteOnly)) {
        return makeErrorValue(ErrorCode::IoError, "cannot open restore target for writing",
                              output.errorString().toStdString());
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qint64 total = sourceInfo.size();
    qint64 written = 0;

    while (!input.atEnd()) {
        if (cancellation.isCancelled()) {
            output.cancelWriting();
            return makeErrorValue(ErrorCode::Cancelled, "restore cancelled");
        }
        const QByteArray chunk = input.read(kChunkSize);
        if (chunk.isEmpty() && input.error() != QFile::NoError) {
            output.cancelWriting();
            return makeErrorValue(ErrorCode::IoError, "error while reading archive object",
                                  input.errorString().toStdString());
        }
        hash.addData(chunk);
        if (output.write(chunk) != chunk.size()) {
            output.cancelWriting();
            return makeErrorValue(ErrorCode::IoError, "error while writing restore target",
                                  output.errorString().toStdString());
        }
        written += chunk.size();
        if (progress) {
            progress(TransferProgress{written, total, {}, 0, -1});
        }
    }

    if (!output.commit()) {
        return makeErrorValue(ErrorCode::IoError, "cannot finalise restored file",
                              output.errorString().toStdString());
    }

    preserveModificationTime(localPath, sourceInfo);

    const QString restoredHash = QString::fromLatin1(hash.result().toHex());
    if (options.verifyChecksum) {
        const Result<QString> expected = checksum(relativePath);
        if (!expected) {
            return expected.error();
        }
        if (!expected.value().isEmpty() && expected.value() != restoredHash) {
            QFile::remove(localPath);
            return makeErrorValue(ErrorCode::IntegrityError, "restored file failed verification",
                                  localPath.toStdString());
        }
    }

    TransferResult result;
    result.bytesTransferred = written;
    result.sha256 = restoredHash;
    return result;
}

Result<bool> LocalFilesystemProvider::fileExists(const QString& relativePath) {
    const Result<QString> target = absolutePathFor(relativePath);
    if (!target) {
        return target.error();
    }
    const QFileInfo info(target.value());
    return info.exists() && info.isFile();
}

Result<QVector<RemoteEntry>> LocalFilesystemProvider::listFiles(const QString& directoryRelativePath,
                                                                bool recursive) {
    QString base = rootPath_;
    if (!directoryRelativePath.trimmed().isEmpty()) {
        const Result<QString> target = absolutePathFor(directoryRelativePath);
        if (!target) {
            return target.error();
        }
        base = target.value();
    }

    const QFileInfo baseInfo(base);
    if (!baseInfo.exists()) {
        // An archive whose destination does not exist yet simply has no objects.
        return QVector<RemoteEntry>{};
    }
    if (!baseInfo.isDir()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "not a directory", base.toStdString());
    }

    QVector<RemoteEntry> entries;
    QDirIterator::IteratorFlags flags =
        QDirIterator::NoIteratorFlags | (recursive ? QDirIterator::Subdirectories
                                                   : QDirIterator::NoIteratorFlags);
    QDirIterator iterator(base, QDir::Files | QDir::NoDotAndDotDot, flags);
    const QDir rootDirectory(rootPath_);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info(path);
        if (info.isSymLink()) {
            continue;
        }
        RemoteEntry entry;
        entry.relativePath = rootDirectory.relativeFilePath(path);
        entry.sizeBytes = info.size();
        entry.isDirectory = false;
        entry.modifiedUtc = info.lastModified().toUTC();
        entries.append(entry);
    }
    return entries;
}

Result<QString> LocalFilesystemProvider::checksum(const QString& relativePath) {
    const Result<QString> target = absolutePathFor(relativePath);
    if (!target) {
        return target.error();
    }
    if (!QFileInfo::exists(target.value())) {
        return makeErrorValue(ErrorCode::NotFound, "archive object not found",
                              relativePath.toStdString());
    }
    return hashFile(target.value());
}

} // namespace nesva::providers
