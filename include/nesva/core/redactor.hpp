// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <vector>

namespace nesva {

/// Removes credentials from arbitrary text before it reaches a log sink,
/// a diagnostic export, or a user-visible error message.
///
/// Two mechanisms are combined:
///  * exact registered secret values (tokens, passwords, proxy credentials);
///  * generic key/value and bearer-token patterns that catch secrets Nesva
///    never saw directly (for example tokens echoed by Rclone or Restic).
///
/// Redaction is intentionally aggressive: a false positive hides a harmless
/// string, while a false negative leaks a credential.
class Redactor {
public:
    static constexpr int kMinimumSecretLength = 4;

    Redactor();

    /// Registers a literal secret value. Values shorter than
    /// kMinimumSecretLength are ignored because replacing them would mangle
    /// ordinary log text.
    void addSecret(const QString& secret);
    void clearSecrets();
    int secretCount() const noexcept { return static_cast<int>(secrets_.size()); }

    QString redact(const QString& text) const;
    QJsonObject redactObject(const QJsonObject& object) const;

    /// Value substituted for anything classified as sensitive.
    static const QString& mask();

private:
    static QJsonValue redactValue(const Redactor& redactor, const QString& key, const QJsonValue& value);

    QStringList secrets_;
    std::vector<QRegularExpression> patterns_;
};

/// Returns true when a JSON/field key name conventionally carries a secret.
bool isSensitiveKeyName(QStringView key);

} // namespace nesva
