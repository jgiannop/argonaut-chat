#include "Attachment.h"
#include "ChatServer.h"
#include "Messenger.h"
#include "ServerStore.h"
#include "TestSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

        // Kept over a restart.
        bob.close();
        bob.open();
        QCOMPARE(bob.messenger->displayName("alice"), QString("Alice S."));
        QCOMPARE(bob.store->messages(fromAlice).size(), 1);
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
