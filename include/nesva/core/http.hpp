// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QUrl>

#include <functional>
#include <memory>

#include "nesva/core/result.hpp"

namespace nesva::net {

enum class HttpVerb { Get, Post, Put };

struct HttpResponse {
    int statusCode = 0;
    QByteArray body;
    QByteArray contentType;
    QHash<QString, QString> headers; ///< lower-case header names
    QString error;                   ///< transport error (no status), empty otherwise

    bool isSuccess() const { return statusCode >= 200 && statusCode < 300; }
    bool isRateLimited() const { return statusCode == 429; }
    bool isAuthFailure() const { return statusCode == 401; }
};

/// Synchronous HTTP over HTTPS, with a bounded timeout.
///
/// Injected into the Graph client so responses can be scripted in tests without
/// a network or a tenant. All calls are blocking; callers run them off the GUI
/// thread.
class HttpTransport {
public:
    virtual ~HttpTransport() = default;

    virtual HttpResponse send(HttpVerb verb, const QUrl& url, const QHash<QString, QString>& headers,
                              const QByteArray& body, int timeoutMs) = 0;
};

/// QNetworkAccessManager-backed implementation (Qt Network, TLS verified).
class QNetworkHttpTransport final : public HttpTransport {
public:
    HttpResponse send(HttpVerb verb, const QUrl& url, const QHash<QString, QString>& headers,
                      const QByteArray& body, int timeoutMs) override;
};

} // namespace nesva::net
