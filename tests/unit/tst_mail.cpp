// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 The Nesva Authors

#include <QtTest/QtTest>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>

#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <functional>
#include <memory>

#include "nesva/core/cancellation.hpp"
#include "nesva/db/database.hpp"
#include "nesva/mail/exporter.hpp"
#include "nesva/mail/graph.hpp"
#include "nesva/mail/http.hpp"
#include "nesva/mail/mail_store.hpp"
#include "nesva/mail/oauth.hpp"
#include "support/test_fixtures.hpp"

using namespace nesva;

namespace {

/// Scripted HTTP layer: answers the token endpoint and the Graph endpoints the
/// exporter uses, so the whole sync path is exercised without a tenant.
class FakeTransport final : public net::HttpTransport {
public:
    QVector<QString> requestedUrls;
    QHash<QString, QByteArray> responses; ///< url substring -> body
    QString deltaBody =
        R"({"value":[
            {"id":"m1","internetMessageId":"<one@example.com>","subject":"Re: [TEST] invoice a/b:c*d?",
             "from":{"emailAddress":{"name":"Alice","address":"alice@example.com"}},
             "receivedDateTime":"2026-03-10T08:00:00Z","lastModifiedDateTime":"2026-03-10T08:00:01Z",
             "parentFolderId":"inbox","hasAttachments":false,"size":120},
            {"id":"m2","internetMessageId":"<two@example.com>","subject":"Second message",
             "from":{"emailAddress":{"address":"bob@example.com"}},
             "receivedDateTime":"2026-03-11T09:30:00Z","lastModifiedDateTime":"2026-03-11T09:30:01Z",
             "parentFolderId":"inbox","hasAttachments":true,"size":240}
        ],
        "@odata.deltaLink":"https://graph.microsoft.com/v1.0/me/mailFolders/inbox/messages/delta?$deltatoken=abc"})";

    std::function<QByteArray(const QString&)> mimeProvider;
    int statusOverride = 200;

    net::HttpResponse send(net::HttpVerb verb, const QUrl& url,
                           const QHash<QString, QString>& headers,
                           const QByteArray& body, int timeoutMs) override {
        Q_UNUSED(verb);
        Q_UNUSED(headers);
        Q_UNUSED(body);
        Q_UNUSED(timeoutMs);
        const QString target = url.toString();
        requestedUrls.append(target);

        net::HttpResponse response;
        if (target.contains(QStringLiteral("login.microsoftonline.com"))) {
            response.statusCode = 200;
            response.body =
                R"({"access_token":"fake-access","refresh_token":"fake-refresh","expires_in":3600,"token_type":"Bearer"})";
            return response;
        }
        if (target.contains(QStringLiteral("/$value"))) {
            response.statusCode = 200;
            response.body = mimeProvider ? mimeProvider(target) : QByteArray("From: a@b\r\n\r\nbody");
            return response;
        }
        if (target.contains(QStringLiteral("/delta"))) {
            response.statusCode = statusOverride;
            response.body = deltaBody.toUtf8();
            return response;
        }
        response.statusCode = 200;
        response.body = "{}";
        return response;
    }
};

} // namespace

/// Minimal HTTP server that answers every request with a canned response, so
/// the real QNetworkHttpTransport can be exercised without a network or tenant.
class CannedHttpServer {
public:
    explicit CannedHttpServer(QByteArray response) : response_(std::move(response)) {}

    bool listen() {
        if (!server_.listen(QHostAddress::LocalHost, 0)) {
            return false;
        }
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    socket->readAll();
                    socket->write(response_);
                    socket->flush();
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket,
                                 &QObject::deleteLater);
            }
        });
        return true;
    }

    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/probe").arg(server_.serverPort()));
    }

private:
    QTcpServer server_;
    QByteArray response_;
};

class MailTest : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void pkceMatchesRfc7636Vector();
    void base64UrlHasNoPaddingOrUnsafeCharacters();
    void authorizeUrlCarriesPkceAndScope();
    void tokenResponseParsing();
    void rejectsErrorResponses();
    void sanitisesFileNames();
    void buildsDatedEmlPaths();
    void exportsMessagesAndRecordsDelta();
    void secondRunSkipsUnchangedMessages();
    void messageLimitKeepsCursorBeforeUnseenMessages();
    void missingFileIsExportedAgain();
    void honoursStatusErrors();
    void httpErrorStatusKeepsBodyAndHeaders();
    void unreachableHostHasNoStatusCode();
    void aadHintsExplainCommonMisconfigurations();

private:
    qint64 seedAccountAndFolders() {
        mail::MailStore store(*database_);
        mail::MailAccount account;
        account.displayName = QStringLiteral("Test user");
        account.tenantId = QStringLiteral("tenant");
        account.clientId = QStringLiteral("client");
        account.accountIdentifier = QStringLiteral("user@example.com");
        const Result<qint64> accountId = store.upsertAccount(account);
        if (!accountId) {
            qWarning("cannot seed the mail account: %s",
                     accountId.error().toDisplayString().c_str());
            return 0;
        }
        mail::MailFolderInfo inbox;
        inbox.id = QStringLiteral("inbox");
        inbox.displayName = QStringLiteral("Inbox");
        inbox.path = QStringLiteral("Inbox");
        store.storeFolders(accountId.value(), {inbox});
        return accountId.value();
    }

    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<db::Database> database_;
};

void MailTest::init() {
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    database_ = std::make_unique<db::Database>();
    QVERIFY(database_->open(directory_->filePath(QStringLiteral("nesva.db"))).ok());
    QVERIFY(database_->migrate().ok());
}

void MailTest::cleanup() {
    database_.reset();
    directory_.reset();
}

void MailTest::pkceMatchesRfc7636Vector() {
    // RFC 7636, Appendix B.
    QCOMPARE(mail::computePkceChallenge(
                 QStringLiteral("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")),
             QStringLiteral("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));

    const mail::PkcePair pair = mail::generatePkce();
    QVERIFY(pair.verifier.size() >= 43);
    QVERIFY(pair.verifier.size() <= 128);
    QCOMPARE(pair.challenge, mail::computePkceChallenge(pair.verifier));
    QCOMPARE(pair.method, QStringLiteral("S256"));
}

void MailTest::base64UrlHasNoPaddingOrUnsafeCharacters() {
    const QString encoded =
        mail::base64UrlEncode(QByteArray::fromHex("fbff00010203"));
    QVERIFY(!encoded.contains(QLatin1Char('=')));
    QVERIFY(!encoded.contains(QLatin1Char('+')));
    QVERIFY(!encoded.contains(QLatin1Char('/')));
}

void MailTest::authorizeUrlCarriesPkceAndScope() {
    mail::OAuthConfig config;
    config.tenantId = QStringLiteral("tenant-id");
    config.clientId = QStringLiteral("client-id");
    const mail::PkcePair pkce = mail::generatePkce();

    const QString url = mail::buildAuthorizeUrl(config, QStringLiteral("http://localhost:1234/"),
                                                pkce, QStringLiteral("state-123"));
    QVERIFY(url.startsWith(QStringLiteral("https://login.microsoftonline.com/tenant-id/oauth2/v2.0/authorize")));
    QVERIFY(url.contains(QStringLiteral("client_id=client-id")));
    QVERIFY(url.contains(QStringLiteral("code_challenge_method=S256")));
    QVERIFY(url.contains(QStringLiteral("state=state-123")));
    QVERIFY(url.contains(QStringLiteral("response_type=code")));
    QVERIFY(url.contains(QStringLiteral("Mail.Read")));
    QVERIFY(url.contains(QStringLiteral("offline_access")));
    QVERIFY(url.contains(QStringLiteral("redirect_uri=")));
    QVERIFY(url.contains(QStringLiteral("localhost")));
    QVERIFY(url.contains(QStringLiteral("localhost")));
}

void MailTest::tokenResponseParsing() {
    const Result<mail::TokenSet> tokens = mail::parseTokenResponse(
        R"({"access_token":"a","refresh_token":"r","expires_in":3600,"token_type":"Bearer","scope":"Mail.Read"})");
    QVERIFY(tokens);
    QCOMPARE(tokens.value().accessToken, QStringLiteral("a"));
    QCOMPARE(tokens.value().refreshToken, QStringLiteral("r"));
    QVERIFY(tokens.value().expiresAtUtc.isValid());
    QVERIFY(!tokens.value().needsRefresh());
}

void MailTest::rejectsErrorResponses() {
    const Result<mail::TokenSet> error = mail::parseTokenResponse(
        R"({"error":"invalid_grant","error_description":"AADSTS70008: expired"})");
    QVERIFY(!error);
    QCOMPARE(static_cast<int>(error.error().code()), static_cast<int>(ErrorCode::PermissionDenied));
    QVERIFY(error.error().detail().find("AADSTS70008") != std::string::npos);

    const Result<mail::TokenSet> garbage = mail::parseTokenResponse("not json");
    QVERIFY(!garbage);
}

void MailTest::sanitisesFileNames() {
    QCOMPARE(mail::MailExporter::sanitiseFileName(QStringLiteral("Re: [TEST] a/b:c*d?")),
             QStringLiteral("Re [TEST] abcd"));
    QCOMPARE(mail::MailExporter::sanitiseFileName(QStringLiteral("   spaced   out   ")),
             QStringLiteral("spaced out"));
    QCOMPARE(mail::MailExporter::sanitiseFileName(QStringLiteral("trailing...")),
             QStringLiteral("trailing"));
    QCOMPARE(mail::MailExporter::sanitiseFileName(QString()), QStringLiteral("message"));
    QVERIFY(mail::MailExporter::sanitiseFileName(QStringLiteral("x")).size() == 1);
    QCOMPARE(mail::MailExporter::sanitiseFileName(QString(300, QLatin1Char('a'))).size(), 100);
}

void MailTest::buildsDatedEmlPaths() {
    const QString path = mail::MailExporter::buildEmlRelativePath(
        QStringLiteral("Inbox"), QDateTime::fromString(QStringLiteral("2026-03-10T08:00:00Z"), Qt::ISODate),
        QStringLiteral("Invoice a/b"), QStringLiteral("<one@example.com>"));
    QVERIFY2(path.startsWith(QStringLiteral("Inbox/2026/03/")), qPrintable(path));
    QVERIFY(path.endsWith(QStringLiteral(".eml")));
    QVERIFY(!path.contains(QLatin1Char(':')));
    // The same key always produces the same path (stable identity).
    QCOMPARE(path, mail::MailExporter::buildEmlRelativePath(
                       QStringLiteral("Inbox"),
                       QDateTime::fromString(QStringLiteral("2026-03-10T08:00:00Z"), Qt::ISODate),
                       QStringLiteral("Invoice a/b"), QStringLiteral("<one@example.com>")));
}

void MailTest::exportsMessagesAndRecordsDelta() {
    const qint64 accountId = seedAccountAndFolders();
    QVERIFY(accountId > 0);
    auto transport = std::make_shared<FakeTransport>();
    transport->mimeProvider = [](const QString& url) {
        return QByteArray("From: x@example.com\r\nSubject: test\r\n\r\nBody for " +
                          url.toUtf8());
    };
    auto tokens = std::make_shared<mail::TokenProvider>(mail::OAuthConfig{}, QStringLiteral("r"),
                                                        transport);
    auto graph = std::make_shared<mail::GraphClient>(tokens, transport);
    const QString destination = directory_->filePath(QStringLiteral("archive"));

    mail::MailExporter exporter(*database_, graph);
    mail::MailExportOptions options;
    options.destinationDirectory = destination;
    const CancellationToken cancellation;
    const Result<mail::MailExportReport> report = exporter.run(accountId, options, cancellation);

    QVERIFY2(report.hasValue(), report.hasValue() ? "" : report.error().toDisplayString().c_str());
    QCOMPARE(report.value().folders, 1);
    QCOMPARE(report.value().exported, 2);
    QCOMPARE(report.value().failed, 0);
    QVERIFY(report.value().bytesWritten > 0);

    // Files exist, one per message, under folder/YYYY/MM/.
    QDir archive(destination);
    const QStringList monthDir = archive.entryList(
        {QStringLiteral("Inbox")}, QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(monthDir.size(), 1);
    QDir inbox(archive.absoluteFilePath(QStringLiteral("Inbox/2026/03")));
    QVERIFY(inbox.exists());
    QCOMPARE(inbox.entryList({QStringLiteral("*.eml")}, QDir::Files).size(), 2);

    // Metadata rows and the delta cursor are persisted for the next run.
    mail::MailStore store(*database_);
    const Result<QVector<mail::MailMessageRecord>> messages = store.messages(accountId, 10);
    QVERIFY(messages);
    QCOMPARE(messages.value().size(), 2);
    for (const mail::MailMessageRecord& message : messages.value()) {
        QCOMPARE(message.exportState, QStringLiteral("exported"));
        QVERIFY(!message.emlPath.isEmpty());
        QVERIFY(!message.emlSha256.isEmpty());
        QVERIFY(!message.messageKey.isEmpty());
    }
    const Result<QVector<mail::MailFolderRecord>> folders = store.folders(accountId);
    QVERIFY(folders);
    QVERIFY(!folders.value().constFirst().deltaLink.isEmpty());
}

void MailTest::secondRunSkipsUnchangedMessages() {
    const qint64 accountId = seedAccountAndFolders();
    QVERIFY(accountId > 0);
    auto transport = std::make_shared<FakeTransport>();
    auto tokens = std::make_shared<mail::TokenProvider>(mail::OAuthConfig{}, QStringLiteral("r"),
                                                        transport);
    auto graph = std::make_shared<mail::GraphClient>(tokens, transport);
    mail::MailExporter exporter(*database_, graph);
    mail::MailExportOptions options;
    options.destinationDirectory = directory_->filePath(QStringLiteral("archive"));
    const CancellationToken cancellation;

    QVERIFY(exporter.run(accountId, options, cancellation));
    const Result<mail::MailExportReport> second = exporter.run(accountId, options, cancellation);
    QVERIFY(second);
    // Same delta payload: nothing changed, nothing is downloaded again.
    QCOMPARE(second.value().exported, 0);
    QCOMPARE(second.value().skipped, 2);
    QCOMPARE(second.value().failed, 0);
}

void MailTest::missingFileIsExportedAgain() {
    // Skipping is decided by the file on disk as well as the database row: a
    // deleted .eml, or a new destination, must be repaired, not skipped.
    const qint64 accountId = seedAccountAndFolders();
    QVERIFY(accountId > 0);
    auto transport = std::make_shared<FakeTransport>();
    transport->mimeProvider = [](const QString& url) {
        return QByteArray("Subject: test\r\n\r\nBody for " + url.toUtf8());
    };
    auto tokens = std::make_shared<mail::TokenProvider>(mail::OAuthConfig{}, QStringLiteral("r"),
                                                        transport);
    auto graph = std::make_shared<mail::GraphClient>(tokens, transport);
    const CancellationToken cancellation;
    mail::MailExporter exporter(*database_, graph);

    const QString destination = directory_->filePath(QStringLiteral("archive"));
    mail::MailExportOptions options;
    options.destinationDirectory = destination;
    QCOMPARE(exporter.run(accountId, options, cancellation).value().exported, 2);

    // Remove one exported file: the next run restores it.
    mail::MailStore store(*database_);
    const Result<QVector<mail::MailMessageRecord>> messages = store.messages(accountId, 100);
    QVERIFY(messages);
    bool removedOne = false;
    for (const mail::MailMessageRecord& message : messages.value()) {
        const QString path = QDir(destination).absoluteFilePath(message.emlPath);
        if (QFile::exists(path)) {
            QVERIFY(QFile::remove(path));
            removedOne = true;
            break;
        }
    }
    QVERIFY(removedOne);
    QCOMPARE(exporter.run(accountId, options, cancellation).value().exported, 1);
}

void MailTest::messageLimitKeepsCursorBeforeUnseenMessages() {
    // A run bounded by --max-messages must not move the delta cursor past the
    // messages it did not examine: Microsoft would never report them again and
    // the archive would silently miss them.
    const qint64 accountId = seedAccountAndFolders();
    QVERIFY(accountId > 0);
    auto transport = std::make_shared<FakeTransport>();
    transport->mimeProvider = [](const QString& url) {
        return QByteArray("Subject: test\r\n\r\nBody for " + url.toUtf8());
    };
    auto tokens = std::make_shared<mail::TokenProvider>(mail::OAuthConfig{}, QStringLiteral("r"),
                                                        transport);
    auto graph = std::make_shared<mail::GraphClient>(tokens, transport);
    const QString destination = directory_->filePath(QStringLiteral("archive"));
    const CancellationToken cancellation;

    mail::MailExporter exporter(*database_, graph);
    mail::MailExportOptions limited;
    limited.destinationDirectory = destination;
    limited.maxMessages = 1;
    const Result<mail::MailExportReport> first = exporter.run(accountId, limited, cancellation);
    QVERIFY(first);
    QCOMPARE(first.value().exported, 1);

    mail::MailStore store(*database_);
    const Result<QVector<mail::MailFolderRecord>> folders = store.folders(accountId);
    QVERIFY(folders);
    QCOMPARE(folders.value().constFirst().deltaLink, QString());

    // The next run starts from the same point and picks up the second message.
    mail::MailExportOptions unbounded;
    unbounded.destinationDirectory = destination;
    const Result<mail::MailExportReport> second = exporter.run(accountId, unbounded, cancellation);
    QVERIFY(second);
    QCOMPARE(second.value().exported, 1);
    QCOMPARE(second.value().skipped, 1);
    QVERIFY(!store.folders(accountId).value().constFirst().deltaLink.isEmpty());
}

void MailTest::honoursStatusErrors() {
    const qint64 accountId = seedAccountAndFolders();
    QVERIFY(accountId > 0);
    auto transport = std::make_shared<FakeTransport>();
    transport->statusOverride = 403;
    auto tokens = std::make_shared<mail::TokenProvider>(mail::OAuthConfig{}, QStringLiteral("r"),
                                                        transport);
    auto graph = std::make_shared<mail::GraphClient>(tokens, transport);
    mail::MailExporter exporter(*database_, graph);
    mail::MailExportOptions options;
    options.destinationDirectory = directory_->filePath(QStringLiteral("archive"));
    const CancellationToken cancellation;
    const Result<mail::MailExportReport> report = exporter.run(accountId, options, cancellation);
    QVERIFY(report);
    // The folder error is reported, nothing is silently treated as success.
    QVERIFY(!report.value().errors.isEmpty());
    QCOMPARE(report.value().exported, 0);
}

void MailTest::aadHintsExplainCommonMisconfigurations() {
    // A desktop public client must never be told to supply a secret: the
    // 7000218 hint has to point at the registration setting instead.
    const QString confidential = mail::aadFixHint(QStringLiteral(
        R"({"error":"invalid_client","error_description":"AADSTS7000218: The request body must contain the following parameter: 'client_assertion' or 'client_secret'."})"));
    QVERIFY(confidential.contains(QStringLiteral("Allow public client flows")));
    QVERIFY(confidential.contains(QStringLiteral("Mobile and desktop applications")));

    const QString redirect = mail::aadFixHint(QStringLiteral(
        R"({"error":"invalid_grant","error_description":"AADSTS50011: The redirect URI specified in the request does not match."})"));
    QVERIFY(redirect.contains(QStringLiteral("http://localhost")));

    QVERIFY(mail::aadFixHint(QStringLiteral(R"({"error":"invalid_grant"})")).isEmpty());
    QVERIFY(mail::aadFixHint(QStringLiteral(R"({"error_description":"AADSTS999999: unknown"})"))
                .isEmpty());
}

void MailTest::httpErrorStatusKeepsBodyAndHeaders() {
    // Qt raises QNetworkReply::error() for HTTP 4xx/5xx. The transport must not
    // mistake that for "no response": the status code, the error document and
    // Retry-After are what callers need (refresh-once, backoff, diagnostics).
    const QByteArray body =
        R"({"error":"invalid_grant","error_description":"AADSTS50011: redirect mismatch"})";
    const QByteArray response = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n"
                                "Retry-After: 7\r\nContent-Length: " +
                                QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" +
                                body;
    CannedHttpServer server(response);
    QVERIFY(server.listen());

    net::QNetworkHttpTransport transport;
    const net::HttpResponse result =
        transport.send(net::HttpVerb::Post, server.url(),
                       {{QStringLiteral("Content-Type"),
                         QStringLiteral("application/x-www-form-urlencoded")}},
                       QByteArrayLiteral("client_id=probe"), 5000);

    QCOMPARE(result.statusCode, 400);
    QVERIFY(!result.isSuccess());
    QCOMPARE(result.body, body);
    QCOMPARE(result.headers.value(QStringLiteral("retry-after")), QStringLiteral("7"));
    QVERIFY(result.error.isEmpty());
}

void MailTest::unreachableHostHasNoStatusCode() {
    // A closed port is a transport failure: status 0 with a non-empty reason,
    // which is what the callers report instead of a misleading "(0: )".
    quint16 port = 0;
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        port = probe.serverPort();
    }

    net::QNetworkHttpTransport transport;
    const net::HttpResponse result = transport.send(
        net::HttpVerb::Get, QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(port)), {}, {}, 5000);

    QCOMPARE(result.statusCode, 0);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.body.isEmpty());
}

QTEST_GUILESS_MAIN(MailTest)
#include "tst_mail.moc"
