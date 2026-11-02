// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/security/secret_service_linux.hpp"

#if defined(Q_OS_LINUX)

#include <QtCore/QMetaType>
#include <QtCore/QVariant>
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusObjectPath>
#include <QtDBus/QDBusReply>

#include <utility>

#include "nesva/core/logging.hpp"

namespace nesva::security {
namespace {

constexpr const char* kService = "org.freedesktop.secrets";
constexpr const char* kPath = "/org/freedesktop/secrets";
constexpr const char* kServiceInterface = "org.freedesktop.Secret.Service";
constexpr const char* kItemInterface = "org.freedesktop.Secret.Item";
constexpr int kTimeoutMs = 5000;

QVariantMap attributesFor(const QString& service, const QString& account) {
    QVariantMap attributes;
    attributes.insert(QStringLiteral("application"), QStringLiteral("nesva"));
    attributes.insert(QStringLiteral("service"), service);
    attributes.insert(QStringLiteral("account"), account);
    return attributes;
}

/// Serialises the Secret Service "Secret" struct:
/// (o session, ay parameters, ay value, s content_type)
QDBusArgument packSecret(const QString& sessionPath, const QByteArray& value) {
    QDBusArgument argument;
    argument.beginStructure();
    argument << QDBusObjectPath(sessionPath);
    argument << QByteArray();
    argument << value;
    argument << QStringLiteral("text/plain");
    argument.endStructure();
    return argument;
}

QByteArray unpackSecret(const QDBusArgument& argument) {
    QString session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
    argument.beginStructure();
    argument >> session >> parameters >> value >> contentType;
    argument.endStructure();
    return value;
}

/// The Secret Service spec types SearchItems' attributes as a{ss}. Qt marshals
/// QVariantMap as a{sv}, so the map is built through QDBusArgument explicitly.
QDBusArgument stringMapArgument(const QVariantMap& values) {
    QDBusArgument argument;
    argument.beginMap(QMetaType::fromType<QString>(), QMetaType::fromType<QString>());
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        argument.beginMapEntry();
        argument << it.key() << it.value().toString();
        argument.endMapEntry();
    }
    argument.endMap();
    return argument;
}

/// CreateItem properties are typed a{sv}, and the Attributes entry inside is
/// itself a{ss}. Qt's QVariantMap marshals as a{sv} (and nested maps as a{sv}),
/// so both levels are built explicitly.
QDBusArgument itemPropertiesArgument(const QString& label, const QVariantMap& attributes) {
    QDBusArgument argument;
    argument.beginMap(QMetaType::fromType<QString>(), QMetaType::fromType<QDBusVariant>());

    argument.beginMapEntry();
    argument << QStringLiteral("org.freedesktop.Secret.Item.Label")
             << QDBusVariant(QVariant(label));
    argument.endMapEntry();

    argument.beginMapEntry();
    argument << QStringLiteral("org.freedesktop.Secret.Item.Attributes")
             << QDBusVariant(QVariant::fromValue(stringMapArgument(attributes)));
    argument.endMapEntry();

    argument.endMap();
    return argument;
}

[[maybe_unused]] QDBusMessage searchItems(QDBusInterface& iface, const QVariantMap& attributes) {
    QDBusMessage message = QDBusMessage::createMethodCall(
        iface.service(), iface.path(), iface.interface(), QStringLiteral("SearchItems"));
    message << QVariant::fromValue(stringMapArgument(attributes));
    iface.setTimeout(kTimeoutMs);
    return iface.connection().call(message, QDBus::Block, kTimeoutMs);
}

} // namespace

SecretServiceStore::SecretServiceStore() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        reason_ = QStringLiteral("no session bus is available");
        return;
    }

    QDBusConnectionInterface* interface = bus.interface();
    if (interface == nullptr ||
        !interface->isServiceRegistered(QString::fromLatin1(kService))) {
        reason_ = QStringLiteral("org.freedesktop.secrets is not registered on the session bus");
        return;
    }

    QDBusInterface service(QString::fromLatin1(kService), QString::fromLatin1(kPath),
                           QString::fromLatin1(kServiceInterface), bus);
    service.setTimeout(kTimeoutMs);
    if (!service.isValid()) {
        reason_ = QStringLiteral("the Secret Service interface could not be resolved");
        return;
    }

    // A "plain" session keeps the secret unencrypted inside the D-Bus channel
    // (a per-user Unix socket) and lets the service store it in its own
    // encrypted store. Nesva never sees the service's encryption keys.
    QDBusMessage reply = service.call(
        QStringLiteral("OpenSession"), QStringLiteral("plain"),
        QVariant::fromValue(QDBusVariant(QVariant(QString()))));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        reason_ = QStringLiteral("the Secret Service refused to open a session: %1")
                      .arg(reply.errorMessage());
        return;
    }

    const QDBusObjectPath session = qdbus_cast<QDBusObjectPath>(reply.arguments().at(1));
    sessionPath_ = session.path();
    available_ = !sessionPath_.isEmpty() && sessionPath_ != QStringLiteral("/");
    if (!available_) {
        reason_ = QStringLiteral("the Secret Service returned an invalid session path");
    }
}

Status SecretServiceStore::store(const QString& service, const QString& account,
                                 const QByteArray& secret) {
    if (!available_) {
        return makeError(ErrorCode::Unsupported, "no Secret Service session", reason_.toStdString());
    }

    // CreateItem belongs to the Collection interface, so the default collection
    // has to be resolved first (ReadAlias on the Service interface).
    QDBusInterface service_interface(QString::fromLatin1(kService), QString::fromLatin1(kPath),
                                     QString::fromLatin1(kServiceInterface),
                                     QDBusConnection::sessionBus());
    service_interface.setTimeout(kTimeoutMs);
    QDBusMessage aliasReply = service_interface.call(QStringLiteral("ReadAlias"),
                                                     QStringLiteral("default"));
    if (aliasReply.type() != QDBusMessage::ReplyMessage || aliasReply.arguments().isEmpty()) {
        return makeError(ErrorCode::PermissionDenied, "the credential store has no default collection",
                         aliasReply.errorMessage().toStdString());
    }
    const QString collectionPath =
        qdbus_cast<QDBusObjectPath>(aliasReply.arguments().at(0)).path();
    if (collectionPath.isEmpty() || collectionPath == QStringLiteral("/")) {
        return makeError(ErrorCode::PermissionDenied,
                         "the credential store has no default collection",
                         "create/unlock a keyring collection and retry");
    }

    QDBusInterface collection(QString::fromLatin1(kService), collectionPath,
                              QStringLiteral("org.freedesktop.Secret.Collection"),
                              QDBusConnection::sessionBus());
    collection.setTimeout(kTimeoutMs);

    const QDBusArgument properties = itemPropertiesArgument(
        QStringLiteral("Nesva: %1 (%2)").arg(service, account), attributesFor(service, account));
    const QDBusArgument packedSecret = packSecret(sessionPath_, secret);
    QDBusMessage reply = collection.callWithArgumentList(
        QDBus::Block, QStringLiteral("CreateItem"),
        {QVariant::fromValue(properties), QVariant::fromValue(packedSecret), QVariant(true)});

    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        return makeError(ErrorCode::PermissionDenied, "the credential store refused to save the secret",
                         reply.errorMessage().toStdString());
    }

    const QDBusObjectPath prompt = qdbus_cast<QDBusObjectPath>(reply.arguments().at(1));
    if (prompt.path() != QStringLiteral("/")) {
        return makeError(ErrorCode::PermissionDenied,
                         "the credential store requires approval to save the secret",
                         "unlock the keyring (or approve the prompt) and retry");
    }
    return Status();
}

Result<QByteArray> SecretServiceStore::lookup(const QString& service, const QString& account) {
    if (!available_) {
        return makeErrorValue(ErrorCode::Unsupported, "no Secret Service session",
                              reason_.toStdString());
    }

    QDBusInterface iface(QString::fromLatin1(kService), QString::fromLatin1(kPath),
                         QString::fromLatin1(kServiceInterface), QDBusConnection::sessionBus());
    iface.setTimeout(kTimeoutMs);

    QDBusMessage reply = searchItems(iface, attributesFor(service, account));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        return makeErrorValue(ErrorCode::DatabaseError, "the credential store search failed",
                              reply.errorMessage().toStdString());
    }

    const QList<QDBusObjectPath> unlocked =
        qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));
    const QList<QDBusObjectPath> locked =
        qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(1));

    if (unlocked.isEmpty()) {
        if (!locked.isEmpty()) {
            return makeErrorValue(ErrorCode::PermissionDenied,
                                  "the credential is locked in the keyring",
                                  "unlock the keyring and retry");
        }
        return makeErrorValue(ErrorCode::NotFound, "no stored secret for this reference",
                              secretReference(service, account).toStdString());
    }

    QDBusInterface item(QString::fromLatin1(kService), unlocked.constFirst().path(),
                        QString::fromLatin1(kItemInterface), QDBusConnection::sessionBus());
    item.setTimeout(kTimeoutMs);
    QDBusMessage secretReply =
        item.call(QStringLiteral("GetSecret"), QVariant::fromValue(QDBusObjectPath(sessionPath_)));
    if (secretReply.type() != QDBusMessage::ReplyMessage || secretReply.arguments().isEmpty()) {
        return makeErrorValue(ErrorCode::PermissionDenied, "the credential store refused the lookup",
                              secretReply.errorMessage().toStdString());
    }

    const QDBusArgument secretArgument = qvariant_cast<QDBusArgument>(secretReply.arguments().at(0));
    return unpackSecret(secretArgument);
}

Status SecretServiceStore::remove(const QString& service, const QString& account) {
    if (!available_) {
        return makeError(ErrorCode::Unsupported, "no Secret Service session", reason_.toStdString());
    }

    QDBusInterface iface(QString::fromLatin1(kService), QString::fromLatin1(kPath),
                         QString::fromLatin1(kServiceInterface), QDBusConnection::sessionBus());
    iface.setTimeout(kTimeoutMs);

    QDBusMessage reply = searchItems(iface, attributesFor(service, account));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return makeError(ErrorCode::DatabaseError, "the credential store search failed",
                         reply.errorMessage().toStdString());
    }

    QList<QDBusObjectPath> items =
        qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().at(0));
    if (items.isEmpty()) {
        return Status();
    }

    QDBusInterface item(QString::fromLatin1(kService), items.constFirst().path(),
                        QString::fromLatin1(kItemInterface), QDBusConnection::sessionBus());
    item.setTimeout(kTimeoutMs);
    QDBusMessage deleteReply = item.call(QStringLiteral("Delete"));
    if (deleteReply.type() != QDBusMessage::ReplyMessage) {
        return makeError(ErrorCode::PermissionDenied, "the credential store refused the deletion",
                         deleteReply.errorMessage().toStdString());
    }
    return Status();
}

} // namespace nesva::security

#endif // Q_OS_LINUX
