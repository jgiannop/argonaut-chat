#include "Attachment.h"
#include "ChatServer.h"
#include "Messenger.h"
#include "ServerStore.h"
#include "TestSupport.h"
#include "Voice.h"

#include <QCoreApplication>
#include <cmath>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QSqlQuery>
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
        m_serverStore = std::make_unique<ServerStore>(m_dir.filePath(QStringLiteral("server-%1/server.sqlite").arg(++m_count)));
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

        // Bob has it, as a message request from Alice.
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("Hello Bob"), kWait);
        QVERIFY(bob.store->conversation(toAlice).request);
        QTRY_COMPARE_WITH_TIMEOUT(m_serverStore->queuedCount("bob", 1), 0, kWait); // acknowledged
        // Until he accepts, Alice learns nothing: ✓, not ✓✓, even when he looks.
        bob.messenger->markRead(toAlice);
        QTest::qWait(300);
        QCOMPARE(alice.store->message(hello).status, QString("sent"));
        QCOMPARE(bob.store->conversation(toAlice).unread, 1);

        // He accepts: Alice sees it read.
        bob.messenger->acceptRequest(toAlice);
        QVERIFY(!bob.store->conversation(toAlice).request);
        QCOMPARE(bob.store->conversation(toAlice).unread, 0);
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(hello).status, QString("read"), kWait);
        // From now on, delivered receipts as usual.
        const qint64 again = alice.messenger->send(toBob, "again");
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(again).status, QString("delivered"), kWait);

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
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("while you were away"), kWait);
        bob.messenger->acceptRequest(toAlice);

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

    void requestsAndBlocking()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        const qint64 toBob = startConversation(alice, "bob");
        QVERIFY(!alice.store->conversation(toBob).request); // mine: accepted from the start

        // Deleted: the next message is a new request.
        alice.messenger->send(toBob, "one");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        bob.messenger->deleteConversation(bob.store->directConversation("alice"));
        QCOMPARE(bob.store->directConversation("alice"), 0);
        alice.messenger->send(toBob, "two");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        qint64 request = bob.store->directConversation("alice");
        QVERIFY(bob.store->conversation(request).request);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(request), QString("two"), kWait);
        QCOMPARE(bob.store->messages(request).size(), 1);

        // Blocked: the request goes, and her messages are dropped without a word.
        bob.messenger->block("alice");
        QVERIFY(bob.messenger->isBlocked("alice"));
        QCOMPARE(bob.store->directConversation("alice"), 0);
        const qint64 three = alice.messenger->send(toBob, "three");
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(three).status, QString("sent"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(m_serverStore->queuedCount("bob", 1), 0, kWait);
        QTest::qWait(200);
        QCOMPARE(bob.store->directConversation("alice"), 0);
        QCOMPARE(alice.store->message(three).status, QString("sent")); // no ✓✓: she learns nothing
        QString error;
        QCOMPARE(startConversation(bob, "alice", &error), 0);
        QVERIFY(error.contains("blocked"));
        QCOMPARE(bob.store->blocked(), QStringList{"alice"});

        // Unblocked: she can reach him again.
        bob.messenger->unblock("alice");
        alice.messenger->send(toBob, "four");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        request = bob.store->directConversation("alice");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(request), QString("four"), kWait);

        // Answering a request accepts it.
        bob.messenger->send(request, "hello you");
        QVERIFY(!bob.store->conversation(request).request);
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(toBob), QString("hello you"), kWait);
    }

    void signedPreKeyRotation()
    {
        const qint64 day = 24LL * 3600 * 1000;
        qint64 clock = QDateTime::currentMSecsSinceEpoch();
        Device bob(file("bob"));
        bob.messenger->setClock([&clock] { return clock; });
        QVERIFY(signUp(bob, "bob"));
        auto serverKey = [this] { return m_serverStore->takeBundle("bob", 1, false)->signedPreKeyId; };
        const quint32 first = serverKey();
        auto restart = [&] {
            bob.messenger->stop();
            bob.messenger->start();
            return QTest::qWaitFor([&] { return bob.messenger->state() == Messenger::State::Online; }, kWait);
        };

        // Within the week: the same key.
        clock += 6 * day;
        QVERIFY(restart());
        QTest::qWait(200);
        QCOMPARE(serverKey(), first);
        QCOMPARE(bob.keys->signedPreKeys().size(), 1);

        // Alice starts a conversation with the old key just before it changes.
        Device alice(file("alice"));
        QVERIFY(signUp(alice, "alice"));
        const qint64 toBob = startConversation(alice, "bob");
        QVERIFY(toBob > 0);

        // A week on: a new key on the server; the old one still kept here.
        clock += 2 * day;
        QVERIFY(restart());
        QTRY_VERIFY_WITH_TIMEOUT(serverKey() != first, kWait);
        const quint32 second = serverKey();
        QTRY_VERIFY_WITH_TIMEOUT(bob.keys->signedPreKeys().last().uploaded, kWait);
        QCOMPARE(bob.keys->signedPreKeys().size(), 2);
        QCOMPARE(bob.keys->latestSignedPreKey()->id, second);
        // Alice's message, started with the old key, still opens.
        alice.messenger->send(toBob, "made with the old key");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bob.store->directConversation("alice")), QString("made with the old key"),
                                  kWait);

        // Thirty days after it was replaced, the first goes; a third comes.
        clock += 31 * day;
        QVERIFY(restart());
        QTRY_VERIFY_WITH_TIMEOUT(serverKey() != second, kWait);
        QTRY_VERIFY_WITH_TIMEOUT(bob.keys->signedPreKeys().last().uploaded, kWait);
        QVERIFY(!bob.keys->signedPreKey(first));
        QVERIFY(bob.keys->signedPreKey(second)); // replaced just now: kept
        QCOMPARE(bob.keys->signedPreKeys().size(), 2);

        // One made but never uploaded (the connection went): sent on the next sign-in.
        const X3dh::SignedPreKey pending = X3dh::makeSignedPreKey(bob.keys->account()->identity, 9999);
        QVERIFY(bob.keys->addSignedPreKey(pending, clock + 1, false));
        QVERIFY(restart());
        QTRY_COMPARE_WITH_TIMEOUT(serverKey(), 9999u, kWait);
        QTRY_VERIFY_WITH_TIMEOUT(bob.keys->signedPreKeys().last().uploaded, kWait);
    }

    // PQXDH is required: someone still without KEM prekeys (a version before)
    // can't be written to until they update; their app makes them at once.
    void upgradingToPqxdh()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        // Bob as on 0.2.1: no KEM prekeys, here or on the server.
        bob.messenger->stop();
        QSqlQuery q(bob.store->database());
        QVERIFY(q.exec("UPDATE signed_prekeys SET kem_public = NULL, kem_secret = NULL, kem_signature = NULL"));
        QVERIFY(!bob.keys->latestSignedPreKey()->hasKem());
        Protocol::PublicSignedPreKey old = Protocol::publicPart(*bob.keys->latestSignedPreKey());
        QVERIFY(old.kemKey.isEmpty());
        QVERIFY(m_serverStore->setSignedPreKey("bob", 1, old));
        while (m_serverStore->takeBundle("bob", 1)->oneTimePreKeyId) {
        } // his one-time ones used up

        // Alice is told why, and nothing is started (no X3DH instead).
        QString error;
        QCOMPARE(startConversation(alice, "bob", &error), 0);
        QVERIFY2(error.contains("older version"), qPrintable(error));
        QVERIFY(!alice.keys->session("bob", 1));

        // Bob's app updated: on sign-in a new signed prekey, with its KEM prekey.
        bob.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->state(), Messenger::State::Online, kWait);
        QTRY_VERIFY_WITH_TIMEOUT(!m_serverStore->takeBundle("bob", 1, false)->signedKemKey.isEmpty(), kWait);
        QTRY_VERIFY_WITH_TIMEOUT(bob.keys->signedPreKeys().last().uploaded, kWait);
        QVERIFY(bob.keys->latestSignedPreKey()->hasKem());
        // Now it starts, with the last-resort KEM prekey (no one-time ones left), and works.
        const qint64 toBob = startConversation(alice, "bob", &error);
        QVERIFY2(toBob > 0, qPrintable(error));
        alice.messenger->send(toBob, "post-quantum hello");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("post-quantum hello"), kWait);
        bob.messenger->acceptRequest(toAlice);
        bob.messenger->send(toAlice, "hello back");
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(toBob), QString("hello back"), kWait);
    }

    void files()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        const qint64 toBob = startConversation(alice, "bob");
        const QByteArray photo = Crypto::randomBytes(1200 * 1024);
        const QString path = writeFile("holiday photo.jpg", photo);

        // Sent with a caption: up, then the message with its key.
        QString error;
        const qint64 sent = alice.messenger->sendFile(toBob, path, "look!", &error);
        QVERIFY2(sent > 0, qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(sent).status, QString("sent"), kWait);
        const auto mine = alice.store->attachments(sent);
        QCOMPARE(mine.size(), 1);
        QCOMPARE(mine.first().state, QString("sent"));
        QCOMPARE(mine.first().name, QString("holiday photo.jpg"));
        QCOMPARE(mine.first().size, photo.size());

        // Bob: a request, so nothing fetched yet.
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 fromAlice = bob.store->directConversation("alice");
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->messages(fromAlice).size(), 1, kWait);
        const ChatStore::Message got = bob.store->messages(fromAlice).first();
        QCOMPARE(got.body, QString("look!"));
        auto theirs = bob.store->attachments(got.id);
        QCOMPARE(theirs.size(), 1);
        QCOMPARE(theirs.first().state, QString("download"));
        QVERIFY(bob.store->pendingDownloads().isEmpty());
        QVERIFY(!bob.messenger->attachmentData(theirs.first().id));

        // Accepted: fetched, checked, and the same bytes.
        bob.messenger->acceptRequest(fromAlice);
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->attachment(theirs.first().id).state, QString("ready"), kWait);
        const qint64 attachment = theirs.first().id;
        QCOMPARE(bob.messenger->attachmentData(attachment), std::optional<QByteArray>(photo));
        QCOMPARE(bob.store->attachment(attachment).type, QString("image/jpeg"));
        const QString saved = m_dir.filePath("saved.jpg");
        QVERIFY(bob.messenger->saveAttachment(attachment, saved));
        QFile savedFile(saved);
        QVERIFY(savedFile.open(QIODevice::ReadOnly));
        QCOMPARE(savedFile.readAll(), photo);
        // Nowhere in the clear: not on the server, not in Bob's folder.
        const QByteArray sample = photo.mid(5000, 64);
        for (const QString &folder : {m_serverStore->attachmentFolder(), bob.store->attachmentFolder(),
                                      alice.store->attachmentFolder()}) {
            for (const QString &name : QDir(folder).entryList(QDir::Files)) {
                QFile blob(folder + "/" + name);
                QVERIFY(blob.open(QIODevice::ReadOnly));
                QVERIFY2(!blob.readAll().contains(sample), qPrintable(folder + "/" + name));
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(sent).status, QString("read"), kWait);

        // Note to Self: here only.
        const int onServer = QDir(m_serverStore->attachmentFolder()).entryList(QDir::Files).size();
        const qint64 note = alice.messenger->sendFile(alice.store->noteToSelf(), path, QString());
        QCOMPARE(alice.store->message(note).status, QString("sent"));
        QCOMPARE(alice.store->attachments(note).first().state, QString("ready"));
        QCOMPARE(alice.messenger->attachmentData(alice.store->attachments(note).first().id),
                 std::optional<QByteArray>(photo));
        QCOMPARE(QDir(m_serverStore->attachmentFolder()).entryList(QDir::Files).size(), onServer);

        // Too large.
        QFile big(m_dir.filePath("big.bin"));
        QVERIFY(big.open(QIODevice::WriteOnly));
        QVERIFY(big.resize(Attachment::MaxFileBytes + 1));
        big.close();
        QCOMPARE(alice.messenger->sendFile(toBob, big.fileName(), QString(), &error), 0);
        QVERIFY(error.contains("25 MB"));

        // Offline: it waits in the outbox, then goes.
        alice.messenger->stop();
        const qint64 later = alice.messenger->sendFile(toBob, writeFile("notes.txt", "some notes"), QString());
        QCOMPARE(alice.store->attachments(later).first().state, QString("upload"));
        QCOMPARE(alice.store->message(later).status, QString("sending"));
        alice.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(later).status, QString("delivered"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->messages(fromAlice).size(), 2, kWait);
        const qint64 notes = bob.store->attachments(bob.store->messages(fromAlice).last().id).first().id;
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->attachment(notes).state, QString("ready"), kWait);
        QCOMPARE(bob.messenger->attachmentData(notes), std::optional<QByteArray>("some notes"));
        QCOMPARE(bob.store->attachment(notes).type, QString("text/plain"));

        // Changed on the server while Bob was away: caught, not kept.
        bob.close();
        alice.messenger->sendFile(toBob, writeFile("contract.pdf", Crypto::randomBytes(3000)), QString());
        QTRY_VERIFY_WITH_TIMEOUT(QDir(m_serverStore->attachmentFolder()).entryList(QDir::Files).size() == onServer + 2,
                                 kWait);
        QTest::qWait(300); // the upload's last chunk is in
        QString newest;
        for (const QFileInfo &info : QDir(m_serverStore->attachmentFolder()).entryInfoList(QDir::Files, QDir::Time))
            if (newest.isEmpty())
                newest = info.filePath();
        QFile stored(newest);
        QVERIFY(stored.open(QIODevice::ReadWrite));
        QByteArray bytes = stored.readAll();
        bytes[100] = char(bytes.at(100) ^ 1);
        QVERIFY(stored.seek(0));
        stored.write(bytes);
        stored.close();
        bob.open();
        bob.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->messages(fromAlice).size(), 3, kWait);
        const qint64 contract = bob.store->attachments(bob.store->messages(fromAlice).last().id).first().id;
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->attachment(contract).state, QString("failed"), kWait);
        QVERIFY(!bob.messenger->attachmentData(contract));

        // Deleting the conversation deletes its files.
        QVERIFY(!QDir(bob.store->attachmentFolder()).entryList(QDir::Files).isEmpty());
        bob.messenger->deleteConversation(fromAlice);
        QVERIFY(QDir(bob.store->attachmentFolder()).entryList(QDir::Files).isEmpty());
    }

    void profiles()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        // A picture: what matters here is that it starts as a PNG does.
        const QByteArray picture = QByteArray("\x89PNG\r\n\x1a\n", 8) + Crypto::randomBytes(3000);
        QString error;
        QVERIFY(!alice.messenger->setMyProfile("Alice", "not a picture", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!alice.messenger->setMyProfile("Alice", QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(300 * 1024, 'x')));
        // Direction marks and controls go: no "evil" name that reads backwards.
        QVERIFY(alice.messenger->setMyProfile(QStringLiteral("Alice") + QChar(0x202E) + QStringLiteral(" \n Smith  "), picture));
        QCOMPARE(alice.messenger->myProfile().name, QString("Alice Smith"));

        // She writes to Bob: her name reaches him even as a request; her picture waits.
        const qint64 toBob = startConversation(alice, "bob");
        alice.messenger->send(toBob, "hi");
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->displayName("alice"), QString("Alice Smith"), kWait);
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 fromAlice = bob.store->directConversation("alice");
        QTest::qWait(300);
        QVERIFY(bob.messenger->avatar("alice").isEmpty());
        QCOMPARE(bob.messenger->displayName("carol"), QString("carol")); // nobody's name: the username
        // Accepted: the picture comes.
        bob.messenger->acceptRequest(fromAlice);
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->avatar("alice"), picture, kWait);
        // A profile is not a message.
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->messages(fromAlice).size(), 1, kWait);

        // Bob's, without a picture, to Alice.
        QVERIFY(bob.messenger->setMyProfile("Bob B.", QByteArray()));
        QTRY_COMPARE_WITH_TIMEOUT(alice.messenger->displayName("bob"), QString("Bob B."), kWait);
        QVERIFY(alice.messenger->avatar("bob").isEmpty());

        // Alice renames herself: the same picture, not uploaded again.
        const int uploads = QDir(m_serverStore->attachmentFolder()).entryList(QDir::Files).size();
        QVERIFY(alice.messenger->setMyProfile("Alice S.", picture));
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->displayName("alice"), QString("Alice S."), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->avatar("alice"), picture, kWait);
        QCOMPARE(QDir(m_serverStore->attachmentFolder()).entryList(QDir::Files).size(), uploads + 1); // a new version: once
        // ... and takes her picture away.
        QVERIFY(alice.messenger->setMyProfile("Alice S.", QByteArray()));
        QTRY_VERIFY_WITH_TIMEOUT(bob.messenger->avatar("alice").isEmpty(), kWait);

        // Changes in quick succession (each while the last is on its way): the last one wins.
        const QByteArray other = QByteArray("\x89PNG\r\n\x1a\n", 8) + Crypto::randomBytes(2000);
        QVERIFY(alice.messenger->setMyProfile("Alice 1", picture));
        QVERIFY(alice.messenger->setMyProfile("Alice 2", other));
        QVERIFY(alice.messenger->setMyProfile("Alice S.", QByteArray()));
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->displayName("alice"), QString("Alice S."), kWait);
        QTest::qWait(500); // anything late would land now
        QVERIFY(bob.messenger->avatar("alice").isEmpty());
        QVERIFY(alice.messenger->setMyProfile("Alice S.", other));
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->avatar("alice"), other, kWait);
        QVERIFY(alice.messenger->setMyProfile("Alice S.", QByteArray()));

        // Kept over a restart.
        bob.close();
        bob.open();
        QCOMPARE(bob.messenger->displayName("alice"), QString("Alice S."));
        QCOMPARE(bob.store->messages(fromAlice).size(), 1);
    }

    void groups()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        Device carol(file("carol"));
        Device dave(file("dave"));
        for (auto &[device, name] : QList<QPair<Device *, QString>>{{&alice, "alice"}, {&bob, "bob"}, {&carol, "carol"}, {&dave, "dave"}})
            QVERIFY(signUp(*device, name));
        // Bob knows Alice; Carol does not.
        const qint64 aliceToBob = startConversation(alice, "bob");
        alice.messenger->send(aliceToBob, "hi Bob");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        bob.messenger->acceptRequest(bob.store->directConversation("alice"));

        auto create = [&](Device &who, const QString &name, const QStringList &members, QString *error = nullptr) {
            std::optional<qint64> result;
            who.messenger->createGroup(name, members, [&](qint64 conversation, const QString &why) {
                result = conversation;
                if (error)
                    *error = why;
            });
            return QTest::qWaitFor([&] { return result.has_value(); }, kWait) ? *result : qint64(-1);
        };
        QString error;
        QCOMPARE(create(alice, "x", {"nobody"}, &error), 0);
        QVERIFY(error.contains("nobody"));
        QStringList crowd;
        for (int i = 0; i < Protocol::MaxGroupMembers; ++i)
            crowd << QStringLiteral("person%1").arg(i);
        QCOMPARE(create(alice, "x", crowd, &error), 0);
        QVERIFY(error.contains("50"));

        const qint64 trip = create(alice, "Trip", {"bob", "Carol"});
        QVERIFY(trip > 0);
        const QString gid = alice.store->group(trip).id;
        QCOMPARE(alice.store->group(trip).members, (QStringList{"alice", "bob", "carol"}));
        QCOMPARE(alice.store->group(trip).admin, QString("alice"));
        // Bob (knows Alice): the group, accepted. Carol: a request.
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->groupConversation(gid) > 0, kWait);
        QTRY_VERIFY_WITH_TIMEOUT(carol.store->groupConversation(gid) > 0, kWait);
        const qint64 bobTrip = bob.store->groupConversation(gid);
        const qint64 carolTrip = carol.store->groupConversation(gid);
        QVERIFY(!bob.store->conversation(bobTrip).request);
        QVERIFY(carol.store->conversation(carolTrip).request);
        QCOMPARE(bob.store->conversation(bobTrip).title, QString("Trip"));

        // A message to all; each has it, from Alice.
        const qint64 hello = alice.messenger->send(trip, "hello all");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobTrip), QString("hello all"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(carol.last(carolTrip), QString("hello all"), kWait);
        QCOMPARE(bob.store->messages(bobTrip).last().author, QString("alice"));
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(hello).status, QString("delivered"), kWait); // Bob's ✓✓
        // Bob answers; Carol has it too, from someone she does not know: it is the group's.
        bob.messenger->send(bobTrip, "count me in");
        QTRY_COMPARE_WITH_TIMEOUT(carol.last(carolTrip), QString("count me in"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(trip), QString("count me in"), kWait);
        QCOMPARE(carol.store->directConversation("bob"), 0);
        // Carol accepts: read receipts go to each author; she writes.
        carol.messenger->acceptRequest(carolTrip);
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(hello).status, QString("read"), kWait);
        carol.messenger->send(carolTrip, "me too");
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(trip), QString("me too"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobTrip), QString("me too"), kWait);

        // Only the admin changes it.
        QCOMPARE([&] {
            std::optional<qint64> result;
            bob.messenger->changeGroup(bobTrip, "Mine now", {"bob"}, [&](qint64 c, const QString &) { result = c; });
            return result.value_or(-1);
        }(), 0);
        // Forgeries: Dave, not a member, sends into the group and "renames" it, by
        // hand with his own keys (as a changed program could).
        const qint64 daveToBob = startConversation(dave, "bob");
        dave.messenger->send(daveToBob, "hello Bob");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("dave") > 0, kWait);
        dave.messenger->stop();
        std::optional<Session> session = dave.keys->session("bob", 1);
        QVERIFY(session);
        Protocol::Content sneaky = Protocol::Content::text("I am in your group now");
        sneaky.group = gid;
        const Protocol::Content rename = Protocol::Content::groupUpdate(gid, "Pwned", "dave", {"dave", "bob"}, 99);
        QHash<quint32, QByteArray> wires{{1, session->encrypt(Protocol::encodeContent(sneaky))}};
        QHash<quint32, QByteArray> wires2{{1, session->encrypt(Protocol::encodeContent(rename))}};
        dave.keys->saveSession("bob", 1, *session);
        ChatClient raw;
        QVERIFY(raw.open(m_url));
        QTRY_VERIFY_WITH_TIMEOUT(raw.isConnected(), kWait);
        std::optional<bool> sent;
        raw.login("dave", 1, dave.keys->account()->identity, [&](const ChatClient::Result &r) {
            const bool loggedIn = r.ok; // copied: `r` is gone by the time the sends answer
            raw.send("bob", wires, [&, loggedIn](const ChatClient::Result &) {
                raw.send("bob", wires2, [&, loggedIn](const ChatClient::Result &s) { sent = loggedIn && s.ok; });
            });
        });
        QTRY_VERIFY_WITH_TIMEOUT(sent.has_value(), kWait);
        QVERIFY(*sent);
        QTRY_COMPARE_WITH_TIMEOUT(m_serverStore->queuedCount("bob", 1), 0, kWait); // Bob took them
        QTest::qWait(200);
        QCOMPARE(bob.last(bobTrip), QString("me too"));
        QCOMPARE(bob.store->group(bobTrip).name, QString("Trip"));
        QCOMPARE(bob.store->group(bobTrip).members, (QStringList{"alice", "bob", "carol"}));
        // Not anywhere else either.
        QCOMPARE(bob.last(bob.store->directConversation("dave")), QString("hello Bob"));
        for (const ChatStore::Conversation &c : bob.store->conversations())
            for (const ChatStore::Message &m : bob.store->messages(c.id))
                QVERIFY(m.body != QLatin1String("I am in your group now"));
        raw.close();
        dave.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(dave.messenger->state(), Messenger::State::Online, kWait);

        // Alice takes Carol out and brings Dave in.
        std::optional<qint64> changed;
        alice.messenger->changeGroup(trip, "Trip 2", {"bob", "dave"}, [&](qint64 c, const QString &) { changed = c; });
        QTRY_VERIFY_WITH_TIMEOUT(changed.has_value(), kWait);
        QCOMPARE(*changed, trip);
        QTRY_VERIFY_WITH_TIMEOUT(carol.store->conversation(carolTrip).left, kWait);
        QCOMPARE(carol.messenger->send(carolTrip, "still here?"), 0);
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->group(bobTrip).name, QString("Trip 2"), kWait);
        QTRY_VERIFY_WITH_TIMEOUT(dave.store->groupConversation(gid) > 0, kWait);
        const qint64 daveTrip = dave.store->groupConversation(gid);
        QVERIFY(dave.store->conversation(daveTrip).request); // he does not know Alice
        alice.messenger->send(trip, "welcome Dave");
        QTRY_COMPARE_WITH_TIMEOUT(dave.last(daveTrip), QString("welcome Dave"), kWait);
        QTest::qWait(200);
        QVERIFY(carol.last(carolTrip) != QString("welcome Dave"));

        // Bob leaves; the admin tells the rest.
        bob.messenger->leaveGroup(bobTrip);
        QVERIFY(bob.store->conversation(bobTrip).left);
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->group(trip).members, (QStringList{"alice", "dave"}), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(dave.store->group(daveTrip).members, (QStringList{"alice", "dave"}), kWait);
        alice.messenger->send(trip, "just us");
        QTRY_COMPARE_WITH_TIMEOUT(dave.last(daveTrip), QString("just us"), kWait);
        QTest::qWait(200);
        QVERIFY(bob.last(bobTrip) != QString("just us"));
    }

    void linkedDevices()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        const QByteArray picture = QByteArray("\x89PNG\r\n\x1a\n", 8) + Crypto::randomBytes(1500);
        QVERIFY(alice.messenger->setMyProfile("Alice", picture));
        const qint64 aliceToBob = startConversation(alice, "bob");
        alice.messenger->send(aliceToBob, "hello Bob");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 bobToAlice = bob.store->directConversation("alice");
        bob.messenger->acceptRequest(bobToAlice);

        std::optional<qint64> earlier;
        alice.messenger->createGroup("Before", {"bob"}, [&](qint64 c, const QString &) { earlier = c; });
        QTRY_VERIFY_WITH_TIMEOUT(earlier.has_value() && *earlier > 0, kWait);
        const QString earlierId = alice.store->group(*earlier).id;
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->groupConversation(earlierId) > 0, kWait);
        alice.messenger->block("mallory");

        // Alice's second computer: a code, typed on the first, and it joins.
        Device laptop(file("laptop"));
        std::optional<QString> code;
        std::optional<bool> linked;
        QString error;
        laptop.messenger->linkThisDevice(m_url, [&](const QString &shown) { code = shown; },
                                         [&](bool ok, const QString &why) { linked = ok; error = why; });
        QTRY_VERIFY_WITH_TIMEOUT(code.has_value(), kWait);
        QCOMPARE(code->size(), 64);
        std::optional<bool> sent;
        alice.messenger->linkDevice("not a code", [&](bool ok, const QString &) { sent = ok; });
        QCOMPARE(sent, std::optional<bool>(false));
        sent.reset();
        alice.messenger->linkDevice(*code, [&](bool ok, const QString &why) { sent = ok; error = why; });
        QTRY_VERIFY_WITH_TIMEOUT(sent.has_value(), kWait);
        QVERIFY2(*sent, qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(linked.has_value(), kWait);
        QVERIFY2(*linked, qPrintable(error));
        QCOMPARE(laptop.messenger->username(), QString("alice"));
        QCOMPARE(laptop.messenger->device(), 2u);
        QCOMPARE(laptop.keys->account()->identity.publicKey(), alice.keys->account()->identity.publicKey());
        QCOMPARE(laptop.messenger->myProfile().name, QString("Alice"));
        QCOMPARE(laptop.messenger->myProfile().avatar, picture);
        QVERIFY(laptop.store->messages(laptop.store->noteToSelf()).isEmpty()); // no old history
        // Her contacts, groups and blocks, but none of the messages.
        QVERIFY(laptop.messenger->isBlocked("mallory"));
        QVERIFY(laptop.store->directConversation("bob") > 0);
        QVERIFY(laptop.store->messages(laptop.store->directConversation("bob")).isEmpty());
        QVERIFY(laptop.store->groupConversation(earlierId) > 0);
        QCOMPARE(laptop.store->group(laptop.store->groupConversation(earlierId)).members, (QStringList{"alice", "bob"}));
        bob.messenger->send(bob.store->groupConversation(earlierId), "in the old group");
        QTRY_COMPARE_WITH_TIMEOUT(laptop.last(laptop.store->groupConversation(earlierId)), QString("in the old group"), kWait);
        // The same code again: used.
        sent.reset();
        alice.messenger->linkDevice(*code, [&](bool ok, const QString &) { sent = ok; });
        QTRY_COMPARE_WITH_TIMEOUT(sent, std::optional<bool>(false), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.messenger->myDevices().size(), 2, kWait);

        // Bob writes: both of Alice's computers have it. Safety number: the same.
        bob.messenger->send(bobToAlice, "hi both");
        QTRY_VERIFY_WITH_TIMEOUT(laptop.store->directConversation("bob") > 0, kWait);
        const qint64 laptopToBob = laptop.store->directConversation("bob");
        QTRY_COMPARE_WITH_TIMEOUT(laptop.last(laptopToBob), QString("hi both"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(aliceToBob), QString("hi both"), kWait);
        QCOMPARE(laptop.messenger->safetyNumber("bob"), alice.messenger->safetyNumber("bob"));
        QVERIFY(!bob.messenger->safetyNumberHasChanged("alice"));

        // Bob was accepted on the first computer: on the laptop too, not a request.
        QVERIFY(!laptop.store->conversation(laptopToBob).request);
        // Read on the first computer: read on the laptop too.
        QCOMPARE(laptop.store->conversation(laptopToBob).unread, 1);
        alice.messenger->markRead(aliceToBob);
        QTRY_COMPARE_WITH_TIMEOUT(laptop.store->conversation(laptopToBob).unread, 0, kWait);
        // She answers from the laptop: Bob has it; her first computer shows it as hers.
        laptop.messenger->send(laptopToBob, "from my laptop");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobToAlice), QString("from my laptop"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(aliceToBob), QString("from my laptop"), kWait);
        QVERIFY(alice.store->messages(aliceToBob).last().outgoing());

        // A group made on the first computer is on the laptop; written there, seen everywhere.
        std::optional<qint64> group;
        alice.messenger->createGroup("Us", {"bob"}, [&](qint64 c, const QString &) { group = c; });
        QTRY_VERIFY_WITH_TIMEOUT(group.has_value() && *group > 0, kWait);
        const QString gid = alice.store->group(*group).id;
        QTRY_VERIFY_WITH_TIMEOUT(laptop.store->groupConversation(gid) > 0, kWait);
        const qint64 laptopGroup = laptop.store->groupConversation(gid);
        QVERIFY(!laptop.store->conversation(laptopGroup).request);
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->groupConversation(gid) > 0, kWait);
        laptop.messenger->send(laptopGroup, "group, from the laptop");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bob.store->groupConversation(gid)), QString("group, from the laptop"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(*group), QString("group, from the laptop"), kWait);
        // A new name on the laptop reaches the first computer (and Bob).
        QVERIFY(laptop.messenger->setMyProfile("Alice L.", picture));
        QTRY_COMPARE_WITH_TIMEOUT(alice.messenger->myProfile().name, QString("Alice L."), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->displayName("alice"), QString("Alice L."), kWait);

        // The first computer removes the laptop: it can no longer sign in.
        std::optional<bool> removed;
        alice.messenger->removeDevice(2, [&](bool ok, const QString &) { removed = ok; });
        QTRY_VERIFY_WITH_TIMEOUT(removed.has_value(), kWait);
        QVERIFY(*removed);
        QTRY_VERIFY_WITH_TIMEOUT(laptop.messenger->state() != Messenger::State::Online, kWait);
        QTest::qWait(500);
        QVERIFY(laptop.messenger->state() != Messenger::State::Online);
        alice.messenger->send(aliceToBob, "only one computer again");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobToAlice), QString("only one computer again"), kWait);
        QVERIFY(laptop.last(laptopToBob) != QString("only one computer again"));
    }

    void disappearing()
    {
        qint64 clock = QDateTime::currentMSecsSinceEpoch();
        const qint64 hour = 3600 * 1000;
        Device alice(file("alice"));
        Device bob(file("bob"));
        Device carol(file("carol"));
        for (Device *d : {&alice, &bob, &carol})
            d->messenger->setClock([&clock] { return clock; });
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        QVERIFY(signUp(carol, "carol"));
        const qint64 toBob = startConversation(alice, "bob");
        alice.messenger->send(toBob, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        bob.messenger->acceptRequest(toAlice);

        // Alice sets an hour: Bob's side follows, with a note on each.
        QCOMPARE(Messenger::describeTimer(3600), QString("1 hour"));
        QCOMPARE(Messenger::describeTimer(4 * 7 * 24 * 3600), QString("4 weeks"));
        QCOMPARE(Messenger::describeTimer(0), QString("off"));
        alice.messenger->setDisappearing(toBob, 3600);
        QCOMPARE(alice.store->conversation(toBob).expireSeconds, 3600);
        QVERIFY(alice.store->messages(toBob).last().system);
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->conversation(toAlice).expireSeconds, 3600, kWait);
        QVERIFY(bob.store->messages(toAlice).last().system);
        QCOMPARE(bob.store->messages(toAlice).last().body, QString("alice set disappearing messages to 1 hour."));

        // A message: Alice's copy counts from sending, Bob's once he has seen it.
        const qint64 secret = alice.messenger->send(toBob, "this goes away");
        QCOMPARE(alice.store->message(secret).expiresAt, clock + hour);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("this goes away"), kWait);
        const ChatStore::Message got = bob.store->messages(toAlice).last();
        QCOMPARE(got.expireSeconds, 3600);
        QCOMPARE(got.expiresAt, 0); // not seen yet
        clock += hour / 2;
        bob.messenger->markRead(toAlice);
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->message(got.id).expiresAt, clock + hour, kWait);

        // A file, the same way.
        const QString path = m_dir.filePath("plan.txt");
        { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("the plan"); }
        QVERIFY(bob.messenger->sendFile(toAlice, path, QString()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->messages(toBob).size(), 4, kWait); // hi, the note, the secret, the file
        const qint64 planMessage = alice.store->messages(toBob).last().id;
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->attachments(planMessage).first().state, QString("ready"), kWait);
        alice.messenger->markRead(toBob);
        QTRY_VERIFY_WITH_TIMEOUT(alice.store->message(planMessage).expiresAt > 0, kWait);
        QCOMPARE(QDir(alice.store->attachmentFolder()).entryList(QDir::Files).size(), 1);

        // Half an hour on: Alice's own is gone (an hour since sent); Bob's copy stays a while.
        clock += hour / 2 + 1;
        alice.messenger->expireNow();
        bob.messenger->expireNow();
        QVERIFY(alice.store->message(secret).id == 0);
        QCOMPARE(bob.store->message(got.id).id, got.id);
        // And an hour after he read it, his too; the file with Alice's.
        clock += hour;
        alice.messenger->expireNow();
        bob.messenger->expireNow();
        QVERIFY(bob.store->message(got.id).id == 0);
        QVERIFY(alice.store->message(planMessage).id == 0);
        QCOMPARE(QDir(alice.store->attachmentFolder()).entryList(QDir::Files).size(), 0);
        // The notes and the earlier message stay.
        QCOMPARE(bob.store->messages(toAlice).first().body, QString("hi"));

        // Bob turns it off: new messages stay.
        bob.messenger->setDisappearing(toAlice, 0);
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->conversation(toBob).expireSeconds, 0, kWait);
        QCOMPARE(alice.store->messages(toBob).last().body, QString("bob set disappearing messages to off."));
        const qint64 stays = alice.messenger->send(toBob, "this stays");
        QCOMPARE(alice.store->message(stays).expiresAt, 0);

        // A stranger's timer starts nothing.
        const qint64 carolToBob = startConversation(carol, "bob");
        carol.messenger->setDisappearing(carolToBob, 60);
        QTest::qWait(300);
        QCOMPARE(bob.store->directConversation("carol"), 0);

        // In a group: any member sets it for all.
        std::optional<qint64> group;
        alice.messenger->createGroup("Quiet", {"bob"}, [&](qint64 c, const QString &) { group = c; });
        QTRY_VERIFY_WITH_TIMEOUT(group.has_value() && *group > 0, kWait);
        const QString gid = alice.store->group(*group).id;
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->groupConversation(gid) > 0, kWait);
        bob.messenger->setDisappearing(bob.store->groupConversation(gid), 300);
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->conversation(*group).expireSeconds, 300, kWait);
    }

    void replies()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        const qint64 toBob = startConversation(alice, "bob");
        const qint64 question = alice.messenger->send(toBob, "Pizza tonight?");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        bob.messenger->acceptRequest(toAlice);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("Pizza tonight?"), kWait);

        // Alice's second computer, to see her replies arrive there too.
        Device laptop(file("laptop"));
        std::optional<QString> code;
        std::optional<bool> linked;
        laptop.messenger->linkThisDevice(m_url, [&](const QString &shown) { code = shown; },
                                         [&](bool ok, const QString &) { linked = ok; });
        QTRY_VERIFY_WITH_TIMEOUT(code.has_value(), kWait);
        alice.messenger->linkDevice(*code, [](bool, const QString &) {});
        QTRY_VERIFY_WITH_TIMEOUT(linked.has_value() && *linked, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.messenger->myDevices().size(), 2, kWait);

        // Bob answers her question: the quote names it, and her.
        const qint64 theirs = bob.store->messages(toAlice).last().id;
        const qint64 answer = bob.messenger->send(toAlice, "Yes!", theirs);
        const ChatStore::Message sent = bob.store->message(answer);
        QVERIFY(sent.isReply());
        QCOMPARE(sent.quoteUid, alice.store->message(question).uid);
        QCOMPARE(sent.quoteAuthor, QString("alice"));
        QCOMPARE(sent.quoteText, QString("Pizza tonight?"));
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(toBob), QString("Yes!"), kWait);
        const ChatStore::Message got = alice.store->messages(toBob).last();
        QCOMPARE(got.quoteUid, alice.store->message(question).uid);
        QCOMPARE(got.quoteAuthor, QString("alice"));
        QCOMPARE(got.quoteText, QString("Pizza tonight?"));
        QCOMPARE(alice.store->messageByUid(toBob, got.quoteUid), question);

        // She answers his answer: on her laptop too, with its quote.
        alice.messenger->send(toBob, "Great", got.id);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("Great"), kWait);
        QCOMPARE(bob.store->messages(toAlice).last().quoteAuthor, QString("bob"));
        QCOMPARE(bob.store->messages(toAlice).last().quoteText, QString("Yes!"));
        const qint64 laptopToBob = laptop.store->directConversation("bob");
        QVERIFY(laptopToBob > 0);
        QTRY_COMPARE_WITH_TIMEOUT(laptop.last(laptopToBob), QString("Great"), kWait);
        QCOMPARE(laptop.store->messages(laptopToBob).last().quoteAuthor, QString("bob"));

        // Long texts are quoted by their start; a file by its name.
        const QString longText(500, QLatin1Char('x'));
        const qint64 essay = alice.messenger->send(toBob, longText);
        const auto quote = alice.messenger->quoteOf(essay);
        QVERIFY(quote);
        QCOMPARE(quote->text.size(), Protocol::MaxQuoteChars);
        QVERIFY(quote->text.endsWith(QStringLiteral("…")));
        const qint64 photo = alice.messenger->sendFile(
            toBob, writeFile("photo.png", QByteArray("\x89PNG\r\n\x1a\n", 8) + Crypto::randomBytes(100)), QString());
        QVERIFY(photo > 0);
        QCOMPARE(alice.messenger->quoteOf(photo)->text, QStringLiteral("📎 photo.png"));

        // Only a message of the same conversation; a note is not a reply to anything there.
        const qint64 note = alice.messenger->send(alice.store->noteToSelf(), "unrelated", question);
        QVERIFY(!alice.store->message(note).isReply());
        // In Note to Self, a reply to a note.
        const qint64 reply = alice.messenger->send(alice.store->noteToSelf(), "about that", note);
        QCOMPARE(alice.store->message(reply).quoteText, QString("unrelated"));
    }

    void reactions()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        Device carol(file("carol"));
        Device dave(file("dave"));
        for (auto [d, name] : {std::pair{&alice, "alice"}, {&bob, "bob"}, {&carol, "carol"}, {&dave, "dave"}})
            QVERIFY(signUp(*d, name));
        const qint64 toBob = startConversation(alice, "bob");
        const qint64 hello = alice.messenger->send(toBob, "Passed my exam!");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("Passed my exam!"), kWait);
        const qint64 theirs = bob.store->messages(toAlice).last().id;
        // Not on a request: accepting comes first.
        QVERIFY(!bob.messenger->react(theirs, "👍"));
        bob.messenger->acceptRequest(toAlice);

        QSignalSpy told(alice.messenger.get(), &Messenger::reactionReceived);
        QVERIFY(bob.messenger->react(theirs, "🎉"));
        QCOMPARE(bob.store->myReaction(theirs), QString("🎉"));
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->reactions(hello).size(), 1, kWait);
        QCOMPARE(alice.store->reactions(hello).first().author, QString("bob"));
        QCOMPARE(alice.store->reactions(hello).first().emoji, QString("🎉"));
        QTRY_COMPARE_WITH_TIMEOUT(told.size(), 1, kWait);
        QCOMPARE(told.first().at(2).toString(), QString("🎉"));
        QCOMPARE(told.first().at(3).toString(), QString("Passed my exam!"));
        QVERIFY(bob.store->pendingReactions().isEmpty()); // sent

        // Changed: one per person, the newest.
        QVERIFY(bob.messenger->react(theirs, "❤️"));
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->reactions(hello).value(0).emoji, QString("❤️"), kWait);
        QCOMPARE(alice.store->reactions(hello).size(), 1);
        // Alice reacts to her own; then Bob takes his back.
        QVERIFY(alice.messenger->react(hello, "😂"));
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->reactions(theirs).size(), 2, kWait);
        QVERIFY(bob.messenger->react(theirs, QString()));
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->reactions(hello).size(), 1, kWait);
        QCOMPARE(alice.store->reactions(hello).first().author, QString()); // hers alone
        QVERIFY(bob.store->pendingReactions().isEmpty());
        QCOMPARE(told.size(), 2); // no word for a removal, nor for her own

        // Made while offline: sent once back.
        bob.messenger->stop();
        QVERIFY(bob.messenger->react(theirs, "👍"));
        QCOMPARE(bob.store->pendingReactions().size(), 1);
        QTest::qWait(200);
        QCOMPARE(alice.store->reactions(hello).size(), 1);
        bob.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->reactions(hello).size(), 2, kWait);
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->pendingReactions().isEmpty(), kWait);

        // Not something that is not an emoji.
        QVERIFY(!bob.messenger->react(theirs, "lol"));
        QVERIFY(!bob.messenger->react(theirs, "👍 👍"));

        // In a group, from Carol, to all.
        std::optional<qint64> group;
        alice.messenger->createGroup("Study", {"bob", "carol"}, [&](qint64 c, const QString &) { group = c; });
        QTRY_VERIFY_WITH_TIMEOUT(group.has_value() && *group > 0, kWait);
        const QString gid = alice.store->group(*group).id;
        QTRY_VERIFY_WITH_TIMEOUT(carol.store->groupConversation(gid) > 0, kWait);
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->groupConversation(gid) > 0, kWait);
        const qint64 carolGroup = carol.store->groupConversation(gid);
        const qint64 bobGroup = bob.store->groupConversation(gid);
        carol.messenger->acceptRequest(carolGroup);
        bob.messenger->acceptRequest(bobGroup);
        const qint64 news = alice.messenger->send(*group, "Exam moved to Friday");
        QTRY_COMPARE_WITH_TIMEOUT(carol.last(carolGroup), QString("Exam moved to Friday"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobGroup), QString("Exam moved to Friday"), kWait);
        QVERIFY(carol.messenger->react(carol.store->messages(carolGroup).last().id, "😮"));
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->reactions(news).size(), 1, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->reactions(bob.store->messages(bobGroup).last().id).size(), 1, kWait);
        QCOMPARE(alice.store->reactions(news).first().author, QString("carol"));

        // Forgeries by Dave, by hand: on Alice's message to Bob (not his
        // conversation), into the group he is not in, and with a wrong author.
        const qint64 daveToBob = startConversation(dave, "bob");
        dave.messenger->send(daveToBob, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("dave") > 0, kWait);
        const QString uid = alice.store->message(hello).uid;
        const QString daveMessage = dave.store->messages(daveToBob).last().uid;
        sendByHand(dave, "bob", {Protocol::Content::reaction(uid, "alice", "💩", false),
                                 Protocol::Content::reaction(alice.store->message(news).uid, "alice", "💩", false, gid),
                                 Protocol::Content::reaction(daveMessage, "bob", "💩", false)});
        QTRY_COMPARE_WITH_TIMEOUT(m_serverStore->queuedCount("bob", 1), 0, kWait);
        QTest::qWait(200);
        for (const ChatStore::Conversation &c : bob.store->conversations())
            for (const ChatStore::Message &m : bob.store->messages(c.id))
                for (const ChatStore::Reaction &r : bob.store->reactions(m.id))
                    QVERIFY(r.emoji != QLatin1String("💩"));

        // Gone with their message.
        bob.messenger->deleteConversation(toAlice);
        QVERIFY(bob.store->reactions(theirs).isEmpty());
    }

    void deleting()
    {
        qint64 clock = QDateTime::currentMSecsSinceEpoch();
        Device alice(file("alice"));
        Device bob(file("bob"));
        Device dave(file("dave"));
        for (Device *d : {&alice, &bob, &dave})
            d->messenger->setClock([&clock] { return clock; });
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        QVERIFY(signUp(dave, "dave"));
        const qint64 toBob = startConversation(alice, "bob");
        alice.messenger->send(toBob, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        bob.messenger->acceptRequest(toAlice);

        // Alice's laptop, to see deletes reach it too.
        Device laptop(file("laptop"));
        laptop.messenger->setClock([&clock] { return clock; });
        std::optional<QString> code;
        std::optional<bool> linked;
        laptop.messenger->linkThisDevice(m_url, [&](const QString &shown) { code = shown; },
                                         [&](bool ok, const QString &) { linked = ok; });
        QTRY_VERIFY_WITH_TIMEOUT(code.has_value(), kWait);
        alice.messenger->linkDevice(*code, [](bool, const QString &) {});
        QTRY_VERIFY_WITH_TIMEOUT(linked.has_value() && *linked, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.messenger->myDevices().size(), 2, kWait);

        // A photo with a caption: Bob has it, reacts and replies to it.
        const qint64 oops = alice.messenger->sendFile(toBob, writeFile("oops.png", QByteArray("\x89PNG\r\n\x1a\n", 8) + Crypto::randomBytes(500)), "wrong chat");
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(oops).status, QString("delivered"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("wrong chat"), kWait);
        const qint64 theirs = bob.store->messages(toAlice).last().id;
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->attachments(theirs).value(0).state, QString("ready"), kWait);
        const QString blob = bob.store->attachments(theirs).first().file;
        QVERIFY(QFile::exists(bob.store->attachmentFolder() + "/" + blob));
        QVERIFY(bob.messenger->react(theirs, "😂"));
        const qint64 reply = bob.messenger->send(toAlice, "lol", theirs);
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->reactions(oops).size(), 1, kWait);
        const qint64 laptopToBob = laptop.store->directConversation("bob");
        const QString oopsUid = alice.store->message(oops).uid;
        QTRY_VERIFY_WITH_TIMEOUT(laptop.store->messageByUid(laptopToBob, oopsUid) > 0, kWait);
        const qint64 laptopCopy = laptop.store->messageByUid(laptopToBob, oopsUid);
        QCOMPARE(laptop.store->message(laptopCopy).body, QString("wrong chat"));

        // Only mine, only once sent.
        QVERIFY(!bob.messenger->canDeleteForEveryone(theirs));
        QVERIFY(alice.messenger->canDeleteForEveryone(oops));
        QVERIFY(alice.messenger->deleteForEveryone(oops));
        QVERIFY(alice.store->message(oops).deleted);
        QVERIFY(alice.store->message(oops).body.isEmpty());
        QVERIFY(alice.store->attachments(oops).isEmpty());
        QVERIFY(alice.store->reactions(oops).isEmpty());
        QVERIFY(!alice.messenger->canDeleteForEveryone(oops)); // once
        // Bob: the mark only; no text, file, reaction, nor quote of it.
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->message(theirs).deleted, kWait);
        QVERIFY(bob.store->message(theirs).body.isEmpty());
        QVERIFY(bob.store->attachments(theirs).isEmpty());
        QVERIFY(!QFile::exists(bob.store->attachmentFolder() + "/" + blob));
        QVERIFY(bob.store->reactions(theirs).isEmpty());
        QVERIFY(bob.store->message(reply).isReply());
        QVERIFY(bob.store->message(reply).quoteText.isEmpty());
        QVERIFY(!bob.messenger->react(theirs, "👍"));
        QVERIFY(!bob.messenger->quoteOf(theirs));
        // Her laptop too.
        QTRY_VERIFY_WITH_TIMEOUT(laptop.store->message(laptopCopy).deleted, kWait);
        QVERIFY(laptop.store->message(laptopCopy).body.isEmpty());
        QVERIFY(alice.store->pendingDeletes().isEmpty());

        // Made offline: it goes when she is back.
        const qint64 second = alice.messenger->send(toBob, "typo");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("typo"), kWait);
        QTRY_VERIFY_WITH_TIMEOUT(alice.store->message(second).status != QLatin1String("sending"), kWait);
        alice.messenger->stop();
        QVERIFY(alice.messenger->deleteForEveryone(second));
        QCOMPARE(alice.store->pendingDeletes().size(), 1);
        alice.messenger->start();
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->messages(toAlice).last().deleted, kWait);
        QTRY_VERIFY_WITH_TIMEOUT(alice.store->pendingDeletes().isEmpty(), kWait);

        // A day later: too late.
        const qint64 old = alice.messenger->send(toBob, "yesterday's");
        QTRY_VERIFY_WITH_TIMEOUT(alice.store->message(old).status != QLatin1String("sending"), kWait);
        clock += Messenger::DeleteForEveryoneMs + 60000;
        QVERIFY(!alice.messenger->canDeleteForEveryone(old));
        QVERIFY(!alice.messenger->deleteForEveryone(old));
        clock -= Messenger::DeleteForEveryoneMs + 60000;

        // For me: here only.
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("yesterday's"), kWait);
        const qint64 bobsCopy = bob.store->messages(toAlice).last().id;
        bob.messenger->deleteForMe(bobsCopy);
        QCOMPARE(bob.store->message(bobsCopy).id, 0);
        QVERIFY(!alice.store->message(old).deleted);

        // Forgeries by Dave: deleting Alice's message, as himself and as her.
        const qint64 kept = alice.messenger->send(toBob, "keep me");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("keep me"), kWait);
        const QString keptUid = alice.store->message(kept).uid;
        const qint64 daveToBob = startConversation(dave, "bob");
        dave.messenger->send(daveToBob, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("dave") > 0, kWait);
        sendByHand(dave, "bob", {Protocol::Content::deletion(keptUid, "alice"), Protocol::Content::deletion(keptUid, "dave")});
        QTRY_COMPARE_WITH_TIMEOUT(m_serverStore->queuedCount("bob", 1), 0, kWait);
        QTest::qWait(200);
        QVERIFY(!bob.store->messages(toAlice).last().deleted);
        QCOMPARE(bob.last(toAlice), QString("keep me"));
    }

    void editing()
    {
        qint64 clock = QDateTime::currentMSecsSinceEpoch();
        Device alice(file("alice"));
        Device bob(file("bob"));
        Device dave(file("dave"));
        for (Device *d : {&alice, &bob, &dave})
            d->messenger->setClock([&clock] { return clock; });
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        QVERIFY(signUp(dave, "dave"));
        const qint64 toBob = startConversation(alice, "bob");
        const qint64 plan = alice.messenger->send(toBob, "Meet at 5");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        bob.messenger->acceptRequest(toAlice);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("Meet at 5"), kWait);
        const qint64 theirs = bob.store->messages(toAlice).last().id;
        const qint64 reply = bob.messenger->send(toAlice, "ok", theirs);
        QTRY_COMPARE_WITH_TIMEOUT(alice.last(toBob), QString("ok"), kWait);

        Device laptop(file("laptop"));
        std::optional<QString> code;
        std::optional<bool> linked;
        laptop.messenger->linkThisDevice(m_url, [&](const QString &shown) { code = shown; },
                                         [&](bool ok, const QString &) { linked = ok; });
        QTRY_VERIFY_WITH_TIMEOUT(code.has_value(), kWait);
        alice.messenger->linkDevice(*code, [](bool, const QString &) {});
        QTRY_VERIFY_WITH_TIMEOUT(linked.has_value() && *linked, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.messenger->myDevices().size(), 2, kWait);
        const QString planUid = alice.store->message(plan).uid;
        // Her laptop learns of it now (it was sent before the link): a new message, then an edit of that.
        const qint64 later = alice.messenger->send(toBob, "Bring the map");
        const QString laterUid = alice.store->message(later).uid;
        const qint64 laptopToBob = [&] {
            [[maybe_unused]] const bool there = QTest::qWaitFor([&] { return laptop.store->directConversation("bob") > 0; }, kWait);
            return laptop.store->directConversation("bob");
        }();
        QTRY_VERIFY_WITH_TIMEOUT(laptop.store->messageByUid(laptopToBob, laterUid) > 0, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("Bring the map"), kWait);

        // Only mine, only sent.
        QVERIFY(!bob.messenger->canEdit(theirs));
        QVERIFY(!bob.messenger->edit(theirs, "Meet at 6"));
        QVERIFY(alice.messenger->canEdit(plan));
        QVERIFY(!alice.messenger->edit(plan, "Meet at 5")); // the same
        QVERIFY(!alice.messenger->edit(plan, QString()));  // nothing left
        QVERIFY(alice.messenger->edit(plan, "Meet at 6"));
        QCOMPARE(alice.store->message(plan).body, QString("Meet at 6"));
        QVERIFY(alice.store->message(plan).editedAt > 0);
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->message(theirs).body, QString("Meet at 6"), kWait);
        QVERIFY(bob.store->message(theirs).editedAt > 0);
        // A reply keeps the words it answered.
        QCOMPARE(bob.store->message(reply).quoteText, QString("Meet at 5"));
        QTRY_VERIFY_WITH_TIMEOUT(alice.store->pendingEdits().isEmpty(), kWait);
        // And on her laptop, the one it has.
        QVERIFY(alice.messenger->edit(later, "Bring the map and water"));
        const qint64 laptopLater = laptop.store->messageByUid(laptopToBob, laterUid);
        QTRY_COMPARE_WITH_TIMEOUT(laptop.store->message(laptopLater).body, QString("Bring the map and water"), kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("Bring the map and water"), kWait);

        // Twice quickly: the last wins.
        QVERIFY(alice.messenger->edit(plan, "Meet at 7"));
        QVERIFY(alice.messenger->edit(plan, "Meet at 8"));
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->message(theirs).body, QString("Meet at 8"), kWait);
        QTRY_VERIFY_WITH_TIMEOUT(alice.store->pendingEdits().isEmpty(), kWait);
        QTest::qWait(200);
        QCOMPARE(bob.store->message(theirs).body, QString("Meet at 8"));

        // Offline: it goes when she is back.
        alice.messenger->stop();
        QVERIFY(alice.messenger->edit(plan, "Meet at 9"));
        QCOMPARE(alice.store->pendingEdits().size(), 1);
        alice.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->message(theirs).body, QString("Meet at 9"), kWait);

        // A day later: too late. A deleted one: never.
        clock += Messenger::EditMs + 60000;
        QVERIFY(!alice.messenger->canEdit(plan));
        clock -= Messenger::EditMs + 60000;
        QVERIFY(alice.messenger->deleteForEveryone(later));
        QVERIFY(!alice.messenger->canEdit(later));
        // A note to self: any time.
        const qint64 note = alice.messenger->send(alice.store->noteToSelf(), "milk");
        QVERIFY(alice.messenger->edit(note, "milk, eggs"));
        QCOMPARE(alice.store->message(note).body, QString("milk, eggs"));
        QVERIFY(alice.store->pendingEdits().isEmpty());

        // Forgeries by Dave: editing Alice's message, as himself and as her.
        const qint64 daveToBob = startConversation(dave, "bob");
        dave.messenger->send(daveToBob, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("dave") > 0, kWait);
        sendByHand(dave, "bob", {Protocol::Content::edit(planUid, "alice", "Meet at midnight, come alone"),
                                 Protocol::Content::edit(planUid, "dave", "Meet at midnight, come alone")});
        QTRY_COMPARE_WITH_TIMEOUT(m_serverStore->queuedCount("bob", 1), 0, kWait);
        QTest::qWait(200);
        QCOMPARE(bob.store->message(theirs).body, QString("Meet at 9"));
    }

    // Read receipts off (as Signal's, both ways): mine not sent, theirs shown as delivered.
    void readReceiptsOff()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        const qint64 toBob = startConversation(alice, "bob");
        auto status = [&](const QString &body) {
            for (const ChatStore::Message &m : alice.store->messages(toBob))
                if (m.body == body)
                    return m.status;
            return QString();
        };
        alice.messenger->send(toBob, "one");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        bob.messenger->acceptRequest(toAlice);
        bob.messenger->markRead(toAlice);
        QTRY_COMPARE_WITH_TIMEOUT(status("one"), QString("read"), kWait);

        // Bob turns his off: Alice learns only that it arrived.
        bob.messenger->setReadReceipts(false);
        alice.messenger->send(toBob, "two");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("two"), kWait);
        bob.messenger->markRead(toAlice);
        QTRY_COMPARE_WITH_TIMEOUT(status("two"), QString("delivered"), kWait);
        QTest::qWait(300);
        QCOMPARE(status("two"), QString("delivered"));
        QVERIFY(bob.store->unseenUids(toAlice).isEmpty()); // read here all the same

        // Bob's on again, Alice's off: his receipts reach her, but say "delivered".
        bob.messenger->setReadReceipts(true);
        alice.messenger->setReadReceipts(false);
        alice.messenger->send(toBob, "three");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("three"), kWait);
        bob.messenger->markRead(toAlice);
        QTRY_COMPARE_WITH_TIMEOUT(status("three"), QString("delivered"), kWait);
        QTest::qWait(300);
        QCOMPARE(status("three"), QString("delivered"));
        QCOMPARE(status("one"), QString("read")); // what was read before stays so
    }

    // @mentions (as Signal's): in groups only, of members only; mine notified.
    void mentions()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        Device carol(file("carol"));
        Device dave(file("dave"));
        for (auto &[device, name] : QList<QPair<Device *, QString>>{{&alice, "alice"}, {&bob, "bob"}, {&carol, "carol"}, {&dave, "dave"}})
            QVERIFY(signUp(*device, name));
        const qint64 aliceToBob = startConversation(alice, "bob");
        alice.messenger->send(aliceToBob, "hi Bob");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 bobToAlice = bob.store->directConversation("alice");
        bob.messenger->acceptRequest(bobToAlice);
        std::optional<qint64> created;
        alice.messenger->createGroup("Trip", {"bob", "carol"}, [&](qint64 c, const QString &) { created = c; });
        QTRY_VERIFY_WITH_TIMEOUT(created.has_value() && *created > 0, kWait);
        const qint64 trip = *created;
        const QString gid = alice.store->group(trip).id;
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->groupConversation(gid) > 0, kWait);
        QTRY_VERIFY_WITH_TIMEOUT(carol.store->groupConversation(gid) > 0, kWait);
        const qint64 bobTrip = bob.store->groupConversation(gid);
        const qint64 carolTrip = carol.store->groupConversation(gid);
        carol.messenger->acceptRequest(carolTrip);
        QSignalSpy bobHeard(bob.messenger.get(), &Messenger::messageReceived);
        QSignalSpy carolHeard(carol.messenger.get(), &Messenger::messageReceived);

        // Alice mentions Bob, and Dave (not in it) and herself: only Bob's stays.
        const QString text = "@Bob @Dave @Alice pack";
        const qint64 sent = alice.messenger->send(
            trip, text, 0, {Protocol::Mention{0, 4, "bob"}, Protocol::Mention{5, 5, "dave"}, Protocol::Mention{11, 6, "alice"}});
        const QList<Protocol::Mention> onlyBob{Protocol::Mention{0, 4, "bob"}};
        QCOMPARE(alice.store->message(sent).mentions, onlyBob);
        QVERIFY(!alice.store->message(sent).mentionsMe);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobTrip), text, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(carol.last(carolTrip), text, kWait);
        const ChatStore::Message atBob = bob.store->messages(bobTrip).last();
        QCOMPARE(atBob.mentions, onlyBob);
        QVERIFY(atBob.mentionsMe);
        QVERIFY(bob.store->conversation(bobTrip).mentioned);
        QTRY_COMPARE_WITH_TIMEOUT(bobHeard.size(), 1, kWait);
        QVERIFY(bobHeard.last().at(4).toBool()); // a mention of him
        QCOMPARE(carol.store->messages(carolTrip).last().mentions, onlyBob);
        QVERIFY(!carol.store->messages(carolTrip).last().mentionsMe);
        QVERIFY(!carol.store->conversation(carolTrip).mentioned);
        QTRY_COMPARE_WITH_TIMEOUT(carolHeard.size(), 1, kWait);
        QVERIFY(!carolHeard.last().at(4).toBool());
        // Read: no longer marked.
        bob.messenger->markRead(bobTrip);
        QVERIFY(!bob.store->conversation(bobTrip).mentioned);

        // An edit moves it to Carol.
        QTRY_VERIFY_WITH_TIMEOUT(alice.messenger->canEdit(sent), kWait);
        QVERIFY(alice.messenger->edit(sent, "@Carol pack", {Protocol::Mention{0, 6, "carol"}}));
        QTRY_COMPARE_WITH_TIMEOUT(carol.last(carolTrip), QString("@Carol pack"), kWait);
        QVERIFY(carol.store->messages(carolTrip).last().mentionsMe);
        QVERIFY(carol.store->conversation(carolTrip).mentioned);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobTrip), QString("@Carol pack"), kWait);
        QCOMPARE(bob.store->messages(bobTrip).last().mentions, (QList<Protocol::Mention>{Protocol::Mention{0, 6, "carol"}}));
        QVERIFY(!bob.store->messages(bobTrip).last().mentionsMe);
        // The same text, the mention gone: an edit too.
        QVERIFY(alice.messenger->edit(sent, "@Carol pack"));
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->messages(bobTrip).last().mentions.isEmpty(), kWait);

        // In a direct chat: none.
        const qint64 direct = alice.messenger->send(aliceToBob, "@Bob", 0, {Protocol::Mention{0, 4, "bob"}});
        QVERIFY(alice.store->message(direct).mentions.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobToAlice), QString("@Bob"), kWait);
        QVERIFY(bob.store->messages(bobToAlice).last().mentions.isEmpty());
        // A file's caption may have them.
        QString error;
        const qint64 file = alice.messenger->sendFileData(trip, "map.txt", "x", "@Bob the map", &error, 0,
                                                          {Protocol::Mention{0, 4, "bob"}});
        QVERIFY2(file > 0, qPrintable(error));
        QCOMPARE(alice.store->message(file).mentions, onlyBob);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobTrip), QString("@Bob the map"), kWait);
        QVERIFY(bob.store->messages(bobTrip).last().mentionsMe);

        // Carol, by hand: a mention of Dave (no member) is dropped, Bob's kept;
        // one direct to Bob keeps none.
        const qint64 carolToBob = startConversation(carol, "bob");
        carol.messenger->send(carolToBob, "hello");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("carol") > 0, kWait);
        const qint64 bobToCarol = bob.store->directConversation("carol");
        QTRY_COMPARE_WITH_TIMEOUT(m_serverStore->queuedCount("bob", 1), 0, kWait);
        QTest::qWait(5); // newer than "hello"
        Protocol::Content forged = Protocol::Content::text("@Bob @Dave");
        forged.group = gid;
        forged.mentions = {Protocol::Mention{0, 4, "bob"}, Protocol::Mention{5, 5, "dave"}};
        Protocol::Content toHim = Protocol::Content::text("@Bob direct");
        toHim.mentions = {Protocol::Mention{0, 4, "bob"}};
        sendByHand(carol, "bob", {forged, toHim});
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobTrip), QString("@Bob @Dave"), kWait);
        QCOMPARE(bob.store->messages(bobTrip).last().mentions, onlyBob);
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(bobToCarol), QString("@Bob direct"), kWait);
        QVERIFY(bob.store->messages(bobToCarol).last().mentions.isEmpty());
    }

    void typing()
    {
        qint64 clock = QDateTime::currentMSecsSinceEpoch();
        Device alice(file("alice"));
        Device bob(file("bob"));
        Device carol(file("carol"));
        for (Device *d : {&alice, &bob, &carol})
            d->messenger->setClock([&clock] { return clock; });
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        QVERIFY(signUp(carol, "carol"));
        const qint64 toBob = startConversation(alice, "bob");
        alice.messenger->send(toBob, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");

        // A request: nothing either way.
        alice.messenger->setTyping(toBob, true);
        QTest::qWait(300);
        QVERIFY(bob.messenger->typingIn(toAlice).isEmpty());
        alice.messenger->setTyping(toBob, false);
        bob.messenger->acceptRequest(toAlice);

        QSignalSpy changed(bob.messenger.get(), &Messenger::typingChanged);
        alice.messenger->setTyping(toBob, true);
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->typingIn(toAlice), QStringList{"alice"}, kWait);
        QVERIFY(!changed.isEmpty());
        alice.messenger->setTyping(toBob, false);
        QTRY_VERIFY_WITH_TIMEOUT(bob.messenger->typingIn(toAlice).isEmpty(), kWait);
        // Her message ends it.
        alice.messenger->setTyping(toBob, true);
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->typingIn(toAlice).size(), 1, kWait);
        alice.messenger->send(toBob, "there");
        QTRY_COMPARE_WITH_TIMEOUT(bob.last(toAlice), QString("there"), kWait);
        QVERIFY(bob.messenger->typingIn(toAlice).isEmpty());
        // Silence: gone after a while.
        alice.messenger->setTyping(toBob, true);
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->typingIn(toAlice).size(), 1, kWait);
        clock += Messenger::TypingShownMs + 1;
        bob.messenger->expireTyping();
        QVERIFY(bob.messenger->typingIn(toAlice).isEmpty());
        alice.messenger->setTyping(toBob, false);

        // Never kept for later: Bob away, nothing waits for him.
        bob.messenger->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!m_server->isOnline("bob", 1), kWait);
        alice.messenger->setTyping(toBob, true);
        QTest::qWait(300);
        QCOMPARE(m_serverStore->queuedCount("bob", 1), 0);
        alice.messenger->setTyping(toBob, false);
        bob.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->state(), Messenger::State::Online, kWait);

        // Off: not shown, not sent.
        bob.messenger->setTypingIndicators(false);
        alice.messenger->setTyping(toBob, true);
        QTest::qWait(300);
        QVERIFY(bob.messenger->typingIn(toAlice).isEmpty());
        alice.messenger->setTyping(toBob, false);
        QSignalSpy aliceSees(alice.messenger.get(), &Messenger::typingChanged);
        bob.messenger->setTyping(toAlice, true);
        QTest::qWait(300);
        QVERIFY(aliceSees.isEmpty());
        bob.messenger->setTypingIndicators(true);

        // In a group: who.
        std::optional<qint64> group;
        alice.messenger->createGroup("Lunch", {"bob", "carol"}, [&](qint64 c, const QString &) { group = c; });
        QTRY_VERIFY_WITH_TIMEOUT(group.has_value() && *group > 0, kWait);
        const QString gid = alice.store->group(*group).id;
        QTRY_VERIFY_WITH_TIMEOUT(carol.store->groupConversation(gid) > 0, kWait);
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->groupConversation(gid) > 0, kWait);
        carol.messenger->acceptRequest(carol.store->groupConversation(gid));
        bob.messenger->acceptRequest(bob.store->groupConversation(gid));
        carol.messenger->setTyping(carol.store->groupConversation(gid), true);
        QTRY_COMPARE_WITH_TIMEOUT(alice.messenger->typingIn(*group), QStringList{"carol"}, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.messenger->typingIn(bob.store->groupConversation(gid)), QStringList{"carol"}, kWait);
    }

    void voiceMessages()
    {
        Device alice(file("alice"));
        Device bob(file("bob"));
        QVERIFY(signUp(alice, "alice"));
        QVERIFY(signUp(bob, "bob"));
        const qint64 toBob = startConversation(alice, "bob");
        alice.messenger->send(toBob, "listen to this");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("alice") > 0, kWait);
        const qint64 toAlice = bob.store->directConversation("alice");
        bob.messenger->acceptRequest(toAlice);

        // Two seconds of sound: a tone that rises.
        QByteArray samples(2 * Voice::SampleRate * 2, Qt::Uninitialized);
        auto *pcm = reinterpret_cast<qint16 *>(samples.data());
        for (int i = 0; i < 2 * Voice::SampleRate; ++i)
            pcm[i] = qint16(8000 * std::sin(2 * 3.141592653589793 * (300.0 + i / 160.0) * i / Voice::SampleRate));
        QString error;
        QCOMPARE(alice.messenger->sendVoice(toBob, QByteArray(100, '\0'), &error), 0); // too short
        QVERIFY(!error.isEmpty());
        // A recording's own file: only a clean Ogg Opus of 0.1 s or more.
        QCOMPARE(alice.messenger->sendVoiceNote(toBob, "OggS rubbish", &error), 0);
        QCOMPARE(alice.messenger->sendVoiceNote(toBob, Voice::toOpus(QByteArray(800, '\0')), &error), 0); // 25 ms
        QCOMPARE(alice.messenger->sendVoiceNote(toBob, Voice::toWav(samples), &error), 0);
        const qint64 voice = alice.messenger->sendVoice(toBob, samples, &error);
        QVERIFY2(voice > 0, qPrintable(error));
        const ChatStore::StoredAttachment mine = alice.store->attachments(voice).value(0);
        QVERIFY(mine.voice);
        QCOMPARE(mine.type, QString("audio/ogg")); // Opus
        QVERIFY(mine.name.endsWith(QStringLiteral(".ogg")));
        QVERIFY(mine.size < samples.size() / 8); // 64 KB of samples, about 4 KB sent
        QTRY_COMPARE_WITH_TIMEOUT(alice.store->message(voice).status, QString("delivered"), kWait);

        QTRY_VERIFY_WITH_TIMEOUT(bob.store->messages(toAlice).size() == 2, kWait);
        const qint64 theirs = bob.store->messages(toAlice).last().id;
        QTRY_COMPARE_WITH_TIMEOUT(bob.store->attachments(theirs).value(0).state, QString("ready"), kWait);
        const ChatStore::StoredAttachment got = bob.store->attachments(theirs).first();
        QVERIFY(got.voice);
        const auto data = bob.messenger->attachmentData(got.id);
        QVERIFY(data);
        const auto audio = Voice::decode(*data);
        QVERIFY(audio);
        QCOMPARE(audio->sampleRate, 48000);
        QVERIFY(qAbs(audio->durationMs() - 2000) <= 30);
        // In the list: a voice message.
        QCOMPARE(bob.store->conversation(toAlice).lastText, QStringLiteral("🎤 Voice message"));
        QCOMPARE(bob.messenger->quoteOf(theirs)->text, QStringLiteral("🎤 Voice message")); // a reply's quote
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
    QString writeFile(const QString &name, const QByteArray &data)
    {
        const QString path = m_dir.filePath(name);
        QFile out(path);
        if (out.open(QIODevice::WriteOnly))
            out.write(data);
        return path;
    }

    // Contents sent by hand with `from`'s keys, as a changed program could,
    // to device 1 of `to` (`from` stops meanwhile, so nothing else uses the session).
    void sendByHand(Device &from, const QString &to, const QList<Protocol::Content> &contents)
    {
        from.messenger->stop();
        std::optional<Session> session = from.keys->session(to, 1);
        QVERIFY(session);
        QList<QHash<quint32, QByteArray>> wires;
        for (const Protocol::Content &content : contents) {
            const QByteArray encoded = Protocol::encodeContent(content);
            QVERIFY(!encoded.isEmpty());
            wires.append({{1, session->encrypt(encoded)}});
        }
        from.keys->saveSession(to, 1, *session);
        ChatClient raw;
        QVERIFY(raw.open(m_url));
        QTRY_VERIFY_WITH_TIMEOUT(raw.isConnected(), kWait);
        int sent = 0;
        bool ok = true;
        raw.login(from.messenger->username(), 1, from.keys->account()->identity, [&](const ChatClient::Result &r) {
            ok = ok && r.ok;
            for (const auto &wire : wires)
                raw.send(to, wire, [&](const ChatClient::Result &s) {
                    ok = ok && s.ok;
                    ++sent;
                });
        });
        QTRY_COMPARE_WITH_TIMEOUT(sent, int(wires.size()), kWait);
        QVERIFY(ok);
        raw.close();
        from.messenger->start();
        QTRY_COMPARE_WITH_TIMEOUT(from.messenger->state(), Messenger::State::Online, kWait);
    }

    QString file(const QString &name) { return m_dir.filePath(QStringLiteral("%1-%2/chat.sqlite").arg(name).arg(++m_count)); }

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
