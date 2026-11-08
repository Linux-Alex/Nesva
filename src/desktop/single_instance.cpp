// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "single_instance.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include "nesva/core/paths.hpp"

namespace nesva::ui {

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}

SingleInstance::~SingleInstance() {
    if (!ownsServer_ || server_ == nullptr) {
        return;
    }
    server_->close();
    QLocalServer::removeServer(socketName());
}

QString SingleInstance::socketName() const {
    // Shared with the CLI, which asks the window to quit before an update replaces
    // the executables.
    return nesva::Paths::desktopSocketName();
}

bool SingleInstance::notifyExistingInstance() {
    QLocalSocket socket;
    socket.connectToServer(socketName());
    if (!socket.waitForConnected(1000)) {
        return false;
    }
    socket.write("show\n");
    socket.flush();
    socket.waitForBytesWritten(1000);
    // A socket wait only pumps this socket's notifiers. When the listener lives
    // in the same thread (the unit test), give its queued newConnection a turn
    // so it can read the request and answer before this process exits.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    // Keep the connection open until the running instance acknowledges, so a
    // process that exits immediately cannot drop the request in the pipe.
    if (socket.waitForReadyRead(1000)) {
        socket.readAll();
    }
    socket.disconnectFromServer();
    return true;
}

bool SingleInstance::listen() {
    if (server_ != nullptr && server_->isListening()) {
        return true;
    }
    error_.clear();
    // A previous failed attempt leaves a non-listening server behind; replace
    // it instead of reporting success.
    delete server_;
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(socketName())) {
        // A previous process may have left a socket file behind (Unix). A
        // second attempt also covers losing a race with a simultaneous start:
        // the caller re-checks notifyExistingInstance() afterwards.
        QLocalServer::removeServer(socketName());
        if (!server_->listen(socketName())) {
            error_ = server_->errorString();
            return false;
        }
    }
    ownsServer_ = true;

    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            auto readRequest = [this, socket] {
                while (socket->canReadLine()) {
                    const QByteArray request = socket->readLine().trimmed();
                    if (request == QByteArrayLiteral("show")) {
                        emit showRequested();
                        socket->write("ok\n");
                        socket->flush();
                    } else if (request == QByteArrayLiteral("quit")) {
                        // Sent by "nesva-cli update --install": the installers
                        // replace the running executables and cannot do that while
                        // this process holds them open.
                        emit quitRequested();
                        socket->write("ok\n");
                        socket->flush();
                    }
                }
            };
            connect(socket, &QLocalSocket::readyRead, this, readRequest);
            // The request may already be buffered when the connection is
            // accepted; readyRead is only emitted for new data.
            readRequest();
        }
    });
    return true;
}

} // namespace nesva::ui
