// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QVector>

#include "nesva/core/result.hpp"

namespace nesva::scan {

/// How a user-supplied exclusion pattern is interpreted.
enum class PatternKind {
    Glob,    ///< Shell-style wildcard; '/' separates path segments.
    Regex,   ///< Full QRegularExpression, matched against the relative path.
    Literal, ///< Exact path or file name, no wildcards.
};

struct ExclusionRule {
    QString pattern;
    PatternKind kind = PatternKind::Glob;
};

/// Compiled exclusion rules.
///
/// Matching rules, documented because they decide what is *not* backed up:
///  * a pattern without '/' matches the file name only (glob/literal) or is
///    applied to the whole relative path (regex);
///  * a pattern containing '/' is matched against the full relative path,
///    which always uses '/' separators regardless of platform;
///  * matching is case-sensitive on Linux and case-insensitive on Windows and
///    macOS, mirroring the underlying filesystem.
class ExclusionRules {
public:
    ExclusionRules() = default;

    /// Compiles the rules. Returns ErrorCode::ConfigurationError when a regex
    /// pattern is invalid, so a broken rule can never silently stop excluding.
    static Result<ExclusionRules> compile(const QVector<ExclusionRule>& rules);

    bool isEmpty() const noexcept { return compiled_.isEmpty(); }
    int ruleCount() const noexcept { return static_cast<int>(compiled_.size()); }

    /// \param relativePath path relative to the source root, '/' separated.
    /// \param fileName basename of the entry.
    bool matches(const QString& relativePath, const QString& fileName) const;

    /// Converts a glob to an anchored regular expression. Exposed for tests and
    /// for the dry-run preview that shows the effective pattern.
    static QRegularExpression globToRegularExpression(const QString& glob);

private:
    struct CompiledRule {
        QRegularExpression expression;
        bool matchFileNameOnly = false;
    };

    QVector<CompiledRule> compiled_;
};

} // namespace nesva::scan
