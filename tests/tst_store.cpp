#include "AppLock.h"
#include "ChatStore.h"
#include "Crypto.h"
#include "CipherSql.h"
#include "KeyStore.h"
#include "TestSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTest>

class TestStore : public QObject
{
    Q_OBJECT

private slots:
    void noteToSelfAndMessages()
    {
        const QString file = TestSupport::home() + QStringLiteral("/chat.sqlite");
        qint64 self = 0;
        {
            ChatStore store(file);
            QVERIFY(store.isOpen());
            self = store.noteToSelf();
            QVERIFY2(self > 0, qPrintable(store.error()));
            QCOMPARE(store.noteToSelf(), self); // one, always the same
            QSignalSpy changed(&store, &ChatStore::messagesChanged);
            const QDateTime t = QDateTime::currentDateTimeUtc();
            store.addMessage(self, QStringLiteral("first"), QString(), t.addSecs(-10));
            const ChatStore::Message m = store.addMessage(self, QStringLiteral("second"), QString(), t);
            QCOMPARE(changed.size(), 2);
            QCOMPARE(m.status, QStringLiteral("sent"));
            const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("Anna"), QStringLiteral("anna"));
            store.addMessage(anna, QStringLiteral("hi!"), QStringLiteral("anna"), t.addSecs(5));
            store.addMessage(anna, QStringLiteral("there?"), QStringLiteral("anna"), t.addSecs(6));
            // Newest activity first, with the last message and what is unread.
            const QList<ChatStore::Conversation> list = store.conversations();
            QCOMPARE(list.size(), 2);
            QCOMPARE(list.first().title, QStringLiteral("Anna"));
            QCOMPARE(list.first().lastText, QStringLiteral("there?"));
            QCOMPARE(list.first().unread, 2);
            QCOMPARE(list.last().unread, 0); // what I write is never unread
            store.markRead(anna);
            QCOMPARE(store.conversation(anna).unread, 0);
        }
        // Kept: the history is on this computer, and only for its user.
        ChatStore again(file);
        const QList<ChatStore::Message> notes = again.messages(self);
        QCOMPARE(notes.size(), 2);
        QCOMPARE(notes.first().body, QStringLiteral("first")); // oldest first
        QVERIFY(notes.first().outgoing());
        QVERIFY(TestSupport::onlyMine(file));
    }

    void wireIdsOutboxAndReceipts()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/wire.sqlite"));
        const qint64 bob = store.addConversation(QStringLiteral("direct"), QStringLiteral("bob"), QStringLiteral("bob"));
        QCOMPARE(store.directConversation(QStringLiteral("bob")), bob);
        QCOMPARE(store.directConversation(QStringLiteral("carol")), 0);

        const auto a = store.addMessage(bob, QStringLiteral("a"), QString(), QDateTime::currentDateTimeUtc(),
                                        QStringLiteral("id-a"), QStringLiteral("sending"));
        const auto b = store.addMessage(bob, QStringLiteral("b"), QString(), QDateTime::currentDateTimeUtc(),
                                        QStringLiteral("id-b"), QStringLiteral("sending"));
        QCOMPARE(store.message(a.id).uid, QStringLiteral("id-a"));
        QVERIFY(store.hasMessage(bob, QStringLiteral("id-a")));
        QVERIFY(!store.hasMessage(bob, QStringLiteral("id-x")));
        QVERIFY(!store.hasMessage(bob, QString()));
        QCOMPARE(store.outbox().size(), 2);
        store.setStatus(a.id, QStringLiteral("sent"));
        QCOMPARE(store.outbox().size(), 1);

        // Receipts move on, never back; "sending" too (the receipt can come first).
        store.advanceStatus(bob, {QStringLiteral("id-a"), QStringLiteral("id-b")}, QStringLiteral("read"));
        QCOMPARE(store.message(a.id).status, QStringLiteral("read"));
        QCOMPARE(store.message(b.id).status, QStringLiteral("read"));
        store.advanceStatus(bob, {QStringLiteral("id-a")}, QStringLiteral("delivered"));
        QCOMPARE(store.message(a.id).status, QStringLiteral("read"));
        // Only mine, only in that conversation.
        const qint64 carol = store.addConversation(QStringLiteral("direct"), QStringLiteral("carol"), QStringLiteral("carol"));
        const auto theirs = store.addMessage(carol, QStringLiteral("c"), QStringLiteral("carol"),
                                             QDateTime::currentDateTimeUtc(), QStringLiteral("id-c"));
        const auto mine = store.addMessage(carol, QStringLiteral("d"), QString(), QDateTime::currentDateTimeUtc(),
                                           QStringLiteral("id-d"), QStringLiteral("sent"));
        store.advanceStatus(bob, {QStringLiteral("id-d")}, QStringLiteral("read"));
        store.advanceStatus(carol, {QStringLiteral("id-c")}, QStringLiteral("read"));
        QCOMPARE(store.message(mine.id).status, QStringLiteral("sent"));
        QCOMPARE(store.message(theirs.id).status, QStringLiteral("received"));
        QCOMPARE(store.unseenUids(carol), QStringList{QStringLiteral("id-c")});
        store.markRead(carol);
        QVERIFY(store.unseenUids(carol).isEmpty());
    }

    void oldDatabasesGetTheWireId()
    {
        // A database from before the network, without the uid column.
        const QString file = TestSupport::home() + QStringLiteral("/old.sqlite");
        {
            CipherSql::registerDriver();
            QSqlDatabase db = QSqlDatabase::addDatabase(CipherSql::DriverName, QStringLiteral("old"));
            db.setDatabaseName(file);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE conversations (id INTEGER PRIMARY KEY, kind TEXT NOT NULL,"
                                          " title TEXT NOT NULL DEFAULT '', peer TEXT NOT NULL DEFAULT '',"
                                          " created INTEGER NOT NULL, last_activity INTEGER NOT NULL)")));
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE messages (id INTEGER PRIMARY KEY, conversation INTEGER NOT NULL,"
                                          " author TEXT NOT NULL DEFAULT '', body TEXT NOT NULL, sent_at INTEGER NOT NULL,"
                                          " status TEXT NOT NULL, seen INTEGER NOT NULL DEFAULT 0)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO conversations VALUES (1, 'self', 'Note to Self', '', 0, 0)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO messages VALUES (1, 1, '', 'old note', 0, 'sent', 1)")));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("old"));
        ChatStore store(file);
        const auto notes = store.messages(1);
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.first().body, QStringLiteral("old note"));
        QVERIFY(notes.first().uid.isEmpty());
        QVERIFY(store.addMessage(1, QStringLiteral("new"), QString(), QDateTime::currentDateTimeUtc(),
                                 QStringLiteral("u1")).id > 0);
        QVERIFY(store.hasMessage(1, QStringLiteral("u1")));
        QVERIFY(!store.conversation(1).request); // old conversations: accepted
    }

    void requestsAndBlocked()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/requests.sqlite"));
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("anna"),
                                                  QStringLiteral("anna"), true);
        store.addMessage(anna, QStringLiteral("hi"), QStringLiteral("anna"));
        QVERIFY(store.conversation(anna).request);
        QSignalSpy changed(&store, &ChatStore::conversationsChanged);
        store.setAccepted(anna);
        QVERIFY(!store.conversation(anna).request);
        QCOMPARE(changed.size(), 1);
        store.setAccepted(anna); // already: nothing
        QCOMPARE(changed.size(), 1);

        store.deleteConversation(anna);
        QCOMPARE(store.directConversation(QStringLiteral("anna")), 0);
        QVERIFY(store.messages(anna).isEmpty());
        store.deleteConversation(store.noteToSelf()); // never Note to Self
        QVERIFY(store.conversation(store.noteToSelf()).id != 0);

        store.block(QStringLiteral("spammer"));
        store.block(QStringLiteral("spammer"));
        store.block(QStringLiteral("another"));
        QVERIFY(store.isBlocked(QStringLiteral("spammer")));
        QCOMPARE(store.blocked(), (QStringList{QStringLiteral("another"), QStringLiteral("spammer")}));
        store.unblock(QStringLiteral("spammer"));
        QVERIFY(!store.isBlocked(QStringLiteral("spammer")));
    }

    void encrypted()
    {
        const QString file = TestSupport::home() + QStringLiteral("/secret.sqlite");
        const QByteArray key = Crypto::randomBytes(32);
        QByteArray identitySecret;
        {
            ChatStore store(file, key);
            QVERIFY2(store.isOpen(), qPrintable(store.error()));
            QVERIFY(store.isEncrypted());
            // SQLCipher locks and wipes its memory (the pages read, the key).
            QSqlQuery memory(store.database());
            QVERIFY(memory.exec(QStringLiteral("PRAGMA cipher_memory_security")));
            QVERIFY(memory.next());
            QCOMPARE(memory.value(0).toInt(), 1);
            store.addMessage(store.noteToSelf(), QStringLiteral("the secret text"));
            KeyStore keys(&store);
            const KeyStore::Account account{QStringLiteral("alice"), 1, QUrl(QStringLiteral("wss://x")),
                                            Crypto::IdentityKeyPair::generate(), true};
            QVERIFY(keys.saveAccount(account));
            identitySecret = account.identity.secretKey();
        }
        // On disk: no SQLite header, no text, no key.
        QVERIFY(!CipherSql::isPlainSqlite(file));
        QFile raw(file);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        const QByteArray bytes = raw.readAll();
        QVERIFY(!bytes.contains("the secret text"));
        QVERIFY(!bytes.contains("Note to Self"));
        QVERIFY(!bytes.contains(identitySecret));
        QVERIFY(!bytes.contains("CREATE TABLE"));

        // The key opens it again; another key, or none, does not.
        {
            ChatStore again(file, key);
            QVERIFY(again.isOpen());
            QCOMPARE(again.messages(again.noteToSelf()).first().body, QStringLiteral("the secret text"));
            KeyStore keys(&again);
            QCOMPARE(keys.account()->identity.secretKey(), identitySecret);
        }
        QVERIFY(!ChatStore(file, Crypto::randomBytes(32)).isOpen());
        QVERIFY(!ChatStore(file).isOpen());
        QVERIFY(!ChatStore(file, QByteArray("short")).isOpen());
        // A failed opening leaves the file as it was.
        QVERIFY(ChatStore(file, key).isOpen());
        QVERIFY(TestSupport::onlyMine(file));
    }

    void plainHistoryIsEncryptedInPlace()
    {
        const QString file = TestSupport::home() + QStringLiteral("/before.sqlite");
        {
            ChatStore plain(file); // as before the encryption
            QVERIFY(!plain.isEncrypted());
            plain.addMessage(plain.noteToSelf(), QStringLiteral("written before"));
        }
        QVERIFY(CipherSql::isPlainSqlite(file));
        const QByteArray key = Crypto::randomBytes(32);
        {
            ChatStore store(file, key);
            QVERIFY2(store.isOpen(), qPrintable(store.error()));
            QVERIFY(store.isEncrypted());
            QCOMPARE(store.messages(store.noteToSelf()).first().body, QStringLiteral("written before"));
            store.addMessage(store.noteToSelf(), QStringLiteral("written after"));
        }
        QVERIFY(!CipherSql::isPlainSqlite(file));
        QFile raw(file);
        QVERIFY(raw.open(QIODevice::ReadOnly));
        QVERIFY(!raw.readAll().contains("written before"));
        QVERIFY(!QFile::exists(file + QStringLiteral(".encrypting")));
        ChatStore again(file, key);
        QCOMPARE(again.messages(again.noteToSelf()).size(), 2);
    }

    // Mute, archive and pin, as Signal's.
    // The app lock: an Argon2id hash only, wrong tries slowed, kept across restarts.
    void appLock()
    {
        const QString file = TestSupport::home() + QStringLiteral("/lock.sqlite");
        qint64 clock = 1'000'000;
        {
            ChatStore store(file);
            AppLock lock(&store);
            lock.setClock([&clock] { return clock; });
            QVERIFY(!lock.isSet());
            QVERIFY(lock.check(QStringLiteral("anything"))); // no lock: nothing to check
            QString error;
            QVERIFY(!lock.set(QString(), QStringLiteral("short"), 5, &error));
            QVERIFY(error.contains(QString::number(AppLock::MinLength)));
            QVERIFY(lock.set(QString(), QStringLiteral("correct horse"), 5));
            QVERIFY(lock.isSet());
            QCOMPARE(lock.idleMinutes(), 5);
            // Kept as a hash, never as it is.
            const QString hash = store.appLock().hash;
            QVERIFY(hash.startsWith(QStringLiteral("$argon2id$")));
            QVERIFY(!hash.contains(QStringLiteral("correct horse")));
            QVERIFY(Crypto::verifyPassphrase(hash, QStringLiteral("correct horse")));
            QVERIFY(!Crypto::verifyPassphrase(hash, QStringLiteral("correct horsE")));
            QVERIFY(!Crypto::verifyPassphrase(QStringLiteral("rubbish"), QStringLiteral("correct horse")));
            QVERIFY(Crypto::hashPassphrase(QStringLiteral("correct horse")) != hash); // salted

            QVERIFY(lock.check(QStringLiteral("correct horse")));
            // Wrong ones: four free, the fifth makes the next one wait.
            for (int i = 1; i < AppLock::MaxFreeTries; ++i) {
                QVERIFY(!lock.check(QStringLiteral("wrong"), &error));
                QCOMPARE(error, QStringLiteral("Wrong passphrase."));
                QCOMPARE(lock.waitMs(), 0);
            }
            QVERIFY(!lock.check(QStringLiteral("wrong"), &error));
            QCOMPARE(lock.waitMs(), AppLock::FirstWaitMs);
            QVERIFY(error.contains(QStringLiteral("30")));
            // Even the right one waits.
            QVERIFY(!lock.check(QStringLiteral("correct horse"), &error));
            QVERIFY(error.contains(QStringLiteral("wait")));
        }
        {
            // A restart forgets nothing.
            ChatStore store(file);
            AppLock lock(&store);
            lock.setClock([&clock] { return clock; });
            QCOMPARE(lock.waitMs(), AppLock::FirstWaitMs);
            clock += AppLock::FirstWaitMs;
            QVERIFY(!lock.check(QStringLiteral("wrong")));
            QCOMPARE(lock.waitMs(), 2 * AppLock::FirstWaitMs); // doubling
            for (int i = 0; i < 10; ++i) {
                clock += lock.waitMs();
                QVERIFY(!lock.check(QStringLiteral("wrong")));
            }
            QCOMPARE(lock.waitMs(), AppLock::MaxWaitMs); // at most
            clock += lock.waitMs();
            QVERIFY(lock.check(QStringLiteral("correct horse")));
            QCOMPARE(store.appLock().failures, 0);

            // Changing it, the idle time, turning it off: only with it.
            QString error;
            QVERIFY(!lock.set(QStringLiteral("wrong"), QStringLiteral("new passphrase"), 1, &error));
            QVERIFY(lock.set(QStringLiteral("correct horse"), QStringLiteral("new passphrase"), 1));
            QVERIFY(!lock.check(QStringLiteral("correct horse")));
            QVERIFY(lock.check(QStringLiteral("new passphrase")));
            QVERIFY(lock.setIdleMinutes(QStringLiteral("new passphrase"), 0));
            QCOMPARE(lock.idleMinutes(), 0);
            QVERIFY(!lock.turnOff(QStringLiteral("wrong")));
            QVERIFY(lock.isSet());
            QVERIFY(lock.turnOff(QStringLiteral("new passphrase")));
            QVERIFY(!lock.isSet());
            QVERIFY(lock.set(QString(), QStringLiteral("again again"), 5));
            lock.forget(); // --forget-app-lock
            QVERIFY(!lock.isSet());
        }
    }

    void muteArchivePin()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/pins.sqlite"));
        const QDateTime t0 = QDateTime::currentDateTimeUtc().addSecs(-600);
        QList<qint64> ids;
        for (int i = 0; i < 6; ++i) {
            const QString name = QStringLiteral("p%1").arg(i);
            ids << store.addConversation(QStringLiteral("direct"), name, name);
            store.addMessage(ids.last(), QStringLiteral("hi"), QString(), t0.addSecs(i)); // p5 the newest
        }
        auto order = [&] {
            QStringList out;
            for (const ChatStore::Conversation &c : store.conversations())
                out << c.peer;
            return out.join(QLatin1Char(' '));
        };
        QCOMPARE(order(), QStringLiteral("p5 p4 p3 p2 p1 p0"));
        // Pinned on top, in the order pinned; four at most.
        QVERIFY(store.setPinned(ids[1], true));
        QVERIFY(store.setPinned(ids[3], true));
        QCOMPARE(order(), QStringLiteral("p1 p3 p5 p4 p2 p0"));
        QVERIFY(store.setPinned(ids[0], true));
        QVERIFY(store.setPinned(ids[2], true));
        QVERIFY(!store.setPinned(ids[4], true));
        QCOMPARE(store.conversation(ids[4]).pinned, 0);
        QVERIFY(store.setPinned(ids[3], false));
        QCOMPARE(order(), QStringLiteral("p1 p0 p2 p5 p4 p3"));

        // Archiving unpins; pinning brings it back.
        store.setArchived(ids[1], true);
        QVERIFY(store.conversation(ids[1]).archived);
        QCOMPARE(store.conversation(ids[1]).pinned, 0);
        QVERIFY(store.setPinned(ids[1], true));
        QVERIFY(!store.conversation(ids[1]).archived);

        // Their message brings an archived one back; mine doesn't; muted, it stays.
        store.setArchived(ids[5], true);
        store.addMessage(ids[5], QStringLiteral("mine"));
        QVERIFY(store.conversation(ids[5]).archived);
        store.addMessage(ids[5], QStringLiteral("theirs"), QStringLiteral("p5"));
        QVERIFY(!store.conversation(ids[5]).archived);
        store.setArchived(ids[5], true);
        store.setMutedUntil(ids[5], ChatStore::MutedForever);
        QVERIFY(store.conversation(ids[5]).isMuted());
        store.addMessage(ids[5], QStringLiteral("again"), QStringLiteral("p5"));
        QVERIFY(store.conversation(ids[5]).archived);
        // A mute that has run out is none.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        store.setMutedUntil(ids[4], now + 3600 * 1000);
        QVERIFY(store.conversation(ids[4]).isMuted(now));
        QVERIFY(!store.conversation(ids[4]).isMuted(now + 3601 * 1000));
        store.setMutedUntil(ids[4], 0);
        QVERIFY(!store.conversation(ids[4]).isMuted());
    }

    void findMessages()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/find.sqlite"));
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("anna"), QStringLiteral("anna"));
        const QDateTime t = QDateTime::currentDateTimeUtc();
        const qint64 a = store.addMessage(anna, QStringLiteral("Καλημέρα! Coffee at the Harbour?"), QStringLiteral("anna"), t).id;
        const qint64 b = store.addMessage(anna, QStringLiteral("harbour it is"), QString(), t.addSecs(1)).id;
        store.addMessage(anna, QStringLiteral("see you"), QStringLiteral("anna"), t.addSecs(2));
        const qint64 gone = store.addMessage(anna, QStringLiteral("harbour (deleted)"), QString(), t.addSecs(3)).id;
        store.markDeleted(gone);
        store.addSystemNote(anna, QStringLiteral("harbour note"), t.addSecs(4));
        QCOMPARE(store.findMessages(anna, QStringLiteral("HARBOUR")), (QList<qint64>{a, b})); // oldest first
        QCOMPARE(store.findMessages(anna, QStringLiteral("καλημέρα")), QList<qint64>{a});     // Greek, any case
        QCOMPARE(store.findMessages(anna, QStringLiteral("  coffee   at ")), QList<qint64>{a});
        QVERIFY(store.findMessages(anna, QStringLiteral("tea")).isEmpty());
        QVERIFY(store.findMessages(anna, QStringLiteral("  ")).isEmpty());
        QVERIFY(store.findMessages(store.noteToSelf(), QStringLiteral("harbour")).isEmpty());
        QCOMPARE(store.messageCount(anna), 5);
    }

    void defaultPlace()
    {
        QVERIFY(ChatStore::defaultFile().endsWith(QStringLiteral("/argonaut-chat-tests/chat.sqlite")));
#ifndef Q_OS_WIN // there, it is the system's AppData folder (Qt's test one)
        QVERIFY(ChatStore::defaultFile().startsWith(TestSupport::home()));
#endif
    }
};

CHAT_TEST_MAIN(TestStore, QCoreApplication)
#include "tst_store.moc"
