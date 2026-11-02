// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/core/redactor.hpp"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>

#include <algorithm>
#include <utility>

namespace nesva {
namespace {

const QStringList& sensitiveKeyFragments() {
    static const QStringList fragments = {
        QStringLiteral("password"),      QStringLiteral("passwd"),
        QStringLiteral("passphrase"),    QStringLiteral("secret"),
        QStringLiteral("token"),         QStringLiteral("credential"),
        QStringLiteral("apikey"),        QStringLiteral("accesskey"),
        QStringLiteral("privatekey"),    QStringLiteral("authorization"),
        QStringLiteral("cookie"),        QStringLiteral("session"),
        QStringLiteral("signature"),     QStringLiteral("clientsecret"),
    };
    return fragments;
}

QString normaliseKey(QStringView key) {
    QString lowered = key.toString().toLower();
    lowered.remove(QLatin1Char('-'));
    lowered.remove(QLatin1Char('_'));
    lowered.remove(QLatin1Char(' '));
    return lowered;
}

} // namespace

bool isSensitiveKeyName(QStringView key) {
    const QString normalised = normaliseKey(key);
    if (normalised.isEmpty()) {
        return false;
    }
    return std::any_of(sensitiveKeyFragments().cbegin(), sensitiveKeyFragments().cend(),
                       [&normalised](const QString& fragment) {
                           return normalised.contains(normaliseKey(QStringView(fragment)));
                       });
}

const QString& Redactor::mask() {
    static const QString value = QStringLiteral("[REDACTED]");
    return value;
}

Redactor::Redactor() {
    const auto options = QRegularExpression::CaseInsensitiveOption;

    // Specific token shapes are matched before the generic assignment pattern so
    // that e.g. "Authorization: Bearer <jwt>" is masked as a whole instead of
    // leaving the credential behind.

    // HTTP authorization header values: keep the scheme, mask the credential.
    patterns_.emplace_back(
        QString::fromUtf8(R"((?i)\b(?:bearer|basic)\s+([A-Za-z0-9\-._~+/]{8,}=*))"), options);

    // PEM private key blocks (no capture groups: the whole block is masked).
    patterns_.emplace_back(QString::fromUtf8(
        R"(-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----)"));

    // AWS access key identifiers.
    patterns_.emplace_back(QString::fromUtf8(R"(\b(?:AKIA|ASIA)[0-9A-Z]{16}\b)"));

    // GitHub personal access tokens and similar prefixed tokens.
    patterns_.emplace_back(QString::fromUtf8(R"(\bgh[pousr]_[A-Za-z0-9]{20,}\b)"));

    // JWT-shaped values (OAuth access tokens, MSAL id tokens).
    patterns_.emplace_back(
        QString::fromUtf8(R"(\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,})"));

    // Generic "password=hunter2", "token: abc" style assignments. The keyword is
    // matched non-capturing; capture group 1 is the value that gets masked, so
    // the surrounding context stays readable in logs.
    patterns_.emplace_back(
        QString::fromUtf8(
            R"re((?i)\b(?:password|passwd|passphrase|secret|token|api[_-]?key|access[_-]?key|)re") +
            QString::fromUtf8(
                R"re(refresh[_-]?token|client[_-]?secret|authorization|session[_-]?key|cookie)\b)re") +
            QString::fromUtf8(R"re(\s*[:=]\s*("[^"]*"|'[^']*'|[^\s,;"'}]+))re"),
        options);
}

void Redactor::addSecret(const QString& secret) {
    if (secret.size() < kMinimumSecretLength) {
        return;
    }
    if (secrets_.contains(secret)) {
        return;
    }
    secrets_.append(secret);
    // Longest first so that overlapping secrets are fully masked.
    std::sort(secrets_.begin(), secrets_.end(),
              [](const QString& lhs, const QString& rhs) { return lhs.size() > rhs.size(); });
}

void Redactor::clearSecrets() {
    secrets_.clear();
}

QString Redactor::redact(const QString& text) const {
    if (text.isEmpty()) {
        return text;
    }

    QString result = text;
    for (const QString& secret : secrets_) {
        if (result.contains(secret)) {
            result.replace(secret, mask());
        }
    }

    for (const QRegularExpression& pattern : patterns_) {
        QString output;
        qsizetype cursor = 0;
        QRegularExpressionMatchIterator it = pattern.globalMatch(result);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            output += result.mid(cursor, match.capturedStart() - cursor);
            const QString captured = match.lastCapturedIndex() >= 1 ? match.captured(1) : QString();
            if (!captured.isEmpty()) {
                QString masked = match.captured(0);
                masked.replace(captured, mask());
                output += masked;
            } else {
                output += mask();
            }
            cursor = match.capturedEnd();
        }
        output += result.mid(cursor);
        result = output;
    }

    return result;
}

QJsonValue Redactor::redactValue(const Redactor& redactor, const QString& key,
                                 const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        QJsonObject redacted;
        for (auto it = object.begin(); it != object.end(); ++it) {
            redacted.insert(it.key(), redactValue(redactor, it.key(), it.value()));
        }
        return redacted;
    }
    case QJsonValue::Array: {
        QJsonArray array;
        const QJsonArray source = value.toArray();
        for (const QJsonValue& element : source) {
            array.append(redactValue(redactor, key, element));
        }
        return array;
    }
    case QJsonValue::String:
        if (isSensitiveKeyName(QStringView(key))) {
            return mask();
        }
        return redactor.redact(value.toString());
    default:
        if (isSensitiveKeyName(QStringView(key))) {
            return mask();
        }
        return value;
    }
}

QJsonObject Redactor::redactObject(const QJsonObject& object) const {
    QJsonObject redacted;
    for (auto it = object.begin(); it != object.end(); ++it) {
        redacted.insert(it.key(), redactValue(*this, it.key(), it.value()));
    }
    return redacted;
}

} // namespace nesva
