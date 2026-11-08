// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include "nesva/core/updater.hpp"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSysInfo>
#include <QtCore/QTimeZone>

#include "nesva/core/hashing.hpp"
#include "nesva/core/logging.hpp"

namespace nesva::update {
namespace {

constexpr int kApiTimeoutMs = 20000;
constexpr int kDownloadTimeoutMs = 300000;

QString architectureToken() {
    const QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == QStringLiteral("x86_64")) {
        return QStringLiteral("x86_64");
    }
    return arch;
}

/// "v0.6.6" or "0.6.6" -> "0.6.6"; anything else (a -candidate suffix, a branch
/// name) yields an empty string, which the caller treats as "not a release".
QString releasedVersion(const QString& tag) {
    static const QRegularExpression pattern(QStringLiteral("^v?(\\d+\\.\\d+\\.\\d+)$"));
    const QRegularExpressionMatch match = pattern.match(tag.trimmed());
    return match.hasMatch() ? match.captured(1) : QString();
}

Result<QByteArray> fetchBytes(net::HttpTransport& transport, const QUrl& url,
                              const QHash<QString, QString>& headers, int timeoutMs) {
    const net::HttpResponse response = transport.send(net::HttpVerb::Get, url, headers, {}, timeoutMs);
    if (response.statusCode == 0) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              QStringLiteral("cannot reach %1").arg(url.host()).toStdString(),
                              response.error.toStdString());
    }
    if (!response.isSuccess()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              QStringLiteral("%1 answered %2").arg(url.host()).arg(response.statusCode)
                                  .toStdString(),
                              QString::fromUtf8(response.body.left(200)).toStdString());
    }
    return response.body;
}

} // namespace

const ReleaseAsset* ReleaseInfo::asset(const QString& name) const {
    for (const ReleaseAsset& candidate : assets) {
        if (candidate.name == name) {
            return &candidate;
        }
    }
    return nullptr;
}

Version Version::parse(const QString& text) {
    Version version;
    // An optional leading "v" and nothing after the patch number, so "v0.6.10"
    // parses and a tag like "v0.6.6-candidate" does not.
    static const QRegularExpression pattern(QStringLiteral("^v?(\\d+)\\.(\\d+)\\.(\\d+)$"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch()) {
        return version;
    }
    version.major_ = match.captured(1).toInt();
    version.minor_ = match.captured(2).toInt();
    version.patch_ = match.captured(3).toInt();
    version.valid_ = true;
    return version;
}

QString Version::toString() const {
    return QStringLiteral("%1.%2.%3").arg(major_).arg(minor_).arg(patch_);
}

int Version::compare(const Version& other) const {
    if (major_ != other.major_) {
        return major_ < other.major_ ? -1 : 1;
    }
    if (minor_ != other.minor_) {
        return minor_ < other.minor_ ? -1 : 1;
    }
    if (patch_ != other.patch_) {
        return patch_ < other.patch_ ? -1 : 1;
    }
    return 0;
}

QString installerAssetName(const Version& version) {
#if defined(Q_OS_WIN)
    return QStringLiteral("nesva-%1-windows-%2-setup.exe")
        .arg(version.toString(), architectureToken());
#else
    return QStringLiteral("nesva-%1-linux-%2.tar.gz")
        .arg(version.toString(), architectureToken());
#endif
}

QString checksumAssetName() {
#if defined(Q_OS_WIN)
    return QStringLiteral("SHA256SUMS-windows.txt");
#else
    return QStringLiteral("SHA256SUMS-linux.txt");
#endif
}

namespace {

/// Fallback that needs no API quota.
///
/// The GitHub REST API allows 60 anonymous requests per hour *per IP*, which a
/// corporate NAT or a few restarts can exhaust; releases.atom has no such limit.
/// It carries the tag of the newest release, and the assets are published under a
/// name convention this project controls, so the download URLs can be built from
/// the tag without asking the API.
Result<ReleaseInfo> latestFromAtom(net::HttpTransport& transport, const QString& repository) {
    const QUrl url(QStringLiteral("https://github.com/%1/releases.atom").arg(repository));
    const Result<QByteArray> body = fetchBytes(transport, url, {}, kApiTimeoutMs);
    if (!body) {
        return body.error();
    }
    const QString feed = QString::fromUtf8(body.value());
    static const QRegularExpression linkPattern(
        QStringLiteral("releases/tag/(v[0-9][^<\" ]*)"));
    const QRegularExpressionMatch match = linkPattern.match(feed);
    if (!match.hasMatch()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "the release feed lists no release");
    }

    ReleaseInfo release;
    release.tag = match.captured(1);
    release.version = releasedVersion(release.tag);
    release.notesUrl = QUrl(QStringLiteral("https://github.com/%1/releases/tag/%2")
                                .arg(repository, release.tag));
    if (release.version.isEmpty()) {
        return makeErrorValue(ErrorCode::ExternalToolFailed,
                              "the newest release is not a plain version",
                              release.tag.toStdString());
    }

    const Version version = Version::parse(release.version);
    const QString base =
        QStringLiteral("https://github.com/%1/releases/download/%2/").arg(repository, release.tag);
    for (const QString& name : {installerAssetName(version), checksumAssetName()}) {
        release.assets.append(ReleaseAsset{name, QUrl(base + name), 0});
    }
    return release;
}

} // namespace
Result<UpdateCheck> checkForUpdate(net::HttpTransport& transport, const QString& currentVersion,
                                   const QString& repository) {
    const QUrl url(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(repository));
    // The GitHub API serves JSON only, and it is versioned: pin the vendor media
    // type so a future default does not change the payload shape under us.
    const Result<QByteArray> body = fetchBytes(
        transport, url,
        {{QStringLiteral("Accept"), QStringLiteral("application/vnd.github+json")},
         {QStringLiteral("X-GitHub-Api-Version"), QStringLiteral("2022-11-28")}},
        kApiTimeoutMs);
    const QJsonObject object = body ? QJsonDocument::fromJson(body.value()).object() : QJsonObject();
    if (object.isEmpty()) {
        // The API is rate limited per IP, so a corporate NAT (or a few restarts) can
        // exhaust it. Fall back to the Atom feed, which has no such limit.
        const Result<ReleaseInfo> fallback = latestFromAtom(transport, repository);
        if (!fallback) {
            return body ? makeErrorValue(ErrorCode::ExternalToolFailed,
                                         "the release service returned no usable release")
                        : body.error();
        }
        UpdateCheck check;
        check.currentVersion = currentVersion;
        check.release = fallback.value();
        const Version published = Version::parse(check.release.version);
        const Version current = Version::parse(currentVersion);
        check.newer = published.isValid() && current.isValid() && published.compare(current) > 0;
        return check;
    }

    UpdateCheck check;
    check.currentVersion = currentVersion;
    check.release.tag = object.value(QStringLiteral("tag_name")).toString();
    check.release.version = releasedVersion(check.release.tag);
    check.release.notesUrl = QUrl(object.value(QStringLiteral("html_url")).toString());
    check.release.prerelease = object.value(QStringLiteral("prerelease")).toBool(false);
    check.release.publishedUtc =
        QDateTime::fromString(object.value(QStringLiteral("published_at")).toString(), Qt::ISODate);

    const QJsonArray assets = object.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& value : assets) {
        const QJsonObject assetObject = value.toObject();
        ReleaseAsset asset;
        asset.name = assetObject.value(QStringLiteral("name")).toString();
        asset.url = QUrl(assetObject.value(QStringLiteral("browser_download_url")).toString());
        asset.sizeBytes = static_cast<qint64>(assetObject.value(QStringLiteral("size")).toDouble(0));
        check.release.assets.append(asset);
    }

    if (check.release.version.isEmpty() || check.release.prerelease) {
        // A tag like v0.6.6-candidate, or a release flagged pre-release, is never
        // installed automatically.
        check.newer = false;
        return check;
    }

    const Version published = Version::parse(check.release.version);
    const Version current = Version::parse(currentVersion);
    if (!published.isValid() || !current.isValid()) {
        return makeErrorValue(ErrorCode::InvalidArgument, "cannot compare versions",
                              QStringLiteral("published %1, running %2")
                                  .arg(check.release.version, currentVersion)
                                  .toStdString());
    }
    check.newer = published.compare(current) > 0;
    return check;
}

Result<QString> downloadInstaller(net::HttpTransport& transport, const ReleaseInfo& release,
                                  const QString& directory,
                                  const std::function<void(const QString&)>& progress) {
    const auto report = [&progress](const QString& line) {
        if (progress) {
            progress(line);
        }
    };

    const Version version = Version::parse(release.version);
    const QString installerName = installerAssetName(version);
    const ReleaseAsset* installer = release.asset(installerName);
    if (installer == nullptr) {
        return makeErrorValue(ErrorCode::NotFound,
                              QStringLiteral("release %1 has no %2")
                                  .arg(release.tag, installerName)
                                  .toStdString());
    }
    const ReleaseAsset* sums = release.asset(checksumAssetName());
    if (sums == nullptr) {
        return makeErrorValue(ErrorCode::IntegrityError,
                              QStringLiteral("release %1 publishes no %2")
                                  .arg(release.tag, checksumAssetName())
                                  .toStdString());
    }

    if (!QDir().mkpath(directory)) {
        return makeErrorValue(ErrorCode::IoError, "cannot create the download directory",
                              directory.toStdString());
    }

    report(QStringLiteral("Reading the published checksum list"));
    const Result<QByteArray> sumsBody = fetchBytes(transport, sums->url, {}, kApiTimeoutMs);
    if (!sumsBody) {
        return sumsBody.error();
    }
    QString expectedSha256;
    const QStringList lines =
        QString::fromUtf8(sumsBody.value()).split(QStringLiteral("\n"), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QStringList parts =
            line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() >= 2 && parts.at(1).trimmed() == installerName) {
            expectedSha256 = parts.at(0).trimmed().toLower();
            break;
        }
    }
    if (expectedSha256.isEmpty()) {
        return makeErrorValue(ErrorCode::IntegrityError,
                              QStringLiteral("%1 is not listed in %2")
                                  .arg(installerName, checksumAssetName())
                                  .toStdString());
    }

    report(QStringLiteral("Downloading %1").arg(installerName));
    const Result<QByteArray> payload = fetchBytes(transport, installer->url, {}, kDownloadTimeoutMs);
    if (!payload) {
        return payload.error();
    }

    report(QStringLiteral("Verifying its SHA-256 checksum"));
    const QString actual = sha256OfBytes(payload.value()).toLower();
    if (actual.compare(expectedSha256, Qt::CaseInsensitive) != 0) {
        return makeErrorValue(ErrorCode::IntegrityError,
                              "the downloaded update does not match the published checksum",
                              QStringLiteral("%1: expected %2, got %3")
                                  .arg(installerName, expectedSha256, actual)
                                  .toStdString());
    }

    const QString target = QDir(directory).filePath(installerName);
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(payload.value()) != payload.value().size()) {
        return makeErrorValue(ErrorCode::IoError, "cannot write the downloaded update",
                              target.toStdString());
    }
    file.close();

    logInfo("update.download", QStringLiteral("update downloaded and verified"),
            QJsonObject{{QStringLiteral("version"), release.version},
                        {QStringLiteral("file"), target},
                        {QStringLiteral("sha256"), actual}});
    report(QStringLiteral("Verified %1").arg(installerName));
    return target;
}

Status runInstaller(const QString& installerPath, const QString& installPrefix) {
    const QFileInfo info(installerPath);
    if (!info.isFile()) {
        return makeError(ErrorCode::NotFound, "the downloaded update is missing",
                         installerPath.toStdString());
    }

#if defined(Q_OS_WIN)
    // The per-user NSIS installer: /S is unattended and needs no elevation,
    // because everything lands in %LOCALAPPDATA%.
    if (!QProcess::startDetached(installerPath, {QStringLiteral("/S")})) {
        return makeError(ErrorCode::ExternalToolFailed, "cannot start the installer",
                         installerPath.toStdString());
    }
    return Status();
#else
    if (installerPath.endsWith(QStringLiteral(".deb"))) {
        // Replacing a system-wide install is the package manager job, which needs
        // root: pkexec is the visible, per-command elevation prompt.
        const int exitCode = QProcess::execute(
            QStringLiteral("pkexec"),
            {QStringLiteral("apt-get"), QStringLiteral("install"), QStringLiteral("-y"),
             installerPath});
        if (exitCode != 0) {
            return makeError(ErrorCode::PermissionDenied,
                             "the package manager did not install the update",
                             QStringLiteral("exit code %1").arg(exitCode).toStdString());
        }
        return Status();
    }

    // Portable tarball: unpack over this installation.
    QString prefix = installPrefix;
    if (prefix.isEmpty()) {
        // <prefix>/lib/nesva/bin -> <prefix>/lib/nesva -> <prefix>
        const QFileInfo bindir(QCoreApplication::applicationDirPath());
        prefix = QFileInfo(bindir.absolutePath()).absolutePath();
    }
    const int exitCode = QProcess::execute(
        QStringLiteral("tar"),
        {QStringLiteral("-xzf"), installerPath, QStringLiteral("-C"), prefix,
         QStringLiteral("--strip-components=1")});
    if (exitCode != 0) {
        return makeError(ErrorCode::IoError, "cannot unpack the update",
                         QStringLiteral("tar exited with %1").arg(exitCode).toStdString());
    }
    return Status();
#endif
}

QDateTime nextWeeklyUpdate(const QDateTime& now, const QByteArray& timeZoneId) {
    const QTimeZone zone(timeZoneId);
    const QTimeZone effective = zone.isValid() ? zone : QTimeZone::systemTimeZone();
    const QDateTime local = now.toTimeZone(effective);

    const QDate date = local.date();
    // QDate::dayOfWeek() is 1 (Monday) .. 7 (Sunday).
    const int daysUntilSunday = (7 - date.dayOfWeek()) % 7;
    QDateTime candidate(date.addDays(daysUntilSunday), QTime(kWeeklyUpdateHour, 0), effective);
    if (candidate <= now) {
        candidate = candidate.addDays(7);
    }
    return candidate;
}

} // namespace nesva::update
