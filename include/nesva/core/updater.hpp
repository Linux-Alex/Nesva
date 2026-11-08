// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVector>

#include <functional>

#include "nesva/core/http.hpp"
#include "nesva/core/result.hpp"

namespace nesva::update {

/// Where releases are published. Overridable so tests never touch the network.
inline constexpr const char* kDefaultRepository = "Linux-Alex/Nesva";

/// One downloadable file of a release.
struct ReleaseAsset {
    QString name;
    QUrl url;
    qint64 sizeBytes = 0;
};

/// A published release, as far as updating needs to know about it.
struct ReleaseInfo {
    QString tag;        ///< "v0.6.6"
    QString version;    ///< "0.6.6"
    QUrl notesUrl;
    QDateTime publishedUtc;
    bool prerelease = false;
    QVector<ReleaseAsset> assets;

    const ReleaseAsset* asset(const QString& name) const;
};

/// A dotted version, compared component by component.
class Version {
public:
    static Version parse(const QString& text);

    QString toString() const;
    bool isValid() const noexcept { return valid_; }
    /// Negative when this is older than \p other, 0 when equal.
    int compare(const Version& other) const;

private:
    int major_ = 0;
    int minor_ = 0;
    int patch_ = 0;
    bool valid_ = false;
};

/// The file this platform installs when updating.
QString installerAssetName(const Version& version);
/// The checksum list published next to it.
QString checksumAssetName();

struct UpdateCheck {
    ReleaseInfo release;
    QString currentVersion;
    bool newer = false;
};

/// Reads the latest published release and compares it with \p currentVersion.
///
/// Pre-releases and tags that are not a plain version are ignored: an update
/// that installs itself unattended must never pick up a candidate build.
Result<UpdateCheck> checkForUpdate(net::HttpTransport& transport, const QString& currentVersion,
                                   const QString& repository = QString::fromLatin1(kDefaultRepository));

/// Downloads the installer for \p check.release into \p directory and verifies
/// its SHA-256 against the release checksum list.
///
/// Nothing is executed here, and a file that fails verification is deleted: the
/// caller either gets a verified path or an error.
Result<QString> downloadInstaller(net::HttpTransport& transport, const ReleaseInfo& release,
                                  const QString& directory,
                                  const std::function<void(const QString&)>& progress = {});

/// Runs a verified installer without a user interface.
///
/// Windows: the per-user NSIS installer with /S, which needs no elevation because
/// Nesva installs into %LOCALAPPDATA%. Linux: a .deb goes through pkexec so the
/// elevation prompt is visible, a tarball is unpacked into the directory of the
/// running executable. The caller is responsible for stopping the agent and
/// quitting before this runs: the executables are in use until then.
Status runInstaller(const QString& installerPath, const QString& installPrefix = {});

/// Next occurrence of "Sunday 18:00" in \p timeZone, strictly after \p now.
///
/// Computed in local wall-clock terms so the schedule survives a DST change
/// instead of drifting by an hour twice a year.
QDateTime nextWeeklyUpdate(const QDateTime& now,
                           const QByteArray& timeZone = QByteArrayLiteral("Europe/Ljubljana"));

/// Hour of the weekly update, local to \p timeZone.
inline constexpr int kWeeklyUpdateHour = 18;

} // namespace nesva::update
