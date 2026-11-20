#include "Crypto.h"
#include "SafetyNumber.h"
#include "TestSupport.h"
#include "X3dh.h"

#include <QCoreApplication>
#include <QTest>

class TestCrypto : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(Crypto::init()); }

    // RFC 5869, appendix A, test cases 1 and 3.
    void hkdfMatchesRfc5869()
    {
        const QByteArray ikm(22, '\x0b');
        QCOMPARE(Crypto::hkdf(ikm, QByteArray::fromHex("000102030405060708090a0b0c"),
                              QByteArray::fromHex("f0f1f2f3f4f5f6f7f8f9"), 42)
                     .toHex(),
                 QByteArray("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));
        QCOMPARE(Crypto::hkdf(ikm, QByteArray(), QByteArray(), 42).toHex(),
                 QByteArray("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"));
        QVERIFY(Crypto::hkdf(ikm, QByteArray(), QByteArray(), 0).isEmpty());
        QVERIFY(Crypto::hkdf(ikm, QByteArray(), QByteArray(), 255 * 32 + 1).isEmpty());
    }

    void dhAgrees()
    {
        const auto a = Crypto::DhKeyPair::generate();
        const auto b = Crypto::DhKeyPair::generate();
        const QByteArray ab = Crypto::dh(a.secretKey, b.publicKey);
        QCOMPARE(ab.size(), Crypto::KeySize);
        QCOMPARE(ab, Crypto::dh(b.secretKey, a.publicKey));
        // A low-order key (all zeros) and a wrong size are refused.
        QVERIFY(Crypto::dh(a.secretKey, QByteArray(32, '\0')).isEmpty());
        QVERIFY(Crypto::dh(a.secretKey, QByteArray(31, '\x09')).isEmpty());
    }

    void identitySignsAndAgrees()
    {
        const auto alice = Crypto::IdentityKeyPair::generate();
        QVERIFY(alice.isValid());
        const QByteArray signature = alice.sign("hello");
        QCOMPARE(signature.size(), Crypto::SignatureSize);
        QVERIFY(Crypto::verify(alice.publicKey(), "hello", signature));
        QVERIFY(!Crypto::verify(alice.publicKey(), "hellO", signature));
        QVERIFY(!Crypto::verify(Crypto::IdentityKeyPair::generate().publicKey(), "hello", signature));

        // The same key as X25519 agrees with the other side.
        const auto other = Crypto::DhKeyPair::generate();
        QCOMPARE(alice.dhPublicKey(), Crypto::dhPublicKeyOf(alice.publicKey()));
        QCOMPARE(Crypto::dh(alice.dhSecretKey(), other.publicKey),
                 Crypto::dh(other.secretKey, alice.dhPublicKey()));

        // Kept and read back; anything else is not a key.
        const auto again = Crypto::IdentityKeyPair::fromSecretKey(alice.secretKey());
        QVERIFY(again.isValid());
        QCOMPARE(again.publicKey(), alice.publicKey());
        QByteArray broken = alice.secretKey();
        broken[40] = char(broken.at(40) ^ 1); // the public half no longer matches the seed
        QVERIFY(!Crypto::IdentityKeyPair::fromSecretKey(broken).isValid());
        QVERIFY(!Crypto::IdentityKeyPair::fromSecretKey("short").isValid());
    }

    void aeadDetectsChanges()
    {
        const QByteArray key = Crypto::randomBytes(32);
        const QByteArray nonce = Crypto::randomBytes(24);
        const QByteArray sealed = Crypto::seal(key, nonce, "secret", "ad");
        QCOMPARE(sealed.size(), 6 + Crypto::AeadTagSize);
        QCOMPARE(Crypto::open(key, nonce, sealed, "ad").value_or(QByteArray()), QByteArray("secret"));
        QVERIFY(!Crypto::open(key, nonce, sealed, "aD"));
        QByteArray changed = sealed;
        changed[0] = char(changed.at(0) ^ 1);
        QVERIFY(!Crypto::open(key, nonce, changed, "ad"));
        QVERIFY(!Crypto::open(key, nonce, sealed.left(10), "ad"));
        // An empty message is a message.
        QCOMPARE(Crypto::open(key, nonce, Crypto::seal(key, nonce, QByteArray(), QByteArray()), QByteArray()),
                 std::optional<QByteArray>(QByteArray()));
    }

    void x3dhAgrees_data()
    {
        QTest::addColumn<bool>("oneTime");
        QTest::newRow("with a one-time prekey") << true;
        QTest::newRow("without") << false;
    }

    void x3dhAgrees()
    {
        QFETCH(bool, oneTime);
        const auto alice = Crypto::IdentityKeyPair::generate();
        const auto bob = Crypto::IdentityKeyPair::generate();
        const X3dh::SignedPreKey signedPreKey = X3dh::makeSignedPreKey(bob, 7);
        const QList<X3dh::OneTimePreKey> oneTimeKeys = X3dh::makeOneTimePreKeys(100, 5);
        QCOMPARE(oneTimeKeys.size(), 5);
        QCOMPARE(oneTimeKeys.last().id, 104u);

        X3dh::PreKeyBundle bundle{bob.publicKey(), 7, signedPreKey.pair.publicKey, signedPreKey.signature, {}, {}};
        if (oneTime) {
            bundle.oneTimePreKeyId = 102;
            bundle.oneTimePreKey = oneTimeKeys.at(2).pair.publicKey;
        }
        QString error;
        const auto start = X3dh::initiate(alice, bundle, &error);
        QVERIFY2(start, qPrintable(error));
        QCOMPARE(start->sharedKey.size(), 32);
        QCOMPARE(start->signedPreKeyId, 7u);
        QCOMPARE(start->oneTimePreKeyId, bundle.oneTimePreKeyId);
        QCOMPARE(start->remoteRatchetKey, signedPreKey.pair.publicKey);
        QCOMPARE(start->associatedData, alice.publicKey() + bob.publicKey());

        const auto answer = X3dh::respond(bob, signedPreKey, oneTime ? &oneTimeKeys.at(2) : nullptr,
                                          alice.publicKey(), start->ephemeralKey);
        QVERIFY(answer);
        QCOMPARE(answer->sharedKey, start->sharedKey);
        QCOMPARE(answer->associatedData, start->associatedData);

        // The wrong one-time key, or none when one was used: another secret.
        const auto wrong = X3dh::respond(bob, signedPreKey, oneTime ? &oneTimeKeys.at(3) : &oneTimeKeys.at(0),
                                         alice.publicKey(), start->ephemeralKey);
        QVERIFY(wrong);
        QVERIFY(wrong->sharedKey != start->sharedKey);
        // Someone else claiming to be Alice: another secret.
        const auto impostor = X3dh::respond(bob, signedPreKey, oneTime ? &oneTimeKeys.at(2) : nullptr,
                                            Crypto::IdentityKeyPair::generate().publicKey(), start->ephemeralKey);
        QVERIFY(impostor);
        QVERIFY(impostor->sharedKey != start->sharedKey);
    }

    void x3dhRefusesBadBundles()
    {
        const auto alice = Crypto::IdentityKeyPair::generate();
        const auto bob = Crypto::IdentityKeyPair::generate();
        const X3dh::SignedPreKey signedPreKey = X3dh::makeSignedPreKey(bob, 1);
        const X3dh::PreKeyBundle good{bob.publicKey(), 1, signedPreKey.pair.publicKey, signedPreKey.signature, {}, {}};
        QVERIFY(X3dh::initiate(alice, good));

        // The server swapped the signed prekey for its own.
        X3dh::PreKeyBundle swapped = good;
        swapped.signedPreKey = Crypto::DhKeyPair::generate().publicKey;
        QString error;
        QVERIFY(!X3dh::initiate(alice, swapped, &error));
        QVERIFY(!error.isEmpty());
        // ... or the identity key.
        X3dh::PreKeyBundle otherIdentity = good;
        otherIdentity.identityKey = Crypto::IdentityKeyPair::generate().publicKey();
        QVERIFY(!X3dh::initiate(alice, otherIdentity));
        // Rubbish.
        X3dh::PreKeyBundle rubbish = good;
        rubbish.identityKey = "not a key";
        QVERIFY(!X3dh::initiate(alice, rubbish));
        X3dh::PreKeyBundle badOneTime = good;
        badOneTime.oneTimePreKeyId = 3;
        badOneTime.oneTimePreKey = QByteArray(32, '\0'); // low order
        QVERIFY(!X3dh::initiate(alice, badOneTime));
        badOneTime.oneTimePreKey = "short";
        QVERIFY(!X3dh::initiate(alice, badOneTime));
        // Bob refuses a low-order ephemeral key.
        QVERIFY(!X3dh::respond(bob, signedPreKey, nullptr, alice.publicKey(), QByteArray(32, '\0')));
    }

    void safetyNumbersMatch()
    {
        const QByteArray alice = Crypto::IdentityKeyPair::generate().publicKey();
        const QByteArray bob = Crypto::IdentityKeyPair::generate().publicKey();
        const QString mine = SafetyNumber::compute(alice, "alice", bob, "bob");
        QCOMPARE(mine.size(), 60);
        for (const QChar c : mine)
            QVERIFY(c.isDigit());
        // The same on both phones.
        QCOMPARE(SafetyNumber::compute(bob, "bob", alice, "alice"), mine);
        // Another key: another number.
        QVERIFY(SafetyNumber::compute(alice, "alice", Crypto::IdentityKeyPair::generate().publicKey(), "bob")
                != mine);
        QVERIFY(SafetyNumber::compute(alice, "alice", "short", "bob").isEmpty());
        QCOMPARE(SafetyNumber::display("123456789012345").toLatin1(), QByteArray("12345 67890 12345"));
    }
};

CHAT_TEST_MAIN(TestCrypto, QCoreApplication)
#include "tst_crypto.moc"
