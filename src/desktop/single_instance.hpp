// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

class QLocalServer;

namespace nesva::ui {

/// One desktop process per data directory.
///
/// The first instance listens on a per-user local socket named from the data
/// root. A later instance asks the running window to come forward and then
/// exits, so opening Nesva again never creates a second window or tray icon.
class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QObject* parent = nullptr);
    ~SingleInstance() override;

    /// Connects to an already running instance and asks it to show its window.
    /// Returns true when the request was delivered.
    bool notifyExistingInstance();

    /// Claims the per-data-directory socket and starts listening. Returns false
    /// when no instance can be reached and the socket cannot be created.
    bool listen();

    QString socketName() const;
    QString errorString() const {
        return error_;
    }

signals:
    /// Emitted by the running instance when another process asks it to show.
    void showRequested();
    /// Emitted when another process asks the window to quit (before an update).
    void quitRequested();

private:
    QLocalServer* server_ = nullptr;
    bool ownsServer_ = false;
    QString error_;
};

} // namespace nesva::ui
