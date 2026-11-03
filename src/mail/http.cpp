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

    QByteArray payload = body;

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

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&loop, reply] {
        if (reply != nullptr && reply->isRunning()) {
            reply->abort();
        }
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs > 0 ? timeoutMs : 30000);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        // An aborted request is a timeout, not an HTTP status.
        response.error = reply->errorString();
        response.statusCode = 0;
    } else {
        response.statusCode =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = reply->readAll();
        response.contentType = reply->rawHeader("Content-Type");
        for (const QByteArray& name : reply->rawHeaderList()) {
            response.headers.insert(QString::fromUtf8(name).toLower(),
                                    QString::fromUtf8(reply->rawHeader(name)));
        }
    }
    reply->deleteLater();
    return response;
}

} // namespace nesva::net
