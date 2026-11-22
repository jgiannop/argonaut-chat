#include "Attachment.h"
#include "ChatStore.h"
#include "History.h"
#include "Protocol.h"
#include "TestSupport.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QTimeZone>
#include <QtEndian>

namespace {

QByteArray frame(char kind, const QByteArray &payload)
{
    QByteArray out(5, Qt::Uninitialized);
    out[0] = kind;
    qToBigEndian(quint32(payload.size()), out.data() + 1);
    return out + payload;
}

QByteArray json(const QJsonObject &o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// A stream by hand: a direct conversation with anna, then `rest`.
QByteArray stream(const QByteArray &rest)
{
    return QByteArray("ACHIST1\n")
           + frame('C', json({{"n", 0}, {"kind", "direct"}, {"peer", "anna"}})) + rest;
}

QJsonObject goodMessage()
{
    return {{"c", 0}, {"uid", Protocol::newMessageId()}, {"author", "anna"}, {"body", "hi"},
            {"at", double(QDateTime::currentMSecsSinceEpoch() - 1000)}, {"status", "received"}};
}

QByteArray end(int messages, int files)
{
    return frame('E', json({{"messages", messages}, {"files", files}}));
}

// Fed in one go: did the reader take it all, and end well?
bool reads(ChatStore &store, const QByteArray &bytes)
{
    History::Reader reader(&store, QStringLiteral("myself"), QDateTime::currentMSecsSinceEpoch());
    return reader.feed(bytes) && reader.finish();
}

QDateTime ago(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(QDateTime::currentMSecsSinceEpoch() - ms, QTimeZone::utc());
}

} // namespace

class TestHistory : public QObject
{
    Q_OBJECT

private slots:
    // The last year's messages, with replies, mentions, edits, reactions and
    // files, from one computer's store to another's; nothing twice.
    void roundTrip()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 hour = 3600 * 1000;
        ChatStore old(TestSupport::home() + QStringLiteral("/old/chat.sqlite"));
        const qint64 notes = old.noteToSelf();
        old.addMessage(notes, QStringLiteral("a note"), QString(), ago(5 * hour), Protocol::newMessageId());
        const qint64 anna = old.addConversation(QStringLiteral("direct"), QStringLiteral("anna"), QStringLiteral("anna"));
        old.setAccepted(anna);
        const QString hiUid = Protocol::newMessageId();
        const ChatStore::Message hi = old.addMessage(anna, QStringLiteral("hi"), QStringLiteral("anna"), ago(4 * hour), hiUid);
        const ChatStore::Message reply =
            old.addMessage(anna, QStringLiteral("hello"), QString(), ago(3 * hour), Protocol::newMessageId(), QStringLiteral("read"));
        old.setQuote(reply.id, hiUid, QStringLiteral("anna"), QStringLiteral("hi"));
        old.editMessage(reply.id, QStringLiteral("hello there"), now - 2 * hour);
        old.setReaction(reply.id, QStringLiteral("anna"), QStringLiteral("👍"), now - hour);
        old.setReaction(hi.id, QString(), QStringLiteral("😂"), now - hour);
        // A picture.
        const QByteArray picture = QByteArray("PNG picture bytes ").repeated(100);
        const ChatStore::Message photo =
            old.addMessage(anna, QString(), QStringLiteral("anna"), ago(2 * hour), Protocol::newMessageId());
        const auto encrypted = Attachment::encrypt(picture);
        ChatStore::StoredAttachment a;
        a.message = photo.id;
        a.name = QStringLiteral("photo.png");
        a.type = QStringLiteral("image/png");
        a.size = picture.size();
        a.key = encrypted->key;
        a.digest = encrypted->digest;
        a.file = old.writeBlob(encrypted->blob);
        a.state = QStringLiteral("ready");
        QVERIFY(old.addAttachment(a) > 0);
        // Left behind: older than a year, disappearing, deleted, a request's, one not sent yet.
        old.addMessage(anna, QStringLiteral("long ago"), QStringLiteral("anna"), ago(400LL * 24 * hour), Protocol::newMessageId());
        const ChatStore::Message vanishing =
            old.addMessage(anna, QStringLiteral("vanishing"), QString(), ago(hour), Protocol::newMessageId());
        old.setMessageExpiry(vanishing.id, 60, 0);
        const ChatStore::Message gone = old.addMessage(anna, QStringLiteral("gone"), QString(), ago(hour), Protocol::newMessageId());
        old.markDeleted(gone.id);
        old.addMessage(anna, QStringLiteral("unsent"), QString(), ago(hour), Protocol::newMessageId(), QStringLiteral("sending"));
        const qint64 spam = old.addConversation(QStringLiteral("direct"), QStringLiteral("spammer"), QStringLiteral("spammer"), true);
        old.addMessage(spam, QStringLiteral("buy now"), QStringLiteral("spammer"), ago(hour), Protocol::newMessageId());
        // A group, with me mentioned.
        const QString groupId = QString(32, QLatin1Char('a'));
        const qint64 group = old.addGroup({0, groupId, QStringLiteral("Friends"), QStringLiteral("anna"),
                                           {QStringLiteral("anna"), QStringLiteral("myself"), QStringLiteral("bob")}, 1},
                                          false);
        const ChatStore::Message mention =
            old.addMessage(group, QStringLiteral("@Me look"), QStringLiteral("bob"), ago(hour), Protocol::newMessageId());
        old.setMentions(mention.id, {{0, 3, QStringLiteral("myself")}}, true);

        History::Writer writer(&old, now);
        QList<QByteArray> pieces;
        for (QByteArray piece = writer.next(); !piece.isEmpty(); piece = writer.next()) {
            QVERIFY(piece.size() <= History::PieceBytes);
            pieces << piece;
        }
        QCOMPARE(writer.messagesWritten(), 5); // the note, hi, hello, the photo, the group's (none of the rest)
        QCOMPARE(writer.filesWritten(), 1);
        QCOMPARE(writer.progress(), 1.0);
        QVERIFY(!pieces.first().contains("buy now"));
        QVERIFY(!pieces.first().contains("long ago"));

        // The new computer: in the group already (the link said so), and
        // "hi" came there since the link.
        ChatStore fresh(TestSupport::home() + QStringLiteral("/new/chat.sqlite"));
        const qint64 newGroup = fresh.addGroup({0, groupId, QStringLiteral("Friends"), QStringLiteral("anna"),
                                                {QStringLiteral("anna"), QStringLiteral("myself"), QStringLiteral("bob")}, 1},
                                               false);
        const qint64 newAnna = fresh.addConversation(QStringLiteral("direct"), QStringLiteral("anna"), QStringLiteral("anna"));
        fresh.setAccepted(newAnna);
        fresh.addMessage(newAnna, QStringLiteral("hi"), QStringLiteral("anna"), ago(4 * hour), hiUid);
        QSignalSpy changed(&fresh, &ChatStore::messagesChanged);
        History::Reader reader(&fresh, QStringLiteral("myself"), now);
        // In two halves, cut anywhere: frames run across pieces.
        const QByteArray all = pieces.join();
        QVERIFY(reader.feed(all.left(all.size() / 3)));
        QVERIFY(!reader.finish());
        QVERIFY(reader.feed(all.mid(all.size() / 3)));
        QVERIFY(reader.finish());
        QCOMPARE(reader.messagesTaken(), 4); // not "hi" again
        QCOMPARE(reader.filesTaken(), 1);
        QVERIFY(changed.size() <= 4); // once per conversation per piece, not per message

        const QList<ChatStore::Message> withAnna = fresh.messages(newAnna);
        QCOMPARE(withAnna.size(), 3);
        QCOMPARE(withAnna.at(0).body, QStringLiteral("hi"));
        const ChatStore::Message hello = withAnna.at(1);
        QCOMPARE(hello.body, QStringLiteral("hello there"));
        QCOMPARE(hello.status, QStringLiteral("read"));
        QVERIFY(hello.outgoing());
        QCOMPARE(hello.quoteAuthor, QStringLiteral("anna"));
        QCOMPARE(hello.editedAt, now - 2 * hour);
        QCOMPARE(fresh.reactions(hello.id).size(), 1);
        QCOMPARE(fresh.reactions(hello.id).first().emoji, QStringLiteral("👍"));
        const QList<ChatStore::StoredAttachment> files = fresh.attachments(withAnna.at(2).id);
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.first().name, QStringLiteral("photo.png"));
        QCOMPARE(files.first().key == a.key, false); // a key of its own
        QCOMPARE(Attachment::decrypt(fresh.readBlob(files.first().file), files.first().key, files.first().digest,
                                     files.first().size)
                     .value_or(QByteArray()),
                 picture);
        QCOMPARE(fresh.messages(fresh.noteToSelf()).size(), 1);
        const QList<ChatStore::Message> inGroup = fresh.messages(newGroup);
        QCOMPARE(inGroup.size(), 1);
        QVERIFY(inGroup.first().mentionsMe);
        QVERIFY(fresh.unseenUids(newGroup).isEmpty()); // history, not news
        QVERIFY(fresh.directConversation(QStringLiteral("spammer")) == 0);

        // Again: nothing twice.
        History::Reader again(&fresh, QStringLiteral("myself"), now);
        QVERIFY(again.feed(all));
        QVERIFY(again.finish());
        QCOMPARE(again.messagesTaken(), 0);
        QCOMPARE(fresh.messages(newAnna).size(), 3);
    }

    // A conversation with someone not here yet is made; one with a blocked
    // person, or a group this computer is not in, is left out.
    void conversationsHere()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/conversations.sqlite"));
        QVERIFY(reads(store, stream(frame('M', json(goodMessage())) + end(1, 0))));
        const qint64 anna = store.directConversation(QStringLiteral("anna"));
        QVERIFY(anna > 0);
        QVERIFY(!store.conversation(anna).request);
        QCOMPARE(store.messages(anna).size(), 1);

        ChatStore blocking(TestSupport::home() + QStringLiteral("/blocking.sqlite"));
        blocking.block(QStringLiteral("anna"));
        QVERIFY(reads(blocking, stream(frame('M', json(goodMessage())) + end(1, 0))));
        QCOMPARE(blocking.directConversation(QStringLiteral("anna")), 0);

        ChatStore noGroup(TestSupport::home() + QStringLiteral("/nogroup.sqlite"));
        QJsonObject inGroup = goodMessage();
        const QByteArray groupStream = QByteArray("ACHIST1\n")
                                       + frame('C', json({{"n", 0}, {"kind", "group"}, {"peer", QString(32, 'b')}}))
                                       + frame('M', json(inGroup)) + end(1, 0);
        QVERIFY(reads(noGroup, groupStream));
        QCOMPARE(noGroup.groupConversation(QString(32, 'b')), 0);
    }

    // What comes is a stranger's: anything out of shape breaks the stream.
    void strangersStreams()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/strangers.sqlite"));
        auto bad = [&](const QByteArray &bytes) {
            History::Reader reader(&store, QStringLiteral("myself"), QDateTime::currentMSecsSinceEpoch());
            return !reader.feed(bytes) || !reader.finish();
        };
        auto with = [](const QString &key, const QJsonValue &value) {
            QJsonObject m = goodMessage();
            if (value.isUndefined())
                m.remove(key);
            else
                m.insert(key, value);
            return m;
        };
        QVERIFY(reads(store, stream(frame('M', json(goodMessage())) + end(1, 0))));
        QVERIFY(bad("ACHIST2\n"));
        QVERIFY(bad(stream(end(0, 0) + frame('M', json(goodMessage()))))); // after the end
        QVERIFY(bad(stream(QByteArray())));                                 // no end
        QVERIFY(bad(stream(end(1, 0))));                                    // counts wrong
        QVERIFY(bad(stream(frame('X', "{}") + end(0, 0))));
        QVERIFY(bad(stream(frame('F', "bytes") + end(0, 1))));              // a file of no message
        QVERIFY(bad(stream(frame('M', json(goodMessage())) + frame('C', json({{"n", 1}, {"kind", "self"}, {"peer", ""}}))
                           + end(1, 0)))); // a conversation after messages
        QVERIFY(bad(stream(frame('M', "not json") + end(1, 0))));
        QByteArray huge(5, Qt::Uninitialized);
        huge[0] = 'M';
        qToBigEndian(quint32(64 * 1024 * 1024), huge.data() + 1);
        QVERIFY(bad(stream(huge)));
        QVERIFY(bad(stream(frame('M', json(with("c", 1))) + end(1, 0))));          // no such conversation
        QVERIFY(bad(stream(frame('M', json(with("author", "bob"))) + end(1, 0))));  // not the person of it
        QVERIFY(bad(stream(frame('M', json(with("author", "myself"))) + end(1, 0))));   // me, by name
        QVERIFY(bad(stream(frame('M', json(with("status", "read"))) + end(1, 0))));  // theirs "read"
        QVERIFY(bad(stream(frame('M', json(with("uid", "x"))) + end(1, 0))));
        QVERIFY(bad(stream(frame('M', json(with("at", 1.0))) + end(1, 0))));        // 1970
        QVERIFY(bad(stream(frame('M', json(with("at", double(QDateTime::currentMSecsSinceEpoch()) + 3 * 86400e3))) + end(1, 0))));
        QVERIFY(bad(stream(frame('M', json(with("body", ""))) + end(1, 0))));       // nothing to show
        QVERIFY(bad(stream(frame('M', json(with("extra", 1))) + end(1, 0))));
        QVERIFY(bad(stream(frame('M', json(with("mentions", QJsonArray{QJsonArray{0, 2, "anna"}}))) + end(1, 0)))); // not a group
        QVERIFY(bad(stream(frame('M', json(with("reactions", QJsonArray{QJsonObject{{"author", "anna"}, {"emoji", "abc"}, {"at", 1}}})))
                           + end(1, 0))));
        QVERIFY(bad(stream(frame('M', json(with("quote", QJsonObject{{"uid", "x"}, {"author", "anna"}, {"text", ""}}))) + end(1, 0))));
        // Files: as many as it says, of the size it says.
        const QJsonArray oneFile{QJsonObject{{"name", "a.txt"}, {"type", "text/plain"}, {"size", 3}}};
        QVERIFY(reads(store, stream(frame('M', json(with("files", oneFile))) + frame('F', "abc") + end(1, 1))));
        QVERIFY(bad(stream(frame('M', json(with("files", oneFile))) + end(1, 0))));
        QVERIFY(bad(stream(frame('M', json(with("files", oneFile))) + frame('F', "abcd") + end(1, 1))));
        QVERIFY(bad(stream(frame('M', json(with("files", QJsonArray{QJsonObject{{"name", "a"}, {"type", "text/plain"},
                                                                                 {"size", 3}, {"voice", true}}})))
                           + frame('F', "abc") + end(1, 1)))); // a "voice" text file
        // Whatever passed before a break stays; whatever comes after is refused.
        History::Reader reader(&store, QStringLiteral("myself"), QDateTime::currentMSecsSinceEpoch());
        QVERIFY(!reader.feed(stream(frame('M', json(goodMessage())) + frame('X', "{}"))));
        QCOMPARE(reader.messagesTaken(), 1);
        QVERIFY(!reader.feed(end(1, 0)));
        QVERIFY(!reader.finish());
    }
};

CHAT_TEST_MAIN(TestHistory, QCoreApplication)
#include "tst_history.moc"
