// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/scan/exclusions.hpp"

#include <QtCore/QRegularExpression>

#include <algorithm>

namespace nesva::scan {
namespace {

QRegularExpression::PatternOptions defaultOptions() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return QRegularExpression::CaseInsensitiveOption;
#else
    return QRegularExpression::NoPatternOption;
#endif
}

/// Escapes a single character for use in a regular expression.
QString escapeLiteral(QChar character) {
    static const QString special = QStringLiteral("\\^$.|?*+()[]{}");
    if (special.contains(character)) {
        return QLatin1Char('\\') + QString(character);
    }
    return QString(character);
}

} // namespace

QRegularExpression ExclusionRules::globToRegularExpression(const QString& glob) {
    QString pattern;
    pattern.reserve(glob.size() * 2);
    pattern += QLatin1Char('^');

    for (qsizetype index = 0; index < glob.size(); ++index) {
        const QChar character = glob.at(index);
        if (character == QLatin1Char('*')) {
            if (index + 1 < glob.size() && glob.at(index + 1) == QLatin1Char('*')) {
                // "**" spans directory separators.
                pattern += QStringLiteral(".*");
                ++index;
                // Collapse "**/" so that "build/**/tmp" also matches "build/tmp".
                if (index + 1 < glob.size() && glob.at(index + 1) == QLatin1Char('/')) {
                    ++index;
                    pattern += QStringLiteral("(?:.*/)?");
                }
            } else {
                pattern += QStringLiteral("[^/]*");
            }
        } else if (character == QLatin1Char('?')) {
            pattern += QStringLiteral("[^/]");
        } else if (character == QLatin1Char('[')) {
            const qsizetype closing = glob.indexOf(QLatin1Char(']'), index + 1);
            if (closing > index + 1) {
                // Character class passes through; a leading '!' becomes '^'.
                QString body = glob.mid(index + 1, closing - index - 1);
                if (body.startsWith(QLatin1Char('!'))) {
                    body.replace(0, 1, QLatin1Char('^'));
                }
                pattern += QLatin1Char('[') + body + QLatin1Char(']');
                index = closing;
            } else {
                pattern += QStringLiteral("\\[");
            }
        } else {
            pattern += escapeLiteral(character);
        }
    }

    pattern += QLatin1Char('$');
    return QRegularExpression(pattern, defaultOptions());
}

Result<ExclusionRules> ExclusionRules::compile(const QVector<ExclusionRule>& rules) {
    ExclusionRules compiled;

    for (const ExclusionRule& rule : rules) {
        const QString trimmed = rule.pattern.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        CompiledRule entry;
        switch (rule.kind) {
        case PatternKind::Glob:
            entry.expression = globToRegularExpression(trimmed);
            entry.matchFileNameOnly = !trimmed.contains(QLatin1Char('/'));
            break;
        case PatternKind::Regex:
            // User regexes are anchored so a rule cannot partially match.
            entry.expression = QRegularExpression(QRegularExpression::anchoredPattern(trimmed),
                                                  defaultOptions());
            entry.matchFileNameOnly = false;
            break;
        case PatternKind::Literal:
            entry.expression = QRegularExpression(
                QRegularExpression::anchoredPattern(QRegularExpression::escape(trimmed)),
                defaultOptions());
            entry.matchFileNameOnly = !trimmed.contains(QLatin1Char('/'));
            break;
        }

        if (!entry.expression.isValid()) {
            return makeErrorValue(ErrorCode::ConfigurationError,
                                  "invalid exclusion pattern: " + trimmed.toStdString(),
                                  entry.expression.errorString().toStdString());
        }
        compiled.compiled_.append(entry);
    }

    return compiled;
}

bool ExclusionRules::matches(const QString& relativePath, const QString& fileName) const {
    if (compiled_.isEmpty()) {
        return false;
    }

    for (const CompiledRule& rule : compiled_) {
        const QString& subject = rule.matchFileNameOnly ? fileName : relativePath;
        if (rule.expression.match(subject).hasMatch()) {
            return true;
        }
    }
    return false;
}

} // namespace nesva::scan
