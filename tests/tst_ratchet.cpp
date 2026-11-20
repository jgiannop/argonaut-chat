#include "RatchetSession.h"
#include "TestSupport.h"
#include "X3dh.h"

#include <QCoreApplication>
#include <QTest>

namespace {

struct Pair
{
    RatchetSession alice;
    RatchetSession bob;
};

// Alice and Bob through X3DH, as they will be through the server.
Pair start()
{
    const auto aliceId = Crypto::IdentityKeyPair::generate();
    const auto bobId = Crypto::IdentityKeyPair::generate();
    const X3dh::SignedPreKey signedPreKey = X3dh::makeSignedPreKey(bobId, 1);
    const X3dh::OneTimePreKey oneTime = X3dh::makeOneTimePreKeys(1, 1).first();
    const X3dh::PreKeyBundle bundle{bobId.publicKey(),   1, signedPreKey.pair.publicKey, signedPreKey.signature, 1,
                                    oneTime.pair.publicKey};
    const auto init = X3dh::initiate(aliceId, bundle);
    const auto answer = X3dh::respond(bobId, signedPreKey, &oneTime, aliceId.publicKey(), init->ephemeralKey);
    return {RatchetSession::forInitiator(init->sharedKey, init->remoteRatchetKey, init->associatedData),
            RatchetSession::forResponder(answer->sharedKey, signedPreKey.pair, answer->associatedData)};
}

QByteArray decrypted(RatchetSession &session, const QByteArray &message)
{
    return session.decrypt(message).value_or(QByteArray("<failed>"));
}

} // namespace

class TestRatchet : public QObject
{
    Q_OBJECT

private slots:
    void conversation()
    {
        Pair p = start();
        QVERIFY(p.alice.isValid());
        QVERIFY(p.bob.isValid());
        QVERIFY(p.alice.canSend());
        // Bob cannot speak first: he has no sending chain yet.
        QVERIFY(!p.bob.canSend());
        QVERIFY(p.bob.encrypt("too early").isEmpty());

        const QByteArray first = p.alice.encrypt("Hi Bob");
        QVERIFY(!first.contains("Hi Bob"));
        QCOMPARE(first.size(), RatchetSession::HeaderSize + 6 + Crypto::AeadTagSize);
        QCOMPARE(decrypted(p.bob, first), QByteArray("Hi Bob"));
        QVERIFY(p.bob.canSend());

        // Back and forth, several each way, every message a new key.
        for (int round = 0; round < 5; ++round) {
            const QByteArray a1 = p.bob.encrypt("from Bob " + QByteArray::number(round));
            const QByteArray a2 = p.bob.encrypt("again");
            QCOMPARE(decrypted(p.alice, a1), "from Bob " + QByteArray::number(round));
            QCOMPARE(decrypted(p.alice, a2), QByteArray("again"));
            const QByteArray b1 = p.alice.encrypt("from Alice");
            QCOMPARE(decrypted(p.bob, b1), QByteArray("from Alice"));
        }
        // The same text twice is two different messages.
        QVERIFY(p.alice.encrypt("same") != p.alice.encrypt("same"));
        // An empty message goes through.
        QCOMPARE(p.bob.decrypt(p.alice.encrypt(QByteArray())), std::optional<QByteArray>(QByteArray()));
    }

    void outOfOrderAndLost()
    {
        Pair p = start();
        QCOMPARE(decrypted(p.bob, p.alice.encrypt("0")), QByteArray("0"));
        const QByteArray m1 = p.alice.encrypt("1");
        const QByteArray m2 = p.alice.encrypt("2");
        const QByteArray m3 = p.alice.encrypt("3");
        QCOMPARE(decrypted(p.bob, m3), QByteArray("3"));
        QCOMPARE(decrypted(p.bob, m1), QByteArray("1"));

        // Bob replies; Alice's next chain. m2, from the old chain, still opens.
        QCOMPARE(decrypted(p.alice, p.bob.encrypt("reply")), QByteArray("reply"));
        const QByteArray n0 = p.alice.encrypt("new chain 0");
        const QByteArray n1 = p.alice.encrypt("new chain 1");
        QCOMPARE(decrypted(p.bob, n1), QByteArray("new chain 1"));
        QCOMPARE(decrypted(p.bob, m2), QByteArray("2"));
        QCOMPARE(decrypted(p.bob, n0), QByteArray("new chain 0"));
        // Each only once.
        QVERIFY(!p.bob.decrypt(m2));
        QVERIFY(!p.bob.decrypt(n1));
        QVERIFY(!p.bob.decrypt(m3));
        // And the session still works.
        QCOMPARE(decrypted(p.bob, p.alice.encrypt("still here")), QByteArray("still here"));
    }

    void rubbishChangesNothing()
    {
        Pair p = start();
        QCOMPARE(decrypted(p.bob, p.alice.encrypt("hello")), QByteArray("hello"));
        const QByteArray good = p.alice.encrypt("the real one");

        QByteArray changedBody = good;
        changedBody[changedBody.size() - 1] = char(changedBody.at(changedBody.size() - 1) ^ 1);
        QVERIFY(!p.bob.decrypt(changedBody));
        QByteArray changedHeader = good;
        changedHeader[RatchetSession::HeaderSize - 1] = char(changedHeader.at(RatchetSession::HeaderSize - 1) ^ 1);
        QVERIFY(!p.bob.decrypt(changedHeader));
        // A new ratchet key from a stranger.
        QByteArray strangerKey = good;
        strangerKey.replace(1, Crypto::KeySize, Crypto::DhKeyPair::generate().publicKey);
        QVERIFY(!p.bob.decrypt(strangerKey));
        QByteArray zeroKey = good;
        zeroKey.replace(1, Crypto::KeySize, QByteArray(Crypto::KeySize, '\0'));
        QVERIFY(!p.bob.decrypt(zeroKey));
        QVERIFY(!p.bob.decrypt(QByteArray()));
        QVERIFY(!p.bob.decrypt(good.left(RatchetSession::HeaderSize)));
        QByteArray otherVersion = good;
        otherVersion[0] = 2;
        QVERIFY(!p.bob.decrypt(otherVersion));

        // After all that, the real one still decrypts.
        QCOMPARE(decrypted(p.bob, good), QByteArray("the real one"));
    }

    void tooManySkipped()
    {
        Pair p = start();
        QCOMPARE(decrypted(p.bob, p.alice.encrypt("hello")), QByteArray("hello"));
        for (quint32 i = 0; i < RatchetSession::MaxSkip + 1; ++i)
            p.alice.encrypt("lost");
        QVERIFY(!p.bob.decrypt(p.alice.encrypt("too far")));

        // Exactly MaxSkip lost is still fine.
        Pair q = start();
        QCOMPARE(decrypted(q.bob, q.alice.encrypt("hello")), QByteArray("hello"));
        for (quint32 i = 0; i < RatchetSession::MaxSkip; ++i)
            q.alice.encrypt("lost");
        QCOMPARE(decrypted(q.bob, q.alice.encrypt("just right")), QByteArray("just right"));
    }

    void anotherSessionCannotRead()
    {
        Pair p = start();
        Pair other = start();
        QVERIFY(!other.bob.decrypt(p.alice.encrypt("not for you")));
    }

    void keptAndReadBack()
    {
        Pair p = start();
        QCOMPARE(decrypted(p.bob, p.alice.encrypt("one")), QByteArray("one"));
        const QByteArray late = p.alice.encrypt("late");
        QCOMPARE(decrypted(p.bob, p.alice.encrypt("two")), QByteArray("two"));

        RatchetSession bob = RatchetSession::deserialize(p.bob.serialize());
        RatchetSession alice = RatchetSession::deserialize(p.alice.serialize());
        QVERIFY(bob.isValid());
        QVERIFY(alice.isValid());
        QCOMPARE(decrypted(bob, late), QByteArray("late")); // the skipped key was kept
        QCOMPARE(decrypted(alice, bob.encrypt("after a restart")), QByteArray("after a restart"));
        QCOMPARE(decrypted(bob, alice.encrypt("still")), QByteArray("still"));

        QVERIFY(!RatchetSession::deserialize(QByteArray()).isValid());
        QVERIFY(!RatchetSession::deserialize("rubbish").isValid());
        const QByteArray data = p.alice.serialize();
        QVERIFY(!RatchetSession::deserialize(data.left(data.size() - 1)).isValid());
        QVERIFY(!RatchetSession::deserialize(data + 'x').isValid());
        QVERIFY(RatchetSession().serialize().isEmpty());
    }

    void refusesBadStarts()
    {
        QVERIFY(!RatchetSession::forInitiator("short", Crypto::DhKeyPair::generate().publicKey, "ad").isValid());
        QVERIFY(!RatchetSession::forInitiator(QByteArray(32, 'k'), QByteArray(32, '\0'), "ad").isValid());
        QVERIFY(!RatchetSession::forResponder(QByteArray(32, 'k'), Crypto::DhKeyPair(), "ad").isValid());
        RatchetSession invalid;
        QVERIFY(invalid.encrypt("x").isEmpty());
        QVERIFY(!invalid.decrypt(QByteArray(100, 'x')));
    }
};

CHAT_TEST_MAIN(TestRatchet, QCoreApplication)
#include "tst_ratchet.moc"
