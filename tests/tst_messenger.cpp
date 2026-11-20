#include "ChatServer.h"
#include "Messenger.h"
#include "ServerStore.h"
#include "TestSupport.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {

constexpr int kWait = 5000;

// One person's computer: their chat store, keys and messenger. Can be
// closed and opened again, as the app is.
struct Device
{
    explicit Device(const QString &file)
        : file(file)
    {
        open();
    }
    void open()
    {
        store = std::make_unique<ChatStore>(file);
        keys = std::make_unique<KeyStore>(store.get());
        messenger = std::make_unique<Messenger>(store.get(), keys.get());
        messenger->setReconnectDelay(50);
    }
    void close()
    {
        messenger.reset();
        keys.reset();
        store.reset();
    }
    // The last message of a conversation, or nothing.
    QString last(qint64 conversation) const
    {
        const auto list = store->messages(conversation);
        return list.isEmpty() ? QString() : list.last().body;
    }

    QString file;
    std::unique_ptr<ChatStore> store;
    std::unique_ptr<KeyStore> keys;
    std::unique_ptr<Messenger> messenger;
};

} // namespace

class TestMessenger : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        m_serverStore = std::make_unique<ServerStore>(m_dir.filePath(QStringLiteral("server-%1.sqlite").arg(++m_count)));
        m_server = std::make_unique<ChatServer>(m_serverStore.get());
        QVERIFY(m_server->listen());
        m_url = QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(m_server->port()));
    }

    void cleanup()
    {
        m_server.reset();
        m_serverStore.reset();
    }

    void signUpAndChat()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QCOMPARE(alice.messenger->state(), Messenger::State::NoAccount);
        QVERIFY(signUp(alice, "Alice"));
        QCOMPARE(alice.messenger->username(), QString("alice")); // normalized
        QCOMPARE(alice.messenger->state(), Messenger::State::Online);
        QVERIFY(signUp(bob, "bob"));

        // The name is taken: nothing of it is kept.
        Device other(file("other"));
        QString error;
        QVERIFY(!signUp(other, "alice", &error));
        QVERIFY(error.contains("taken"));
        QVERIFY(!other.keys->account());
        QCOMPARE(other.messenger->state(), Messenger::State::NoAccount);
        QVERIFY(!signUp(other, "no way!", &error));

        // Alice writes to Bob.
        const qint64 toBob = startConversation(alice, "bob");
        QVERIFY(toBob > 0);
        QCOMPARE(alice.store->conversation(toBob).title, QString("bob"));
        QCOMPARE(startConversation(alice, "bob"), toBob); // the same one
        const qint64 hello = alice.messenger->send(toBob, "Hello Bob");
        QVERIFY(hello > 0);
        QCOMPARE(alice.store->message(hello).status, QString("sending"));

        // Bob has it, in a conversation with Alice; Alice sees ✓✓.
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("Hello Bob"), kWait);
        QCOMPARE(bob.store->conversation(toAlice).unread, 1);
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(hello).status, QString("delivered"), kWait);
        QCOMPARE(m_serverStore->queuedCount("bob", 1), 0); // acknowledged

        // Bob reads it: Alice sees it read.
        bob.messenger->markRead(toAlice);
        QCOMPARE(bob.store->conversation(toAlice).unread, 0);
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(hello).status, QString("read"), kWait);

        // And answers.
        bob.messenger->send(toAlice, QString::fromUtf8("Hi Alice 👋"));
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(toBob), QString::fromUtf8("Hi Alice 👋"), kWait);

        // The same safety number on both sides.
        const QString number = alice.messenger->safetyNumber("bob");
        QCOMPARE(number.size(), 60);
        QCOMPARE(bob.messenger->safetyNumber("alice"), number);
        QVERIFY(!alice.messenger->safetyNumberHasChanged("bob"));
        alice.messenger->setVerified("bob", true);
        QVERIFY(alice.messenger->isVerified("bob"));

        // Note to Self stays here.
        const qint64 note = alice.messenger->send(alice.store->noteToSelf(), "milk");
        QCOMPARE(alice.store->message(note).status, QString("sent"));
        QCOMPARE(startConversation(alice, "alice"), alice.store->noteToSelf());

        // Nobody.
        QCOMPARE(startConversation(alice, "nobody", &error), 0);
        QVERIFY(error.contains("nobody"));
    }

    void awayAndBack()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        const qint64 toBob = startConversation(alice, "bob");

        // Bob's app is closed: the server keeps the message.
        bob.close();
        const qint64 first = alice.messenger->send(toBob, "while you were away");
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(first).status, QString("sent"), kWait);
        QCOMPARE(m_serverStore->queuedCount("bob", 1), 1);

        // Alice is offline: hers waits in the outbox.
        alice.messenger->stop();
        QCOMPARE(alice.messenger->state(), Messenger::State::Offline);
        const qint64 second = alice.messenger->send(toBob, "sent later");
        QCOMPARE(alice.store->message(second).status, QString("sending"));
        QCOMPARE(alice.store->outbox().size(), 1);

        // Bob opens the app again: his account, keys and sessions are still there.
        bob.open();
        QVERIFY(bob.messenger->hasAccount());
        bob.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->state(), Messenger::State::Online, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bob.store->directConversation("alice")), QString("while you were away"),
                                  kWait);

        // Alice is back: the outbox goes.
        alice.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("sent later"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(second).status, QString("delivered"), kWait);
        QVERIFY(alice.store->outbox().isEmpty());
        QCOMPARE(bob.store->messages(toAlice).size(), 2);

        // Alice's app restarts too, and the conversation goes on.
        alice.close();
        alice.open();
        alice.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(alice.messenger->state(), Messenger::State::Online, kWait);
        alice.messenger->send(toBob, "still here");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("still here"), kWait);
        bob.messenger->send(toAlice, "me too");
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(toBob), QString("me too"), kWait);
    }

    void reconnectsByItself()
    {
        Device alice(file("alice"));
        QVERIFY(signUp(alice, "alice"));
        QSignalSpy states(alice.messenger.get(), &Messenger::stateChanged);
        // The server goes away and comes back on the same port.
        const quint16 port = m_server->port();
        m_server.reset();
        QTRY_COMPARE_WITH_TIMEOUT(alice.messenger->state(), Messenger::State::Offline, kWait);
        m_server = std::make_unique<ChatServer>(m_serverStore.get());
        QVERIFY(m_server->listen(QHostAddress::LocalHost, port));
        QTRY_COMPARE_WITH_TIMEOUT(alice.messenger->state(), Messenger::State::Online, kWait);
    }

    void preKeysAreToppedUp()
    {
        Device bob(file("bob"));
        QVERIFY(signUp(bob, "bob"));
        QCOMPARE(m_serverStore->oneTimePreKeyCount("bob", 1), Messenger::PreKeyBatch);
        // Strangers took most of them.
        for (int i = 0; i < Messenger::PreKeyBatch - 5; ++i)
            QVERIFY(m_serverStore->takeBundle("bob", 1));
        QCOMPARE(m_serverStore->oneTimePreKeyCount("bob", 1), 5);
        bob.messenger->stop();
        bob.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(m_serverStore->oneTimePreKeyCount("bob", 1), 5 + Messenger::PreKeyBatch, kWait);
        QCOMPARE(bob.keys->oneTimePreKeyCount(), 2 * Messenger::PreKeyBatch);
    }

    void waitsWhenRateLimited()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        const qint64 toBob = startConversation(alice, "bob");
        ChatServer::Limits limits = m_server->limits();
        limits.sendsPerAccount = {2, 4}; // a burst of 2, then 4 a second
        m_server->setLimits(limits);

        QList<qint64> sent;
        for (int i = 0; i < 6; ++i)
            sent << alice.messenger->send(toBob, QStringLiteral("message %1").arg(i));
        // All arrive in the end, none marked as not sent.
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->messages(bob.store->directConversation("alice")).size(), 6, 3 * kWait);
        for (qint64 id : sent)
            QVERIFY(alice.store->message(id).status != QLatin1String("failed"));
        QTRY_VERIFY_WITH_TIMEOUT(alice.store->outbox().isEmpty(), kWait);
    }

    void keyStoreTrust()
    {
        Device alice(file("alice"));
        const QByteArray first = Crypto::IdentityKeyPair::generate().publicKey();
        const QByteArray second = Crypto::IdentityKeyPair::generate().publicKey();
        QCOMPARE(alice.keys->checkIdentity("bob", first), KeyStore::Trust::New);
        QCOMPARE(alice.keys->checkIdentity("bob", first), KeyStore::Trust::Same);
        QVERIFY(!alice.keys->identityChanged("bob"));
        alice.keys->setVerified("bob", true);
        QCOMPARE(alice.keys->checkIdentity("bob", second), KeyStore::Trust::Changed);
        QVERIFY(alice.keys->identityChanged("bob"));
        QVERIFY(!alice.keys->isVerified("bob")); // a new key is not verified
        QCOMPARE(alice.keys->identityKey("bob"), second);
        alice.keys->setVerified("bob", true);
        QVERIFY(!alice.keys->identityChanged("bob"));
    }

private:
    QString file(const QString &name) { return m_dir.filePath(QStringLiteral("%1-%2.sqlite").arg(name).arg(++m_count)); }

    bool signUp(Device &device, const QString &username, QString *error = nullptr)
    {
        std::optional<bool> result;
        device.messenger->signUp(m_url, username, [&](bool ok, const QString &why) {
            result = ok;
            if (error)
                *error = why;
        });
        if (!QTest::qWaitFor([&] { return result.has_value(); }, kWait))
            return false;
        return *result;
    }

    qint64 startConversation(Device &device, const QString &username, QString *error = nullptr)
    {
        std::optional<qint64> result;
        device.messenger->startConversation(username, [&](qint64 conversation, const QString &why) {
            result = conversation;
            if (error)
                *error = why;
        });
        if (!QTest::qWaitFor([&] { return result.has_value(); }, kWait))
            return -1;
        return *result;
    }

    QTemporaryDir m_dir;
    int m_count = 0;
    std::unique_ptr<ServerStore> m_serverStore;
    std::unique_ptr<ChatServer> m_server;
    QUrl m_url;
};

CHAT_TEST_MAIN(TestMessenger, QCoreApplication)
#include "tst_messenger.moc"
