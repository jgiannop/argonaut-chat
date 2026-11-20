#include "Protocol.h"
#include "Session.h"
#include "TestSupport.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace Protocol;

namespace {

// Bob's keys, as his device keeps them.
struct Bob
{
    Crypto::IdentityKeyPair identity = Crypto::IdentityKeyPair::generate();
    X3dh::SignedPreKey signedPreKey = X3dh::makeSignedPreKey(identity, 1);
    QList<X3dh::OneTimePreKey> oneTime = X3dh::makeOneTimePreKeys(10, 3);

    X3dh::PreKeyBundle bundle(bool withOneTime = true) const
    {
        X3dh::PreKeyBundle b{identity.publicKey(), signedPreKey.id, signedPreKey.pair.publicKey,
                             signedPreKey.signature, {}, {}};
        if (withOneTime) {
            b.oneTimePreKeyId = oneTime.first().id;
            b.oneTimePreKey = oneTime.first().pair.publicKey;
        }
        return b;
    }
    Session::SignedPreKeyLookup signedLookup() const
    {
        return [this](quint32 id) -> std::optional<X3dh::SignedPreKey> {
            if (id == signedPreKey.id)
                return signedPreKey;
            return std::nullopt;
        };
    }
    Session::OneTimePreKeyLookup oneTimeLookup() const
    {
        return [this](quint32 id) -> std::optional<X3dh::OneTimePreKey> {
            for (const auto &key : oneTime) {
                if (key.id == id)
                    return key;
            }
            return std::nullopt;
        };
    }
    void forget(quint32 id)
    {
        oneTime.erase(std::remove_if(oneTime.begin(), oneTime.end(), [id](const auto &k) { return k.id == id; }),
                      oneTime.end());
    }
};

QByteArray decryptedBy(Session &session, const QByteArray &wire)
{
    const auto message = decodeCipherMessage(wire);
    if (!message)
        return "<bad message>";
    return session.decrypt(*message).value_or("<failed>");
}

} // namespace

class TestProtocol : public QObject
{
    Q_OBJECT

private slots:
    void usernames()
    {
        QCOMPARE(normalizeUsername("  Alice.Smith_2 "), QString("alice.smith_2"));
        QVERIFY(isValidUsername("bob"));
        for (const char *bad : {"", "ab", "2bob", "_bob", "bob.", "bo..b", "bob smith", "bob-smith", "bob@x",
                                "abcdefghijklmnopqrstuvwxyz1234567"})
            QVERIFY2(normalizeUsername(bad).isEmpty(), bad);
        // Look-alike letters from other alphabets: the Cyrillic "а".
        QVERIFY(normalizeUsername(QString::fromUtf8("b\xd0\xb0rt")).isEmpty());
        QVERIFY(!isValidUsername("Bob")); // not normalized
    }

    void padding()
    {
        for (int size : {0, 1, 158, 159, 160, 161, 1000}) {
            const QByteArray data(size, 'x');
            const QByteArray padded = pad(data);
            QCOMPARE(padded.size() % PaddingBlock, 0);
            QVERIFY(padded.size() > size);
            QCOMPARE(unpad(padded), std::optional<QByteArray>(data));
        }
        QVERIFY(!unpad(QByteArray()));
        QVERIFY(!unpad(QByteArray(160, '\0')));
        QVERIFY(!unpad(QByteArray(159, 'x') + char(0x80) + 'x'));
        // Short and long-ish texts look the same size.
        QCOMPARE(encodeContent(Content::text("hi")).size(), encodeContent(Content::text("hello there")).size());
    }

    void content()
    {
        Content text = Content::text(QString::fromUtf8("Γειά σου, Bob! 👋\n2nd line"));
        QVERIFY(isValidMessageId(text.id));
        auto back = decodeContent(encodeContent(text));
        QVERIFY(back);
        QCOMPARE(back->type, Content::Type::Text);
        QCOMPARE(back->id, text.id);
        QCOMPARE(back->sentAt, text.sentAt);
        QCOMPARE(back->body, text.body);
        QVERIFY(decodeContent(encodeContent(Content::text(QString())))); // empty text

        const QStringList ids{newMessageId(), newMessageId()};
        back = decodeContent(encodeContent(Content::receiptOf(Content::Receipt::Read, ids)));
        QVERIFY(back);
        QCOMPARE(back->type, Content::Type::Receipt);
        QCOMPARE(back->receipt, Content::Receipt::Read);
        QCOMPARE(back->receiptFor, ids);

        // What cannot be sent.
        QVERIFY(encodeContent(Content::text(QString(MaxTextBytes + 1, 'x'))).isEmpty());
        QVERIFY(encodeContent(Content::receiptOf(Content::Receipt::Read, {})).isEmpty());
        QVERIFY(encodeContent(Content::receiptOf(Content::Receipt::Read, {"not an id"})).isEmpty());
        Content noId = Content::text("x");
        noId.id.clear();
        QVERIFY(encodeContent(noId).isEmpty());
    }

    void profiles()
    {
        const Attachment::Pointer picture{"0123456789abcdef0123456789abcdef", QByteArray(32, 'k'), QByteArray(32, 'd'),
                                          4000, "avatar", "image/png"};
        auto back = decodeContent(encodeContent(Content::profile("Anna K.", picture)));
        QVERIFY(back);
        QCOMPARE(back->type, Content::Type::Profile);
        QCOMPARE(back->name, QString("Anna K."));
        QCOMPARE(back->avatar->id, picture.id);
        back = decodeContent(encodeContent(Content::profile(QString(), std::nullopt)));
        QVERIFY(back && back->name.isEmpty() && !back->avatar);
        // Names: cleaned, and no longer than 64.
        QCOMPARE(cleanDisplayName(QString("  Anna\t\nK.  ")), QString("Anna K."));
        QCOMPARE(cleanDisplayName(QString(100, 'x')).size(), MaxDisplayName);
        QCOMPARE(cleanDisplayName(QStringLiteral("Bank") + QChar(0x202E) + QStringLiteral("evil")), QString("Bankevil"));
        // From strangers: a picture that is not a picture, a huge name, a name not a string.
        const QByteArray id = newMessageId().toLatin1();
        const QByteArray head = "{\"v\":1,\"t\":\"profile\",\"id\":\"" + id + "\",\"sent\":1,";
        Attachment::Pointer notPicture = picture;
        notPicture.type = "application/x-sh";
        QVERIFY(!decodeContent(pad(head + "\"name\":\"x\",\"avatar\":"
                                   + QJsonDocument(Attachment::toJson(notPicture)).toJson(QJsonDocument::Compact) + "}")));
        QVERIFY(!decodeContent(pad(head + "\"name\":\"" + QByteArray(300, 'x') + "\"}")));
        QVERIFY(!decodeContent(pad(head + "\"name\":5}")));
        // What arrives is cleaned too.
        back = decodeContent(pad(head + "\"name\":\"  Mal\\u202elory \"}"));
        QCOMPARE(back->name, QString("Mallory"));
    }

    void groups()
    {
        const QString gid = newGroupId();
        QVERIFY(isValidGroupId(gid));
        auto back = decodeContent(encodeContent(Content::groupUpdate(gid, "Trip", "alice", {"alice", "bob"}, 3)));
        QVERIFY(back);
        QCOMPARE(back->type, Content::Type::Group);
        QCOMPARE(back->group, gid);
        QCOMPARE(back->groupName, QString("Trip"));
        QCOMPARE(back->groupAdmin, QString("alice"));
        QCOMPARE(back->groupMembers, (QStringList{"alice", "bob"}));
        QCOMPARE(back->groupRevision, 3);
        QCOMPARE(decodeContent(encodeContent(Content::leave(gid)))->type, Content::Type::Leave);
        Content text = Content::text("hi all");
        text.group = gid;
        QCOMPARE(decodeContent(encodeContent(text))->group, gid);

        // Not sent: an admin not in it, twice the same member, too many, no revision, a bad id.
        QVERIFY(encodeContent(Content::groupUpdate(gid, "x", "carol", {"alice", "bob"}, 1)).isEmpty());
        QVERIFY(encodeContent(Content::groupUpdate(gid, "x", "alice", {"alice", "alice"}, 1)).isEmpty());
        QStringList crowd{"alice"};
        for (int i = 0; i < MaxGroupMembers; ++i)
            crowd << QStringLiteral("p%1x").arg(i);
        QVERIFY(encodeContent(Content::groupUpdate(gid, "x", "alice", crowd, 1)).isEmpty());
        QVERIFY(encodeContent(Content::groupUpdate(gid, "x", "alice", {"alice"}, 0)).isEmpty());
        QVERIFY(encodeContent(Content::groupUpdate("nope", "x", "alice", {"alice"}, 1)).isEmpty());
        text.group = "../x";
        QVERIFY(encodeContent(text).isEmpty());
        // Not taken, from strangers, the same.
        const QByteArray head = "{\"v\":1,\"t\":\"group\",\"id\":\"" + newMessageId().toLatin1()
                                + "\",\"sent\":1,\"group\":\"" + gid.toLatin1() + "\",\"name\":\"x\",";
        QVERIFY(!decodeContent(pad(head + "\"admin\":\"carol\",\"members\":[\"alice\"],\"revision\":1}")));
        QVERIFY(!decodeContent(pad(head + "\"admin\":\"alice\",\"members\":[\"alice\",\"alice\"],\"revision\":1}")));
        QVERIFY(!decodeContent(pad(head + "\"admin\":\"alice\",\"members\":[\"Alice\"],\"revision\":1}")));
        QVERIFY(!decodeContent(pad(head + "\"admin\":\"alice\",\"members\":[\"alice\"],\"revision\":0}")));
        QVERIFY(!decodeContent(pad(head + "\"admin\":\"alice\",\"members\":[],\"revision\":1}")));
        QVERIFY(decodeContent(pad(head + "\"admin\":\"alice\",\"members\":[\"alice\"],\"revision\":1}")));
    }

    void timers()
    {
        Content text = Content::text("gone soon");
        text.expireSeconds = 3600;
        QCOMPARE(decodeContent(encodeContent(text))->expireSeconds, 3600);
        QCOMPARE(decodeContent(encodeContent(Content::text("stays")))->expireSeconds, 0);
        text.expireSeconds = MaxExpireSeconds + 1;
        QVERIFY(encodeContent(text).isEmpty());
        const QString gid = newGroupId();
        auto back = decodeContent(encodeContent(Content::timer(300, gid)));
        QCOMPARE(back->type, Content::Type::Timer);
        QCOMPARE(back->expireSeconds, 300);
        QCOMPARE(back->group, gid);
        back = decodeContent(encodeContent(Content::timer(0)));
        QCOMPARE(back->expireSeconds, 0); // off
        QVERIFY(back->group.isEmpty());
        const QByteArray head = "{\"v\":1,\"t\":\"timer\",\"id\":\"" + newMessageId().toLatin1() + "\",\"sent\":1,";
        QVERIFY(!decodeContent(pad(head + "\"expire\":-5}")));
        QVERIFY(!decodeContent(pad(head + "\"expire\":\"1h\"}")));
        QVERIFY(!decodeContent(pad(head + "\"expire\":99999999}")));
        QVERIFY(!decodeContent(pad(head + "\"expire\":5,\"group\":\"x\"}")));
    }

    void contentFromStrangers_data()
    {
        QTest::addColumn<QByteArray>("json");
        const QByteArray id = newMessageId().toLatin1();
        QTest::newRow("not json") << QByteArray("{oops");
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("no version") << "{\"t\":\"text\",\"id\":\"" + id + "\",\"sent\":1,\"body\":\"x\"}";
        QTest::newRow("later version") << "{\"v\":2,\"t\":\"text\",\"id\":\"" + id + "\",\"sent\":1,\"body\":\"x\"}";
        QTest::newRow("unknown type") << "{\"v\":1,\"t\":\"sticker\",\"id\":\"" + id + "\",\"sent\":1}";
        QTest::newRow("bad id") << QByteArray("{\"v\":1,\"t\":\"text\",\"id\":\"1\",\"sent\":1,\"body\":\"x\"}");
        QTest::newRow("upper-case id") << "{\"v\":1,\"t\":\"text\",\"id\":\"" + id.toUpper()
                                               + "\",\"sent\":1,\"body\":\"x\"}";
        QTest::newRow("negative time") << "{\"v\":1,\"t\":\"text\",\"id\":\"" + id + "\",\"sent\":-1,\"body\":\"x\"}";
        QTest::newRow("fractional time") << "{\"v\":1,\"t\":\"text\",\"id\":\"" + id
                                                 + "\",\"sent\":1.5,\"body\":\"x\"}";
        QTest::newRow("time as text") << "{\"v\":1,\"t\":\"text\",\"id\":\"" + id + "\",\"sent\":\"1\",\"body\":\"x\"}";
        QTest::newRow("body a number") << "{\"v\":1,\"t\":\"text\",\"id\":\"" + id + "\",\"sent\":1,\"body\":5}";
        QTest::newRow("no body") << "{\"v\":1,\"t\":\"text\",\"id\":\"" + id + "\",\"sent\":1}";
        QTest::newRow("receipt, bad kind") << "{\"v\":1,\"t\":\"receipt\",\"id\":\"" + id
                                                   + "\",\"sent\":1,\"kind\":\"seen\",\"for\":[\"" + id + "\"]}";
        QTest::newRow("receipt, bad ids") << "{\"v\":1,\"t\":\"receipt\",\"id\":\"" + id
                                                  + "\",\"sent\":1,\"kind\":\"read\",\"for\":[1]}";
        QTest::newRow("receipt, no ids") << "{\"v\":1,\"t\":\"receipt\",\"id\":\"" + id
                                                 + "\",\"sent\":1,\"kind\":\"read\",\"for\":[]}";
    }

    void contentFromStrangers()
    {
        QFETCH(QByteArray, json);
        QVERIFY(!decodeContent(pad(json)));
        QVERIFY(!decodeContent(json)); // not padded
    }

    void cipherMessages()
    {
        CipherMessage plain;
        plain.ratchetMessage = "ratchet bytes";
        auto back = decodeCipherMessage(encodeCipherMessage(plain));
        QVERIFY(back);
        QVERIFY(!back->preKey);
        QCOMPARE(back->ratchetMessage, plain.ratchetMessage);

        CipherMessage first;
        first.preKey = true;
        first.identityKey = QByteArray(32, 'i');
        first.baseKey = QByteArray(32, 'b');
        first.signedPreKeyId = 0x01020304;
        first.oneTimePreKeyId = 77;
        first.ratchetMessage = "r";
        back = decodeCipherMessage(encodeCipherMessage(first));
        QVERIFY(back);
        QVERIFY(back->preKey);
        QCOMPARE(back->identityKey, first.identityKey);
        QCOMPARE(back->baseKey, first.baseKey);
        QCOMPARE(back->signedPreKeyId, first.signedPreKeyId);
        QCOMPARE(back->oneTimePreKeyId, first.oneTimePreKeyId);
        QCOMPARE(back->ratchetMessage, first.ratchetMessage);
        first.oneTimePreKeyId.reset();
        QVERIFY(!decodeCipherMessage(encodeCipherMessage(first))->oneTimePreKeyId);

        // From strangers.
        QVERIFY(!decodeCipherMessage(QByteArray()));
        QVERIFY(!decodeCipherMessage(QByteArray(1, '\1')));
        QVERIFY(!decodeCipherMessage("\x03xyz"));
        QByteArray wire = encodeCipherMessage(first);
        QVERIFY(!decodeCipherMessage(wire.left(1 + 32 + 32 + 4 + 1 + 4))); // no ratchet message
        wire[1 + 64 + 4] = 2; // "has one-time prekey" neither 0 nor 1
        QVERIFY(!decodeCipherMessage(wire));
        QVERIFY(!decodeCipherMessage(QByteArray(1, '\1') + QByteArray(MaxCipherMessageBytes, 'x')));
        CipherMessage badKey = first;
        badKey.identityKey = "short";
        QVERIFY(encodeCipherMessage(badKey).isEmpty());
    }

    void bundlesAndEnvelopes()
    {
        Bob bob;
        for (bool withOneTime : {true, false}) {
            const X3dh::PreKeyBundle bundle = bob.bundle(withOneTime);
            // Through text, as over the network.
            const QByteArray wire = QJsonDocument(bundleToJson(bundle)).toJson();
            const auto back = bundleFromJson(QJsonDocument::fromJson(wire).object());
            QVERIFY(back);
            QCOMPARE(back->identityKey, bundle.identityKey);
            QCOMPARE(back->signedPreKeyId, bundle.signedPreKeyId);
            QCOMPARE(back->signedPreKey, bundle.signedPreKey);
            QCOMPARE(back->signature, bundle.signature);
            QCOMPARE(back->oneTimePreKeyId, bundle.oneTimePreKeyId);
            QCOMPARE(back->oneTimePreKey, bundle.oneTimePreKey);
        }
        QJsonObject bad = bundleToJson(bob.bundle());
        bad.insert("identityKey", "not base64!");
        QVERIFY(!bundleFromJson(bad));
        bad = bundleToJson(bob.bundle());
        bad.insert("identityKey", QString::fromLatin1(QByteArray(31, 'x').toBase64()));
        QVERIFY(!bundleFromJson(bad));
        bad = bundleToJson(bob.bundle());
        bad.insert("oneTimePreKey", QJsonObject{{"id", -1}, {"key", bad["identityKey"]}});
        QVERIFY(!bundleFromJson(bad));
        bad.insert("oneTimePreKey", 5);
        QVERIFY(!bundleFromJson(bad));
        QVERIFY(!bundleFromJson(QJsonObject()));

        const Envelope envelope{"42", "alice", 1, 1763600000000, QByteArray("\x01" "cipher", 7)};
        const auto back = envelopeFromJson(envelopeToJson(envelope));
        QVERIFY(back);
        QCOMPARE(back->id, envelope.id);
        QCOMPARE(back->from, envelope.from);
        QCOMPARE(back->fromDevice, envelope.fromDevice);
        QCOMPARE(back->serverTime, envelope.serverTime);
        QCOMPARE(back->message, envelope.message);
        for (const auto &[key, value] : QList<QPair<QString, QJsonValue>>{
                 {"id", ""}, {"id", "has space"}, {"from", "Alice"}, {"from", 3}, {"device", 0},
                 {"device", 1.5}, {"time", -5}, {"message", ""}, {"message", "@@@"}}) {
            QJsonObject json = envelopeToJson(envelope);
            json.insert(key, value);
            QVERIFY2(!envelopeFromJson(json), qPrintable(key));
        }
    }

    // Alice and Bob, through nothing but bytes.
    void sessions()
    {
        Bob bob;
        const auto aliceId = Crypto::IdentityKeyPair::generate();
        auto alice = Session::initiate(aliceId, bob.bundle());
        QVERIFY(alice);
        QVERIFY(alice->sendsPreKeyMessages());
        QCOMPARE(alice->remoteIdentityKey(), bob.identity.publicKey());

        const QByteArray first = alice->encrypt(encodeContent(Content::text("first")));
        const QByteArray second = alice->encrypt(encodeContent(Content::text("second")));
        QVERIFY(decodeCipherMessage(first)->preKey);
        QVERIFY(decodeCipherMessage(second)->preKey);

        // The second arrives first: it starts the session.
        auto accepted = Session::accept(bob.identity, *decodeCipherMessage(second), bob.signedLookup(),
                                        bob.oneTimeLookup());
        QVERIFY(accepted);
        QCOMPARE(decodeContent(accepted->plaintext)->body, QString("second"));
        QCOMPARE(accepted->usedOneTimePreKey, std::optional<quint32>(10));
        QCOMPARE(accepted->session.remoteIdentityKey(), aliceId.publicKey());
        bob.forget(10);
        Session bobSession = accepted->session;
        // The first, late, belongs to the same session.
        QCOMPARE(decodeContent(decryptedBy(bobSession, first))->body, QString("first"));
        // A start cannot be replayed: its one-time prekey is gone.
        QVERIFY(!Session::accept(bob.identity, *decodeCipherMessage(first), bob.signedLookup(),
                                 bob.oneTimeLookup()));

        // Bob answers; from then on Alice sends plain messages.
        const QByteArray reply = bobSession.encrypt(encodeContent(Content::text("hi Alice")));
        QVERIFY(!decodeCipherMessage(reply)->preKey);
        QCOMPARE(decodeContent(decryptedBy(*alice, reply))->body, QString("hi Alice"));
        QVERIFY(!alice->sendsPreKeyMessages());
        const QByteArray third = alice->encrypt(encodeContent(Content::text("third")));
        QVERIFY(!decodeCipherMessage(third)->preKey);

        // Kept and read back, then on.
        Session bobAgain = Session::deserialize(bobSession.serialize());
        Session aliceAgain = Session::deserialize(alice->serialize());
        QVERIFY(bobAgain.isValid());
        QVERIFY(aliceAgain.isValid());
        QCOMPARE(decodeContent(decryptedBy(bobAgain, third))->body, QString("third"));
        QCOMPARE(decodeContent(decryptedBy(aliceAgain, bobAgain.encrypt(encodeContent(Content::text("ok")))))->body,
                 QString("ok"));
        QVERIFY(!Session::deserialize("rubbish").isValid());
    }

    void sessionsFromStrangers()
    {
        Bob bob;
        const auto aliceId = Crypto::IdentityKeyPair::generate();
        auto alice = Session::initiate(aliceId, bob.bundle());
        const auto first = *decodeCipherMessage(alice->encrypt("hello"));

        // Unknown prekeys.
        CipherMessage unknown = first;
        unknown.signedPreKeyId = 99;
        QVERIFY(!Session::accept(bob.identity, unknown, bob.signedLookup(), bob.oneTimeLookup()));
        unknown = first;
        unknown.oneTimePreKeyId = 99;
        QVERIFY(!Session::accept(bob.identity, unknown, bob.signedLookup(), bob.oneTimeLookup()));
        // Someone else's identity key on Alice's message: X3DH gives another secret.
        CipherMessage impostor = first;
        impostor.identityKey = Crypto::IdentityKeyPair::generate().publicKey();
        QVERIFY(!Session::accept(bob.identity, impostor, bob.signedLookup(), bob.oneTimeLookup()));
        // Not a prekey message.
        CipherMessage plain = first;
        plain.preKey = false;
        QVERIFY(!Session::accept(bob.identity, plain, bob.signedLookup(), bob.oneTimeLookup()));

        auto accepted = Session::accept(bob.identity, first, bob.signedLookup(), bob.oneTimeLookup());
        QVERIFY(accepted);
        // Another start (another base key) is not this session's.
        auto other = Session::initiate(Crypto::IdentityKeyPair::generate(), bob.bundle(false));
        QVERIFY(!accepted->session.decrypt(*decodeCipherMessage(other->encrypt("x"))));
        // Without a one-time prekey it still works.
        QVERIFY(Session::accept(bob.identity, *decodeCipherMessage(other->encrypt("y")), bob.signedLookup(),
                                bob.oneTimeLookup()));
    }
};

CHAT_TEST_MAIN(TestProtocol, QCoreApplication)
#include "tst_protocol.moc"
