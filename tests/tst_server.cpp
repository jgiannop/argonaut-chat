#include "ChatClient.h"
#include "Attachment.h"
#include "ChatServer.h"
#include "RateLimiter.h"
#include "ServerStore.h"
#include "Session.h"
#include "TestSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QWebSocket>

#include <memory>

namespace {

constexpr int kWait = 5000;

// One person with one device, as the app will be.
struct Person
{
    explicit Person(const QString &username)
        : name(username)
    {
        signedPreKey = X3dh::makeSignedPreKey(identity, 1);
        oneTime = X3dh::makeOneTimePreKeys(1, 5);
        QObject::connect(&client, &ChatClient::envelopeReceived,
                         [this](const Protocol::Envelope &envelope) { inbox.append(envelope); });
    }

    QString name;
    Crypto::IdentityKeyPair identity = Crypto::IdentityKeyPair::generate();
    X3dh::SignedPreKey signedPreKey;
    QList<X3dh::OneTimePreKey> oneTime;
    ChatClient client;
    QList<Protocol::Envelope> inbox;
    std::optional<Session> session;

    // The plaintext of a message from someone, starting the session if needed.
    QString read(const Protocol::Envelope &envelope)
    {
        const auto message = Protocol::decodeCipherMessage(envelope.message);
        if (!message)
            return QStringLiteral("<bad message>");
        std::optional<QByteArray> plaintext;
        if (session)
            plaintext = session->decrypt(*message);
        if (!plaintext && message->preKey) {
            auto accepted = Session::accept(
                identity, *message,
                [this](quint32 id) -> std::optional<X3dh::SignedPreKey> {
                    return id == signedPreKey.id ? std::optional(signedPreKey) : std::nullopt;
                },
                [this](quint32 id) -> std::optional<X3dh::OneTimePreKey> {
                    for (const auto &key : oneTime) {
                        if (key.id == id)
                            return key;
                    }
                    return std::nullopt;
                });
            if (accepted) {
                session = accepted->session;
                plaintext = accepted->plaintext;
                if (accepted->usedOneTimePreKey)
                    oneTime.removeIf([&](const auto &key) { return key.id == *accepted->usedOneTimePreKey; });
            }
        }
        if (!plaintext)
            return QStringLiteral("<failed>");
        const auto content = Protocol::decodeContent(*plaintext);
        return content ? content->body : QStringLiteral("<bad content>");
    }
};

// A TCP relay that can stop passing anything on while both connections stay
// open: a connection that died quietly (a router that forgot it).
class Relay : public QObject
{
public:
    explicit Relay(quint16 target)
    {
        m_server.listen(QHostAddress::LocalHost);
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this, target] {
            while (QTcpSocket *down = m_server.nextPendingConnection()) {
                auto *up = new QTcpSocket(this);
                up->connectToHost(QHostAddress::LocalHost, target);
                pipe(down, up);
                pipe(up, down);
            }
        });
    }
    quint16 port() const { return m_server.serverPort(); }
    void freeze() { m_frozen = true; }

private:
    void pipe(QTcpSocket *from, QTcpSocket *to)
    {
        QObject::connect(from, &QTcpSocket::readyRead, this, [this, from, to] {
            const QByteArray bytes = from->readAll();
            if (!m_frozen)
                to->write(bytes); // frozen: swallowed, nobody told
        });
    }

    QTcpServer m_server;
    bool m_frozen = false;
};

// Waits for the answer to one request.
ChatClient::Result wait(const std::function<void(const ChatClient::Callback &)> &call)
{
    std::optional<ChatClient::Result> got;
    call([&got](const ChatClient::Result &result) { got = result; });
    (void)QTest::qWaitFor([&got] { return got.has_value(); }, kWait);
    if (!got) {
        ChatClient::Result timeout;
        timeout.error = QStringLiteral("timeout");
        return timeout;
    }
    return *got;
}

} // namespace

class TestServer : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        m_store = std::make_unique<ServerStore>(m_dir.filePath(QStringLiteral("server-%1/server.sqlite").arg(++m_count)));
        QVERIFY(m_store->isOpen());
        m_server = std::make_unique<ChatServer>(m_store.get());
        // Limits that never get in the way here; rateLimits() sets its own.
        ChatServer::Limits generous;
        const RateLimiter::Rule plenty{1e6, 1e6};
        generous.requestsPerConnection = generous.signUpsPerAddress = generous.challengesPerAddress = plenty;
        generous.sendsPerAccount = generous.keyFetchesPerAccount = generous.preKeyUploadsPerAccount = plenty;
        generous.oneTimePreKeysPerDevice = plenty;
        generous.attachmentUploadsPerAccount = plenty;
        generous.uploadBytesPerAccount = generous.downloadBytesPerAccount = {1e12, 1e12}; // bytes
        m_server->setLimits(generous);
        m_server->setClock([this] { return m_now; });
        QVERIFY2(m_server->listen(), qPrintable(m_server->errorString()));
        m_url = QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(m_server->port()));
    }

    void cleanup()
    {
        m_server.reset();
        m_store.reset();
    }

    void signUpAndSignIn()
    {
        Person alice(QStringLiteral("alice"));
        QVERIFY(join(alice));
        QCOMPARE(m_store->devices("alice"), QList<quint32>{1});

        // The name is taken.
        Person other(QStringLiteral("alice"));
        QVERIFY(connectTo(other.client));
        ChatClient::Result result = signUp(other);
        QCOMPARE(result.error, QString("taken"));

        // Alice from another connection, with her key.
        ChatClient again;
        QVERIFY(connectTo(again));
        result = wait([&](auto done) { again.login("alice", 1, alice.identity, done); });
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(result.data.value("prekeys").toInt(), 5);
        // ... which pushes the first one out.
        QTRY_VERIFY_WITH_TIMEOUT(!alice.client.isConnected(), kWait);

        // Someone else's key: refused, and the same answer as for no account.
        ChatClient mallory;
        QVERIFY(connectTo(mallory));
        const auto wrongKey = Crypto::IdentityKeyPair::generate();
        result = wait([&](auto done) { mallory.login("alice", 1, wrongKey, done); });
        QCOMPARE(result.error, QString("denied"));
        result = wait([&](auto done) { mallory.login("nobody", 1, wrongKey, done); });
        QCOMPARE(result.error, QString("denied"));
        // Without a challenge.
        result = wait([&](auto done) { mallory.request("login", {{"username", "alice"}, {"device", 1}}, done); });
        QCOMPARE(result.error, QString("no-challenge"));
        // Nothing before signing in.
        result = wait([&](auto done) { mallory.fetchKeys("alice", done); });
        QCOMPARE(result.error, QString("unauthenticated"));
        result = wait([&](auto done) { mallory.request("nonsense", {}, done); });
        QCOMPARE(result.error, QString("unknown-op"));
    }

    void signUpIsChecked()
    {
        Person eve(QStringLiteral("eve"));
        QVERIFY(connectTo(eve.client));
        auto signUpWith = [&](const QString &username, const QByteArray &signedName,
                              const X3dh::SignedPreKey &signedPreKey) {
            return wait([&](auto done) {
                eve.client.request("challenge", {}, [&, done](const ChatClient::Result &challenge) {
                    const QByteArray nonce = QByteArray::fromBase64(challenge.data.value("nonce").toString().toLatin1());
                    eve.client.request(
                        "register",
                        {{"username", username},
                         {"identityKey", Protocol::Json::base64(eve.identity.publicKey())},
                         {"signedPreKey", Protocol::signedPreKeyToJson(Protocol::publicPart(signedPreKey))},
                         {"signature", Protocol::Json::base64(eve.identity.sign(
                                           Protocol::authMessage(nonce, QString::fromLatin1(signedName), 1)))}},
                        done);
                });
            });
        };
        // Signed for another name.
        QCOMPARE(signUpWith("eve", "mallory", eve.signedPreKey).error, QString("denied"));
        // A signed prekey signed by another key.
        const auto stranger = Crypto::IdentityKeyPair::generate();
        QCOMPARE(signUpWith("eve", "eve", X3dh::makeSignedPreKey(stranger, 1)).error, QString("denied"));
        // A name that is not one.
        QCOMPARE(signUpWith("Eve", "Eve", eve.signedPreKey).error, QString("bad-username"));
        // Too many failures: the connection goes.
        signUpWith("eve", "mallory", eve.signedPreKey);
        signUpWith("eve", "mallory", eve.signedPreKey);
        QTRY_VERIFY_WITH_TIMEOUT(!eve.client.isConnected(), kWait);
        QVERIFY(!m_store->accountExists("eve"));

        // Done right, it works.
        Person eve2(QStringLiteral("eve"));
        QVERIFY(join(eve2));
    }

    // Alice and Bob through the real server, encrypted end to end.
    void exchangeMessages()
    {
        Person alice(QStringLiteral("alice"));
        Person bob(QStringLiteral("bob"));
        QVERIFY(join(alice));
        QVERIFY(join(bob));

        const ChatClient::Result keys = wait([&](auto done) { alice.client.fetchKeys("bob", done); });
        const auto bundles = ChatClient::bundles(keys);
        QVERIFY(bundles);
        QCOMPARE(bundles->size(), 1);
        QCOMPARE(bundles->first().device, 1u);
        QCOMPARE(bundles->first().bundle.identityKey, bob.identity.publicKey());
        QVERIFY(bundles->first().bundle.oneTimePreKeyId);
        alice.session = Session::initiate(alice.identity, bundles->first().bundle);
        QVERIFY(alice.session);

        QVERIFY(sendText(alice, "bob", "Hello Bob"));
        QTRY_COMPARE_WITH_TIMEOUT(bob.inbox.size(), 1, kWait);
        Protocol::Envelope envelope = bob.inbox.takeFirst();
        QCOMPARE(envelope.from, QString("alice"));
        QCOMPARE(envelope.fromDevice, 1u);
        QVERIFY(!envelope.message.contains("Hello")); // the server only ever had this
        QCOMPARE(bob.read(envelope), QString("Hello Bob"));
        QVERIFY(wait([&](auto done) { bob.client.acknowledge({envelope.id}, done); }).ok);
        QCOMPARE(m_store->queuedCount("bob", 1), 0);

        QVERIFY(sendText(bob, "alice", "Hi Alice"));
        QTRY_COMPARE_WITH_TIMEOUT(alice.inbox.size(), 1, kWait);
        envelope = alice.inbox.takeFirst();
        QCOMPARE(alice.read(envelope), QString("Hi Alice"));
        QVERIFY(wait([&](auto done) { alice.client.acknowledge({envelope.id}, done); }).ok);

        // Bob goes offline; the server keeps Alice's messages for him.
        bob.client.close();
        QTRY_VERIFY_WITH_TIMEOUT(!bob.client.isConnected(), kWait);
        QVERIFY(sendText(alice, "bob", "one"));
        QVERIFY(sendText(alice, "bob", "two"));
        QCOMPARE(m_store->queuedCount("bob", 1), 2);

        QVERIFY(connectTo(bob.client));
        QVERIFY(wait([&](auto done) { bob.client.login("bob", 1, bob.identity, done); }).ok);
        QTRY_COMPARE_WITH_TIMEOUT(bob.inbox.size(), 2, kWait);
        QCOMPARE(bob.read(bob.inbox.at(0)), QString("one"));
        QCOMPARE(bob.read(bob.inbox.at(1)), QString("two"));
        // Only the first is acknowledged: the second comes again next time.
        QVERIFY(wait([&](auto done) { bob.client.acknowledge({bob.inbox.at(0).id}, done); }).ok);
        const QString unacknowledged = bob.inbox.at(1).id;
        bob.inbox.clear();
        bob.client.close();
        QTRY_VERIFY_WITH_TIMEOUT(!bob.client.isConnected(), kWait);
        QVERIFY(connectTo(bob.client));
        QVERIFY(wait([&](auto done) { bob.client.login("bob", 1, bob.identity, done); }).ok);
        QTRY_COMPARE_WITH_TIMEOUT(bob.inbox.size(), 1, kWait);
        QCOMPARE(bob.inbox.first().id, unacknowledged);
        QVERIFY(wait([&](auto done) { bob.client.acknowledge({unacknowledged}, done); }).ok);
        QCOMPARE(m_store->queuedCount("bob", 1), 0);

        // Acknowledging someone else's message does nothing to it.
        QVERIFY(sendText(bob, "alice", "for Alice"));
        QTRY_COMPARE_WITH_TIMEOUT(alice.inbox.size(), 1, kWait);
        QVERIFY(wait([&](auto done) { bob.client.acknowledge({alice.inbox.first().id}, done); }).ok);
        QCOMPARE(m_store->queuedCount("alice", 1), 1);
    }

    void sendIsChecked()
    {
        Person alice(QStringLiteral("alice"));
        Person bob(QStringLiteral("bob"));
        QVERIFY(join(alice));
        QVERIFY(join(bob));
        const QByteArray cipher = Protocol::encodeCipherMessage({false, {}, {}, 0, {}, QByteArray(60, 'x')});

        // Bob has device 1 only: the server says so.
        ChatClient::Result result = wait([&](auto done) { alice.client.send("bob", {{2, cipher}}, done); });
        QCOMPARE(result.error, QString("devices"));
        QCOMPARE(result.data.value("devices").toArray(), QJsonArray{1});
        result = wait([&](auto done) { alice.client.send("bob", {{1, cipher}, {2, cipher}}, done); });
        QCOMPARE(result.error, QString("devices"));
        // Nobody.
        result = wait([&](auto done) { alice.client.send("nobody", {{1, cipher}}, done); });
        QCOMPARE(result.error, QString("not-found"));
        // Not a CipherMessage, nothing at all.
        result = wait([&](auto done) { alice.client.send("bob", {{1, QByteArray("\x07rubbish")}}, done); });
        QCOMPARE(result.error, QString("bad-request"));
        result = wait([&](auto done) { alice.client.send("bob", {}, done); });
        QCOMPARE(result.error, QString("bad-request"));
        result = wait([&](auto done) { alice.client.request("send", {{"to", "bob"}, {"messages", "x"}}, done); });
        QCOMPARE(result.error, QString("bad-request"));
        result = wait([&](auto done) { alice.client.request("ack", {{"ids", QJsonArray{1}}}, done); });
        QCOMPARE(result.error, QString("bad-request"));
        QCOMPARE(m_store->queuedCount("bob", 1), 0);
        QVERIFY(bob.inbox.isEmpty());
    }

    void rubbishClosesTheConnection_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("binary");
        QTest::newRow("not json") << QString("hello?") << false;
        QTest::newRow("an array") << QString("[1,2]") << false;
        QTest::newRow("no rid") << QString(R"({"op":"challenge"})") << false;
        QTest::newRow("no op") << QString(R"({"rid":1})") << false;
        QTest::newRow("negative rid") << QString(R"({"rid":-1,"op":"challenge"})") << false;
        QTest::newRow("binary") << QString("binary") << true;
    }

    void rubbishClosesTheConnection()
    {
        QFETCH(QString, text);
        QFETCH(bool, binary);
        QWebSocket socket;
        QSignalSpy closed(&socket, &QWebSocket::disconnected);
        socket.open(m_url);
        QTRY_COMPARE_WITH_TIMEOUT(socket.state(), QAbstractSocket::ConnectedState, kWait);
        if (binary)
            socket.sendBinaryMessage(text.toUtf8());
        else
            socket.sendTextMessage(text);
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(m_server->connectionCount(), 0, kWait);
    }

    void preKeys()
    {
        Person alice(QStringLiteral("alice"));
        Person bob(QStringLiteral("bob"));
        bob.oneTime = X3dh::makeOneTimePreKeys(1, 2);
        QVERIFY(join(alice));
        QVERIFY(join(bob));

        // Each fetch takes one; then there are none, and X3DH goes without.
        QList<std::optional<quint32>> ids;
        for (int i = 0; i < 3; ++i) {
            const auto bundles = ChatClient::bundles(wait([&](auto done) { alice.client.fetchKeys("bob", done); }));
            QVERIFY(bundles);
            ids << bundles->first().bundle.oneTimePreKeyId;
        }
        QVERIFY(ids == (QList<std::optional<quint32>>{1u, 2u, std::nullopt}));
        QCOMPARE(wait([&](auto done) { alice.client.fetchKeys("nobody", done); }).error, QString("not-found"));

        // Bob tops them up, and changes his signed prekey.
        const auto more = X3dh::makeOneTimePreKeys(3, 3);
        const auto newSigned = X3dh::makeSignedPreKey(bob.identity, 2);
        ChatClient::Result result = wait([&](auto done) {
            bob.client.uploadPreKeys(Protocol::publicPart(newSigned), Protocol::publicParts(more), done);
        });
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(result.data.value("prekeys").toInt(), 3);
        const auto bundles = ChatClient::bundles(wait([&](auto done) { alice.client.fetchKeys("bob", done); }));
        QCOMPARE(bundles->first().bundle.signedPreKeyId, 2u);
        QCOMPARE(bundles->first().bundle.signedPreKey, newSigned.pair.publicKey);
        // A signed prekey signed by someone else is refused.
        const auto forged = X3dh::makeSignedPreKey(Crypto::IdentityKeyPair::generate(), 3);
        result = wait([&](auto done) { bob.client.uploadPreKeys(Protocol::publicPart(forged), {}, done); });
        QCOMPARE(result.error, QString("denied"));
        // Never more than the limit kept.
        for (quint32 first = 100; first < 100 + 3 * Protocol::MaxOneTimePreKeysPerUpload;
             first += Protocol::MaxOneTimePreKeysPerUpload) {
            const auto batch = X3dh::makeOneTimePreKeys(first, Protocol::MaxOneTimePreKeysPerUpload);
            result = wait([&](auto done) { bob.client.uploadPreKeys({}, Protocol::publicParts(batch), done); });
            QVERIFY(result.ok);
        }
        QCOMPARE(m_store->oneTimePreKeyCount("bob", 1), ServerStore::MaxOneTimePreKeys);
    }

    void rateLimiter()
    {
        qint64 now = 0;
        RateLimiter limiter([&now] { return now; });
        const RateLimiter::Rule rule{3, 1}; // a burst of 3, then one a second
        for (int i = 0; i < 3; ++i)
            QVERIFY(limiter.take("a", rule));
        qint64 retry = 0;
        QVERIFY(!limiter.take("a", rule, &retry));
        QCOMPARE(retry, 1000);
        QVERIFY(limiter.take("b", rule)); // another key, its own bucket
        now += 500;
        QVERIFY(!limiter.take("a", rule, &retry));
        QCOMPARE(retry, 500);
        now += 500;
        QVERIFY(limiter.take("a", rule));
        QVERIFY(!limiter.take("a", rule));
        // Full again after a while: forgotten.
        now += 10000;
        limiter.prune();
        QCOMPARE(limiter.size(), 0);
    }

    void rateLimits()
    {
        ChatServer::Limits limits = m_server->limits();
        limits.signUpsPerAddress = {2, 1.0 / 3600};
        limits.sendsPerAccount = {3, 1};
        limits.keyFetchesPerAccount = {4, 1.0 / 60};
        limits.oneTimePreKeysPerDevice = {2, 1.0 / 3600};
        m_server->setLimits(limits);

        // Two sign-ups from here, then wait an hour.
        Person alice(QStringLiteral("alice"));
        Person bob(QStringLiteral("bob"));
        Person carol(QStringLiteral("carol"));
        QVERIFY(join(alice));
        QVERIFY(join(bob));
        QVERIFY(connectTo(carol.client));
        ChatClient::Result result = signUp(carol);
        QCOMPARE(result.error, QString("rate-limited"));
        QVERIFY(result.data.value("retryAfter").toDouble() > 0);
        m_now += 3600 * 1000;
        QVERIFY(signUp(carol).ok);

        // One-time prekeys: two, then bundles without, though Bob has more.
        QList<bool> withOneTime;
        for (int i = 0; i < 3; ++i) {
            const auto bundles = ChatClient::bundles(wait([&](auto done) { alice.client.fetchKeys("bob", done); }));
            QVERIFY(bundles);
            withOneTime << bundles->first().bundle.oneTimePreKeyId.has_value();
        }
        QCOMPARE(withOneTime, (QList<bool>{true, true, false}));
        QCOMPARE(m_store->oneTimePreKeyCount("bob", 1), 3);
        // Key fetches per account: the fourth went, the fifth waits.
        QVERIFY(wait([&](auto done) { alice.client.fetchKeys("bob", done); }).ok);
        QCOMPARE(wait([&](auto done) { alice.client.fetchKeys("bob", done); }).error, QString("rate-limited"));

        // Sends: a burst of three, then one a second.
        const QByteArray cipher = Protocol::encodeCipherMessage({false, {}, {}, 0, {}, QByteArray(60, 'x')});
        for (int i = 0; i < 3; ++i)
            QVERIFY(wait([&](auto done) { alice.client.send("bob", {{1, cipher}}, done); }).ok);
        result = wait([&](auto done) { alice.client.send("bob", {{1, cipher}}, done); });
        QCOMPARE(result.error, QString("rate-limited"));
        QCOMPARE(result.data.value("retryAfter").toInt(), 1000);
        // Others are not held back by Alice.
        QVERIFY(wait([&](auto done) { bob.client.send("alice", {{1, cipher}}, done); }).ok);
        m_now += 1000;
        QVERIFY(wait([&](auto done) { alice.client.send("bob", {{1, cipher}}, done); }).ok);
    }

    void connectionsPerAddress()
    {
        ChatServer::Limits limits = m_server->limits();
        limits.connectionsPerAddress = 2;
        m_server->setLimits(limits);

        auto openFrom = [this](const QByteArray &forwardedFor) {
            auto socket = std::make_unique<QWebSocket>();
            QNetworkRequest request(m_url);
            if (!forwardedFor.isEmpty())
                request.setRawHeader("X-Forwarded-For", forwardedFor);
            socket->open(request);
            return socket;
        };
        auto settled = [this](int count) {
            return QTest::qWaitFor([&] { return m_server->connectionCount() == count; }, kWait);
        };

        // Without a trusted proxy, everyone here is 127.0.0.1: the third goes.
        auto a = openFrom("198.51.100.1");
        auto b = openFrom("198.51.100.2");
        QVERIFY(settled(2));
        auto c = openFrom("198.51.100.3");
        QTRY_COMPARE_WITH_TIMEOUT(c->state(), QAbstractSocket::UnconnectedState, kWait);
        QCOMPARE(m_server->connectionCount(), 2);
        a.reset();
        b.reset();
        c.reset();
        QVERIFY(settled(0));

        // Behind a trusted proxy, the address it forwards counts (the last one).
        m_server->setTrustProxy(true);
        auto d = openFrom("10.0.0.9, 198.51.100.1");
        auto e = openFrom("198.51.100.1");
        auto f = openFrom("198.51.100.2");
        QVERIFY(settled(3));
        auto g = openFrom("198.51.100.1");
        QTRY_COMPARE_WITH_TIMEOUT(g->state(), QAbstractSocket::UnconnectedState, kWait);
        QCOMPARE(m_server->connectionCount(), 3);
        // Rubbish in the header: the proxy's own address.
        auto h = openFrom("not an address");
        QVERIFY(settled(4));
    }

    void clientDropsADeadConnection()
    {
        Relay relay(m_server->port());
        ChatClient client;
        client.setKeepAlive(100, 100);
        QSignalSpy lost(&client, &ChatClient::disconnected);
        QVERIFY(client.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(relay.port()))));
        QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), kWait);
        // Alive: the server answers the pings, round after round.
        QTest::qWait(600);
        QVERIFY(client.isConnected());
        QCOMPARE(lost.size(), 0);
        // Quietly dead: noticed, and closed.
        relay.freeze();
        QTRY_COMPARE_WITH_TIMEOUT(lost.size(), 1, kWait);
        QVERIFY(!client.isConnected());
    }

    void serverDropsADeadConnection()
    {
        m_server->setKeepAlive(100);
        Relay relay(m_server->port());
        QWebSocket socket; // Qt answers the server's pings by itself
        socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(relay.port())));
        QTRY_COMPARE_WITH_TIMEOUT(m_server->connectionCount(), 1, kWait);
        QTest::qWait(600);
        QCOMPARE(m_server->connectionCount(), 1);
        relay.freeze();
        QTRY_COMPARE_WITH_TIMEOUT(m_server->connectionCount(), 0, kWait);
    }

    void attachments()
    {
        Person alice(QStringLiteral("alice"));
        Person bob(QStringLiteral("bob"));
        QVERIFY(join(alice));
        QVERIFY(join(bob));
        const QByteArray file = Crypto::randomBytes(1300 * 1024); // three chunks and a bit
        const auto sealed = Attachment::encrypt(file);

        // Up, with progress.
        QList<qint64> progress;
        std::optional<QString> id;
        alice.client.uploadAttachment(
            sealed->blob, [&](qint64 done, qint64) { progress << done; },
            [&](const QString &uploaded, const ChatClient::Result &result) {
                QVERIFY2(result.ok, qPrintable(result.message));
                id = uploaded;
            });
        QTRY_VERIFY_WITH_TIMEOUT(id.has_value(), kWait);
        QVERIFY(Attachment::isValidId(*id));
        QCOMPARE(progress.last(), sealed->blob.size());
        QVERIFY(progress.size() >= 3);
        // On the server: only the encrypted bytes, in a private folder.
        QFile stored(m_store->attachmentFolder() + "/" + *id);
        QVERIFY(stored.open(QIODevice::ReadOnly));
        QCOMPARE(stored.readAll(), sealed->blob);
        QCOMPARE(QFileInfo(m_store->attachmentFolder()).permissions() & (QFile::ReadGroup | QFile::ReadOther),
                 QFile::Permissions());

        // Down, by someone else, and decrypted.
        std::optional<QByteArray> blob;
        bob.client.downloadAttachment(*id, sealed->blob.size(), {},
                                      [&](const QByteArray &got, const ChatClient::Result &result) {
                                          QVERIFY2(result.ok, qPrintable(result.message));
                                          blob = got;
                                      });
        QTRY_VERIFY_WITH_TIMEOUT(blob.has_value(), kWait);
        QCOMPARE(Attachment::decrypt(*blob, sealed->key, sealed->digest, file.size()), std::optional<QByteArray>(file));

        // Not the size the message promised: refused.
        std::optional<ChatClient::Result> wrong;
        bob.client.downloadAttachment(*id, sealed->blob.size() + 1, {},
                                      [&](const QByteArray &, const ChatClient::Result &result) { wrong = result; });
        QTRY_VERIFY_WITH_TIMEOUT(wrong.has_value(), kWait);
        QVERIFY(!wrong->ok);
    }

    void attachmentsFromStrangers()
    {
        Person alice(QStringLiteral("alice"));
        Person bob(QStringLiteral("bob"));
        QVERIFY(join(alice));
        QVERIFY(join(bob));
        ChatClient::Result result = wait([&](auto done) { alice.client.request("upload", {{"size", 1000}}, done); });
        QVERIFY(result.ok);
        const QString id = result.data.value("id").toString();
        const QString chunk = Protocol::Json::base64(QByteArray(600, 'x'));

        // Someone else's upload; the wrong place; past the end.
        result = wait([&](auto done) { bob.client.request("upload-chunk", {{"id", id}, {"offset", 0}, {"data", chunk}}, done); });
        QCOMPARE(result.error, QString("not-found"));
        result = wait([&](auto done) { alice.client.request("upload-chunk", {{"id", id}, {"offset", 10}, {"data", chunk}}, done); });
        QCOMPARE(result.error, QString("bad-offset"));
        QVERIFY(wait([&](auto done) { alice.client.request("upload-chunk", {{"id", id}, {"offset", 0}, {"data", chunk}}, done); }).ok);
        result = wait([&](auto done) { alice.client.request("upload-chunk", {{"id", id}, {"offset", 600}, {"data", chunk}}, done); });
        QCOMPARE(result.error, QString("bad-offset"));
        // Unfinished: nobody gets it.
        result = wait([&](auto done) { bob.client.request("download", {{"id", id}, {"offset", 0}, {"length", 100}}, done); });
        QCOMPARE(result.error, QString("not-found"));
        // Rubbish.
        for (const QJsonObject &bad : {QJsonObject{{"id", "../../etc/passwd"}, {"offset", 0}, {"length", 10}},
                                       QJsonObject{{"id", id}, {"offset", -1}, {"length", 10}},
                                       QJsonObject{{"id", id}, {"offset", 0}, {"length", 10 * 1024 * 1024}}}) {
            result = wait([&](auto done) { bob.client.request("download", bad, done); });
            QCOMPARE(result.error, QString("bad-request"));
        }
        result = wait([&](auto done) { bob.client.request("download", {{"id", "0123456789abcdef0123456789abcdef"}, {"offset", 0}, {"length", 10}}, done); });
        QCOMPARE(result.error, QString("not-found"));
        result = wait([&](auto done) { alice.client.request("upload", {{"size", double(Attachment::blobSize(Attachment::MaxFileBytes) + 1)}}, done); });
        QCOMPARE(result.error, QString("bad-request"));
        ChatClient stranger;
        QVERIFY(connectTo(stranger));
        result = wait([&](auto done) { stranger.request("upload", {{"size", 1000}}, done); });
        QCOMPARE(result.error, QString("unauthenticated"));

        // The server full; too many bytes for one account.
        ChatServer::Limits limits = m_server->limits();
        limits.maxStoredAttachmentBytes = 5000;
        m_server->setLimits(limits);
        result = wait([&](auto done) { alice.client.request("upload", {{"size", 4500}}, done); });
        QCOMPARE(result.error, QString("full"));
        limits.maxStoredAttachmentBytes = 1LL << 40;
        limits.uploadBytesPerAccount = {10000, 1};
        m_server->setLimits(limits);
        QVERIFY(wait([&](auto done) { alice.client.request("upload", {{"size", 8000}}, done); }).ok);
        result = wait([&](auto done) { alice.client.request("upload", {{"size", 8000}}, done); });
        QCOMPARE(result.error, QString("rate-limited"));

        // After 30 days (unfinished: a day), gone, files and all.
        const QString finished = [&] {
            const auto sealed = Attachment::encrypt("small");
            std::optional<QString> uploaded;
            bob.client.uploadAttachment(sealed->blob, {}, [&](const QString &done, const ChatClient::Result &) { uploaded = done; });
            if (!QTest::qWaitFor([&] { return uploaded.has_value(); }, kWait))
                return QString();
            return uploaded.value_or(QString());
        }();
        QVERIFY(m_store->attachment(finished));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 day = 24LL * 3600 * 1000;
        QCOMPARE(m_store->removeOldAttachments(now + 2 * day, 30 * day, day), 2); // the two unfinished ones
        QVERIFY(m_store->attachment(finished));
        QCOMPARE(m_store->removeOldAttachments(now + 31 * day, 30 * day, day), 1);
        QVERIFY(!m_store->attachment(finished));
        QCOMPARE(QDir(m_store->attachmentFolder()).entryList(QDir::Files).size(), 0);
    }

    void linkedDevices()
    {
        Person alice(QStringLiteral("alice"));
        Person bob(QStringLiteral("bob"));
        QVERIFY(join(alice));
        QVERIFY(join(bob));

        // A new computer opens a slot; Alice's sends into it, once.
        ChatClient fresh;
        QVERIFY(connectTo(fresh));
        QSignalSpy provisioned(&fresh, &ChatClient::provisionReceived);
        ChatClient::Result result = wait([&](auto done) { fresh.openProvisioning(done); });
        QVERIFY(result.ok);
        const QString slot = result.data.value("id").toString();
        QVERIFY(Attachment::isValidId(slot));
        const auto oneTime = Crypto::DhKeyPair::generate();
        const QByteArray sealed = Crypto::sealTo(oneTime.publicKey, "the keys");
        QVERIFY(wait([&](auto done) { alice.client.sendProvisioning(slot, sealed, done); }).ok);
        QTRY_COMPARE_WITH_TIMEOUT(provisioned.size(), 1, kWait);
        QCOMPARE(Crypto::openSealed(oneTime, provisioned.at(0).at(0).toByteArray()), std::optional<QByteArray>("the keys"));
        QCOMPARE(wait([&](auto done) { alice.client.sendProvisioning(slot, sealed, done); }).error, QString("not-found"));
        QCOMPARE(wait([&](auto done) { fresh.sendProvisioning(slot, sealed, done); }).error, QString("unauthenticated"));

        // It joins Alice's account: only with her identity key.
        const auto spk = X3dh::makeSignedPreKey(alice.identity, 50);
        const auto stranger = Crypto::IdentityKeyPair::generate();
        result = wait([&](auto done) {
            fresh.addDevice("alice", stranger, Protocol::publicPart(X3dh::makeSignedPreKey(stranger, 1)), {}, done);
        });
        QCOMPARE(result.error, QString("denied"));
        result = wait([&](auto done) {
            fresh.addDevice("alice", alice.identity, Protocol::publicPart(spk),
                            Protocol::publicParts(X3dh::makeOneTimePreKeys(500, 3)), done);
        });
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(result.data.value("device").toInt(), 2);
        QCOMPARE(m_store->devices("alice"), (QList<quint32>{1, 2}));

        // Now Alice has two devices: Bob must send to both; Alice to herself, to the other one.
        const auto bundles = ChatClient::bundles(wait([&](auto done) { bob.client.fetchKeys("alice", done); }));
        QCOMPARE(bundles->size(), 2);
        QCOMPARE(bundles->at(1).bundle.identityKey, alice.identity.publicKey()); // one identity
        const QByteArray cipher = Protocol::encodeCipherMessage({false, {}, {}, 0, {}, QByteArray(60, 'x')});
        result = wait([&](auto done) { bob.client.send("alice", {{1, cipher}}, done); });
        QCOMPARE(result.error, QString("devices"));
        QCOMPARE(result.data.value("devices").toArray(), (QJsonArray{1, 2}));
        QVERIFY(wait([&](auto done) { bob.client.send("alice", {{1, cipher}, {2, cipher}}, done); }).ok);
        result = wait([&](auto done) { alice.client.send("alice", {{2, cipher}}, done); });
        QVERIFY2(result.ok, qPrintable(result.message));
        QCOMPARE(m_store->queuedCount("alice", 2), 2);

        // The list; only the first device removes the others.
        result = wait([&](auto done) { fresh.listDevices(done); });
        QCOMPARE(result.data.value("devices").toArray().size(), 2);
        QCOMPARE(wait([&](auto done) { fresh.removeDevice(1, done); }).error, QString("denied"));
        QSignalSpy gone(&fresh, &ChatClient::disconnected);
        QVERIFY(wait([&](auto done) { alice.client.removeDevice(2, done); }).ok);
        QTRY_COMPARE_WITH_TIMEOUT(gone.size(), 1, kWait);
        QCOMPARE(m_store->devices("alice"), (QList<quint32>{1}));
        QCOMPARE(m_store->queuedCount("alice", 2), 0);
        ChatClient again;
        QVERIFY(connectTo(again));
        QCOMPARE(wait([&](auto done) { again.login("alice", 2, alice.identity, done); }).error, QString("denied"));

        // At most five.
        for (int i = 0; i < ChatServer::MaxDevices - 1; ++i) {
            ChatClient extra;
            QVERIFY(connectTo(extra));
            QVERIFY(wait([&](auto done) { extra.addDevice("alice", alice.identity, Protocol::publicPart(spk), {}, done); }).ok);
        }
        ChatClient sixth;
        QVERIFY(connectTo(sixth));
        result = wait([&](auto done) { sixth.addDevice("alice", alice.identity, Protocol::publicPart(spk), {}, done); });
        QCOMPARE(result.error, QString("too-many-devices"));
    }

    void clientRefusesPlainRemote()
    {
        ChatClient client;
        QString error;
        QVERIFY(!client.open(QUrl("ws://example.com:8790"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!client.open(QUrl("http://127.0.0.1:8790")));
        QVERIFY(!client.open(QUrl("not a url")));
        QVERIFY(client.open(QUrl("ws://localhost:1")));
        client.close();
        // Requests while not connected fail at once.
        QCOMPARE(wait([&](auto done) { client.fetchKeys("bob", done); }).error, QString("disconnected"));
    }

private:
    bool connectTo(ChatClient &client)
    {
        if (!client.open(m_url))
            return false;
        return QTest::qWaitFor([&client] { return client.isConnected(); }, kWait);
    }

    ChatClient::Result signUp(Person &person)
    {
        return wait([&](auto done) {
            person.client.registerAccount(person.name, person.identity, Protocol::publicPart(person.signedPreKey),
                                          Protocol::publicParts(person.oneTime), done);
        });
    }

    bool join(Person &person)
    {
        if (!connectTo(person.client))
            return false;
        const ChatClient::Result result = signUp(person);
        if (!result.ok)
            qWarning() << "sign-up failed:" << result.error << result.message;
        return result.ok && result.data.value("device").toInt() == 1;
    }

    static bool sendText(Person &from, const QString &to, const QString &text)
    {
        const QByteArray message = from.session->encrypt(Protocol::encodeContent(Protocol::Content::text(text)));
        if (message.isEmpty())
            return false;
        const ChatClient::Result result = wait([&](auto done) { from.client.send(to, {{1, message}}, done); });
        if (!result.ok)
            qWarning() << "send failed:" << result.error << result.message;
        return result.ok;
    }

    QTemporaryDir m_dir;
    int m_count = 0;
    qint64 m_now = 1000000; // the server's limiter clock
    std::unique_ptr<ServerStore> m_store;
    std::unique_ptr<ChatServer> m_server;
    QUrl m_url;
};

CHAT_TEST_MAIN(TestServer, QCoreApplication)
#include "tst_server.moc"
