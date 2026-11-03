// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/mail/http.hpp"

#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <utility>

namespace nesva::net {

HttpResponse QNetworkHttpTransport::send(HttpVerb verb, const QUrl& url,
                                         const QHash<QString, QString>& headers,
                                         const QByteArray& body, int timeoutMs) {
    HttpResponse response;

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }

    const QByteArray payload = body;
    const int effectiveTimeoutMs = timeoutMs > 0 ? timeoutMs : 30000;

    QNetworkReply* reply = nullptr;
    switch (verb) {
    case HttpVerb::Get:
        reply = manager.get(request);
        break;
    case HttpVerb::Post:
        reply = manager.post(request, payload);
        break;
    case HttpVerb::Put:
        reply = manager.put(request, payload);
        break;
    }
    if (reply == nullptr) {
        response.error = QStringLiteral("the request could not be created");
        return response;
    }

    // Connect before the loop runs, then check for a request that already
    // finished: a synchronous failure would otherwise wait for the timeout.
    bool timedOut = false;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        if (reply->isRunning()) {
            reply->abort();
        }
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) {
        timer.start(effectiveTimeoutMs);
        loop.exec();
    }
    timer.stop();

    // Qt reports HTTP error statuses (4xx/5xx) through QNetworkReply::error(),
    // so the reply error alone does not mean the exchange failed at the
    // transport level. The presence of a status code is what distinguishes a
    // server response from a broken connection, and the body must be kept:
    // callers need it to read the error document and to honour Retry-After.
    const QVariant statusAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int statusCode = statusAttribute.isValid() ? statusAttribute.toInt() : 0;

    if (statusCode > 0) {
        response.statusCode = statusCode;
        response.body = reply->readAll();
        response.contentType = reply->rawHeader("Content-Type");
        for (const QByteArray& name : reply->rawHeaderList()) {
            response.headers.insert(QString::fromUtf8(name).toLower(),
                                    QString::fromUtf8(reply->rawHeader(name)));
        }
    } else {
        response.statusCode = 0;
        if (timedOut) {
            response.error =
                QStringLiteral("the request timed out after %1 ms").arg(effectiveTimeoutMs);
        } else {
            response.error = reply->errorString();
            if (response.error.isEmpty()) {
                response.error = QStringLiteral("the request failed without an HTTP response");
            }
        }
    }

    reply->deleteLater();
    return response;
}

} // namespace nesva::net
