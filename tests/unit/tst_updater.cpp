// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimeZone>

#include "nesva/core/hashing.hpp"
#include "nesva/core/http.hpp"
#include "nesva/core/updater.hpp"

using namespace nesva;

namespace {

/// Scripted release service: answers the API call, the checksum list and the
/// installer download, so the whole update path runs without a network.
class ScriptedTransport final : public net::HttpTransport {
public:
    QString tag = QStringLiteral("v9.9.9");
    bool prerelease = false;
    QByteArray installerBody = QByteArrayLiteral("installer-bytes");
    QByteArray checksumBody; ///< empty means "derive the correct one"
    int installerStatus = 200;
    QVector<QString> requested;

    net::HttpResponse send(net::HttpVerb verb, const QUrl& url,
                           const QHash<QString, QString>& headers, const QByteArray& body,
                           int timeoutMs) override {
        Q_UNUSED(verb);
        Q_UNUSED(headers);
        Q_UNUSED(body);
        Q_UNUSED(timeoutMs);
        requested.append(url.toString());
        net::HttpResponse response;
        const QString target = url.toString();
        if (target.contains(QStringLiteral("api.github.com"))) {
            response.statusCode = 200;
            QJsonObject release;
            release.insert(QStringLiteral("tag_name"), tag);
            release.insert(QStringLiteral("html_url"),
                           QStringLiteral("https://example.invalid/releases"));
            release.insert(QStringLiteral("prerelease"), prerelease);
            release.insert(QStringLiteral("published_at"),
                           QStringLiteral("2026-11-01T10:00:00Z"));
            QJsonArray assets;
            for (const QString& name :
                 {installerName(), update::checksumAssetName()}) {
                QJsonObject asset;
                asset.insert(QStringLiteral("name"), name);
                asset.insert(QStringLiteral("browser_download_url"),
                             QStringLiteral("https://example.invalid/download/%1").arg(name));
                asset.insert(QStringLiteral("size"), 15);
                assets.append(asset);
            }
            release.insert(QStringLiteral("assets"), assets);
            response.body = QJsonDocument(release).toJson(QJsonDocument::Compact);
            return response;
        }
        if (target.endsWith(update::checksumAssetName())) {
            response.statusCode = 200;
            response.body = checksumBody.isEmpty()
                                  ? QStringLiteral("%1  %2")
                                        .arg(sha256OfBytes(installerBody), installerName())
                                        .toUtf8()
                                  : checksumBody;
            return response;
        }
        response.statusCode = installerStatus;
        if (installerStatus == 200) {
            response.body = installerBody;
        }
        return response;
    }

    static QString installerName() {
        return update::installerAssetName(update::Version::parse(QStringLiteral("9.9.9")));
    }
};

} // namespace

class UpdaterTest : public QObject {
    Q_OBJECT

private slots:
    void parsesAndComparesVersions();
    void detectsANewerRelease();
    void ignoresPreReleasesAndCandidateTags();
    void downloadsAndVerifiesTheInstaller();
    void refusesAnInstallerWithTheWrongChecksum();
    void reportsAnUnreachableService();
    void schedulesTheWeeklyUpdateForSundayEvening();
    void weeklyScheduleSurvivesDaylightSavingTime();
};

void UpdaterTest::parsesAndComparesVersions() {
    QVERIFY(!update::Version::parse(QStringLiteral("v0.6.6-candidate")).isValid());
    QVERIFY(!update::Version::parse(QStringLiteral("main")).isValid());
    const update::Version older = update::Version::parse(QStringLiteral("0.6.5"));
    const update::Version newer = update::Version::parse(QStringLiteral("v0.6.10"));
    QVERIFY(older.isValid());
    QVERIFY(newer.isValid());
    QCOMPARE(older.compare(newer), -1);
    QCOMPARE(newer.compare(older), 1);
    QCOMPARE(older.compare(update::Version::parse(QStringLiteral("0.6.5"))), 0);
    // Patch 10 is newer than patch 9: the comparison is numeric, not lexical.
    QVERIFY(newer.compare(update::Version::parse(QStringLiteral("0.6.9"))) > 0);
}

void UpdaterTest::detectsANewerRelease() {
    ScriptedTransport transport;
    const Result<update::UpdateCheck> check =
        update::checkForUpdate(transport, QStringLiteral("0.6.5"));
    QVERIFY2(check.hasValue(), check.hasValue() ? "" : check.error().toDisplayString().c_str());
    QCOMPARE(check.value().release.version, QStringLiteral("9.9.9"));
    QVERIFY(check.value().newer);
    QVERIFY(transport.requested.constFirst().contains(QStringLiteral("releases/latest")));

    // The same version is not an update.
    ScriptedTransport same;
    const Result<update::UpdateCheck> current =
        update::checkForUpdate(same, QStringLiteral("9.9.9"));
    QVERIFY(current.hasValue());
    QVERIFY(!current.value().newer);
}

void UpdaterTest::ignoresPreReleasesAndCandidateTags() {
    ScriptedTransport flagged;
    flagged.prerelease = true;
    const Result<update::UpdateCheck> pre =
        update::checkForUpdate(flagged, QStringLiteral("0.6.5"));
    QVERIFY(pre.hasValue());
    QVERIFY(!pre.value().newer);

    ScriptedTransport candidate;
    candidate.tag = QStringLiteral("v9.9.9-candidate");
    const Result<update::UpdateCheck> can =
        update::checkForUpdate(candidate, QStringLiteral("0.6.5"));
    QVERIFY(can.hasValue());
    QVERIFY(!can.value().newer);
}

void UpdaterTest::downloadsAndVerifiesTheInstaller() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    ScriptedTransport transport;
    const Result<update::UpdateCheck> check =
        update::checkForUpdate(transport, QStringLiteral("0.6.5"));
    QVERIFY(check.hasValue());

    QStringList progress;
    const Result<QString> path = update::downloadInstaller(
        transport, check.value().release, directory.path(),
        [&progress](const QString& line) { progress.append(line); });
    QVERIFY2(path.hasValue(), path.hasValue() ? "" : path.error().toDisplayString().c_str());
    QVERIFY(QFileInfo::exists(path.value()));
    QVERIFY(path.value().endsWith(ScriptedTransport::installerName()));
    QVERIFY(!progress.isEmpty());
    QFile file(path.value());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), transport.installerBody);
}

void UpdaterTest::refusesAnInstallerWithTheWrongChecksum() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    ScriptedTransport transport;
    transport.checksumBody =
        QStringLiteral("0000000000000000000000000000000000000000000000000000000000000000  %1")
            .arg(ScriptedTransport::installerName())
            .toUtf8();
    const Result<update::UpdateCheck> check =
        update::checkForUpdate(transport, QStringLiteral("0.6.5"));
    QVERIFY(check.hasValue());

    const Result<QString> path =
        update::downloadInstaller(transport, check.value().release, directory.path());
    QVERIFY(!path.hasValue());
    QCOMPARE(static_cast<int>(path.error().code()),
             static_cast<int>(ErrorCode::IntegrityError));
    QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
}

void UpdaterTest::reportsAnUnreachableService() {
    class OfflineTransport final : public net::HttpTransport {
    public:
        net::HttpResponse send(net::HttpVerb, const QUrl&, const QHash<QString, QString>&,
                               const QByteArray&, int) override {
            net::HttpResponse response;
            response.statusCode = 0;
            response.error = QStringLiteral("network is unreachable");
            return response;
        }
    } offline;

    const Result<update::UpdateCheck> check =
        update::checkForUpdate(offline, QStringLiteral("0.6.5"));
    QVERIFY(!check.hasValue());
}

void UpdaterTest::schedulesTheWeeklyUpdateForSundayEvening() {
    const QTimeZone ljubljana("Europe/Ljubljana");
    QVERIFY(ljubljana.isValid());

    // Sunday 17:00 local: tonight at 18:00.
    const QDateTime sundayAfternoon(QDate(2026, 11, 8), QTime(17, 0), ljubljana);
    const QDateTime due = update::nextWeeklyUpdate(sundayAfternoon.toUTC());
    QCOMPARE(due.toTimeZone(ljubljana), QDateTime(QDate(2026, 11, 8), QTime(18, 0), ljubljana));

    // Sunday 19:00 local: the next Sunday, not two hours ago.
    const QDateTime sundayEvening(QDate(2026, 11, 8), QTime(19, 0), ljubljana);
    const QDateTime next = update::nextWeeklyUpdate(sundayEvening.toUTC());
    QCOMPARE(next.toTimeZone(ljubljana), QDateTime(QDate(2026, 11, 15), QTime(18, 0), ljubljana));

    // A Monday morning schedules the coming Sunday.
    const QDateTime monday(QDate(2026, 11, 9), QTime(9, 0), ljubljana);
    const QDateTime fromMonday = update::nextWeeklyUpdate(monday.toUTC());
    QCOMPARE(fromMonday.toTimeZone(ljubljana),
             QDateTime(QDate(2026, 11, 15), QTime(18, 0), ljubljana));
}

void UpdaterTest::weeklyScheduleSurvivesDaylightSavingTime() {
    const QTimeZone ljubljana("Europe/Ljubljana");
    QVERIFY(ljubljana.isValid());

    // 2026-10-25 is the end of summer time: the schedule is defined on the wall
    // clock, so the UTC instant shifts by an hour but 18:00 local stays 18:00.
    const QDateTime beforeChange(QDate(2026, 10, 24), QTime(12, 0), ljubljana);
    const QDateTime dueBefore = update::nextWeeklyUpdate(beforeChange.toUTC());
    QCOMPARE(dueBefore.toTimeZone(ljubljana).time(), QTime(18, 0));

    const QDateTime afterChange(QDate(2026, 10, 26), QTime(12, 0), ljubljana);
    const QDateTime dueAfter = update::nextWeeklyUpdate(afterChange.toUTC());
    QCOMPARE(dueAfter.toTimeZone(ljubljana).time(), QTime(18, 0));
    QCOMPARE(dueAfter.toTimeZone(ljubljana).date().dayOfWeek(), 7);
}

QTEST_GUILESS_MAIN(UpdaterTest)
#include "tst_updater.moc"
