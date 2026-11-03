// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/mail/exporter.hpp"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>

#include <utility>

#include "nesva/core/hashing.hpp"
#include "nesva/core/logging.hpp"
#include "nesva/db/database.hpp"
#include "nesva/mail/mail_store.hpp"

namespace nesva::mail {
namespace {

QString isoOrEmpty(const QDateTime& value) {
    return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs) : QString();
}

} // namespace

MailExporter::MailExporter(db::Database& database, std::shared_ptr<GraphClient> graph,
                           std::function<void(const MailProgress&)> progress)
    : database_(database), graph_(std::move(graph)), progress_(std::move(progress)) {}

QString MailExporter::sanitiseFileName(const QString& value, int maxLength) {
    QString name = value;
    static const QRegularExpression invalid(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]"));
    name.remove(invalid);
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    name.replace(whitespace, QStringLiteral(" "));
    name = name.trimmed();
    while (name.endsWith(QLatin1Char('.'))) {
        name.chop(1);
    }
    if (name.size() > maxLength) {
        name = name.left(maxLength).trimmed();
    }
    if (name.isEmpty()) {
        name = QStringLiteral("message");
    }
    return name;
}

QString MailExporter::buildEmlRelativePath(const QString& folderPath, const QDateTime& receivedUtc,
                                           const QString& subject, const QString& messageKey) {
    const QDateTime when = receivedUtc.isValid() ? receivedUtc : QDateTime::currentDateTimeUtc();
    const QString year = when.toUTC().toString(QStringLiteral("yyyy"));
    const QString month = when.toUTC().toString(QStringLiteral("MM"));
    const QString id = QString::fromLatin1(
        QCryptographicHash::hash(messageKey.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));

    const QStringList segments = {sanitiseFileName(folderPath, 80), year, month,
                                  sanitiseFileName(subject, 90) + QLatin1Char('-') + id +
                                      QStringLiteral(".eml")};
    return segments.join(QLatin1Char('/'));
}

Result<MailExportReport> MailExporter::run(qint64 accountId, const MailExportOptions& options,
                                           const CancellationToken& cancellation) {
    MailStore store(database_);
    const Result<MailAccount> account = store.account(accountId);
    if (!account) {
        return account.error();
    }
    if (options.destinationDirectory.trimmed().isEmpty()) {
        return makeErrorValue(ErrorCode::ConfigurationError,
                              "no destination directory was given for the mail archive");
    }
    QDir destination(options.destinationDirectory);
    if (!destination.exists() && !destination.mkpath(QStringLiteral("."))) {
        return makeErrorValue(ErrorCode::IoError, "cannot create the mail archive directory",
                              options.destinationDirectory.toStdString());
    }

    Result<QVector<MailFolderRecord>> allFolders = store.folders(accountId);
    if (!allFolders) {
        return allFolders.error();
    }

    MailExportReport report;
    int messagesSeen = 0;

    for (const MailFolderRecord& folder : allFolders.value()) {
        if (cancellation.isCancelled()) {
            report.errors.append(QStringLiteral("cancelled by the user"));
            return report;
        }
        if (!folder.syncEnabled) {
            continue;
        }
        if (!options.folderRemoteIds.isEmpty() &&
            !options.folderRemoteIds.contains(folder.remoteId)) {
            continue;
        }

        ++report.folders;
        QString cursor = options.fullResync ? QString() : folder.deltaLink;
        int pageCount = 0;

        while (true) {
            if (cancellation.isCancelled()) {
                report.errors.append(QStringLiteral("cancelled by the user"));
                return report;
            }
            ++pageCount;
            if (options.maxPagesPerFolder > 0 && pageCount > options.maxPagesPerFolder) {
                break;
            }

            const Result<DeltaPage> page = graph_->messagesDelta(folder.remoteId, cursor);
            if (!page) {
                report.errors.append(QStringLiteral("%1: %2")
                                         .arg(folder.displayName,
                                              QString::fromStdString(
                                                  page.error().toDisplayString())));
                break;
            }

            for (const MailMessageInfo& message : page.value().messages) {
                if (cancellation.isCancelled()) {
                    report.errors.append(QStringLiteral("cancelled by the user"));
                    return report;
                }
                ++messagesSeen;
                ++report.messagesSeen;
                if (options.maxMessages > 0 && report.messagesSeen > options.maxMessages) {
                    break;
                }

                if (message.removed) {
                    // The message is gone (or moved) in Microsoft 365. The local
                    // .eml is kept; only the state changes.
                    store.setMessageState(accountId, message.id, QStringLiteral("skipped"),
                                          isoOrEmpty(message.lastModifiedUtc));
                    ++report.removed;
                    continue;
                }

                const QString lastModified = isoOrEmpty(message.lastModifiedUtc);
                const Result<std::optional<MailMessageRecord>> existing =
                    store.findMessage(accountId, message.id);
                if (!existing) {
                    report.errors.append(QString::fromStdString(existing.error().toDisplayString()));
                    ++report.failed;
                    continue;
                }
                const bool unchanged = existing.value().has_value() &&
                                       existing.value()->exportState == QStringLiteral("exported") &&
                                       existing.value()->lastModifiedAt == lastModified;
                if (unchanged) {
                    ++report.skipped;
                    continue;
                }

                const Result<QByteArray> mime = graph_->messageMime(message.id);
                if (!mime) {
                    report.errors.append(QStringLiteral("%1: %2")
                                             .arg(message.subject,
                                                  QString::fromStdString(
                                                      mime.error().toDisplayString())));
                    ++report.failed;
                    continue;
                }

                const QString messageKey = message.internetMessageId.isEmpty()
                                               ? message.id
                                               : message.internetMessageId;
                const QString relative = buildEmlRelativePath(folder.displayName,
                                                              message.receivedUtc,
                                                              message.subject, messageKey);
                const QString target = destination.absoluteFilePath(relative);
                QDir().mkpath(QFileInfo(target).absolutePath());

                QSaveFile file(target);
                if (!file.open(QIODevice::WriteOnly) ||
                    file.write(mime.value()) != mime.value().size() || !file.commit()) {
                    report.errors.append(QStringLiteral("%1: cannot write %2")
                                             .arg(message.subject, relative));
                    ++report.failed;
                    continue;
                }
                report.bytesWritten += mime.value().size();

                MailMessageRecord record;
                record.remoteId = message.id;
                record.internetMessageId = message.internetMessageId;
                record.subject = message.subject;
                record.sender = message.sender;
                record.receivedAt = isoOrEmpty(message.receivedUtc);
                record.messageKey = messageKey;
                record.emlPath = relative;
                record.emlSha256 = sha256OfBytes(mime.value());
                record.exportState = QStringLiteral("exported");
                record.lastModifiedAt = lastModified;
                if (!store.upsertMessage(accountId, folder.id, record).ok()) {
                    report.errors.append(QStringLiteral("%1: cannot record the message")
                                             .arg(message.subject));
                    ++report.failed;
                    continue;
                }
                ++report.exported;

                if (progress_) {
                    progress_(MailProgress{folder.displayName, messagesSeen, report.exported,
                                           report.failed});
                }
            }

            if (!page.value().deltaLink.isEmpty()) {
                // The delta link is the resume point for the next run.
                store.setFolderDeltaLink(folder.id, page.value().deltaLink);
                break;
            }
            if (!page.value().nextLink.isEmpty()) {
                // Persist the next page too, so an interrupted run resumes here.
                store.setFolderDeltaLink(folder.id, page.value().nextLink);
                cursor = page.value().nextLink;
                continue;
            }
            break;
        }
    }

    store.updateAccountStatus(accountId, QStringLiteral("connected"),
                              db::Database::currentTimestampUtc());
    logInfo("mail.exporter", QStringLiteral("mail export finished"),
            QJsonObject{{QStringLiteral("account"), account.value().accountIdentifier},
                        {QStringLiteral("exported"), report.exported},
                        {QStringLiteral("skipped"), report.skipped},
                        {QStringLiteral("failed"), report.failed}});
    return report;
}

} // namespace nesva::mail
