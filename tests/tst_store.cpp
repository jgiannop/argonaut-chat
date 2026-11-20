#include "ChatStore.h"
#include "TestSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTest>

class TestStore : public QObject
{
    Q_OBJECT

private slots:
    void noteToSelfAndMessages()
    {
        const QString file = QDir::homePath() + QStringLiteral("/chat.sqlite");
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
        QCOMPARE(QFileInfo(file).permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
    }

    void defaultPlace()
    {
        QVERIFY(ChatStore::defaultFile().endsWith(QStringLiteral("/argonaut-chat-tests/chat.sqlite")));
        QVERIFY(ChatStore::defaultFile().startsWith(QDir::homePath()));
    }
};

CHAT_TEST_MAIN(TestStore, QCoreApplication)
#include "tst_store.moc"
