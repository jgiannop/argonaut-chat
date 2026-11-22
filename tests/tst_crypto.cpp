#include "Crypto.h"
#include "MlKem.h"
#include "SafetyNumber.h"
#include "TestSupport.h"
#include "X3dh.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

    // ML-KEM-1024 against NIST's ACVP vectors (tests/data, FIPS 203).
    void mlKemMatchesNist()
    {
        QFile file(QStringLiteral(":/data/mlkem1024-acvp.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
        auto hex = [](const QJsonValue &v) { return QByteArray::fromHex(v.toString().toLatin1()); };
        const QJsonArray keyGen = json.value(QStringLiteral("keyGen")).toArray();
        QCOMPARE(keyGen.size(), 5);
        for (const QJsonValue &item : keyGen) {
            const QJsonObject t = item.toObject();
            const MlKem::KeyPair pair = MlKem::KeyPair::fromSeed(hex(t.value("d")) + hex(t.value("z")));
            QCOMPARE(pair.publicKey, hex(t.value("ek")));
            QCOMPARE(pair.secretKey, hex(t.value("dk")));
        }
        const QJsonArray encaps = json.value(QStringLiteral("encapsulation")).toArray();
        QCOMPARE(encaps.size(), 5);
        for (const QJsonValue &item : encaps) {
            const QJsonObject t = item.toObject();
            const auto out = MlKem::encapsulate(hex(t.value("ek")), hex(t.value("m")));
            QVERIFY(out);
            QCOMPARE(out->ciphertext, hex(t.value("c")));
            QCOMPARE(out->sharedSecret, hex(t.value("k")));
            QCOMPARE(MlKem::decapsulate(hex(t.value("dk")), out->ciphertext).value_or(QByteArray()), hex(t.value("k")));
        }
        // Valid and changed ciphertexts: a changed one gives FIPS 203's rejection secret.
        const QJsonArray decaps = json.value(QStringLiteral("decapsulation")).toArray();
        QCOMPARE(decaps.size(), 10);
        int modified = 0;
        for (const QJsonValue &item : decaps) {
            const QJsonObject t = item.toObject();
            QCOMPARE(MlKem::decapsulate(hex(t.value("dk")), hex(t.value("c"))).value_or(QByteArray()), hex(t.value("k")));
            modified += t.value("reason").toString() == QLatin1String("modified ciphertext");
        }
        QVERIFY(modified > 0);
        // The input checks: bad public keys refused, also by encapsulate().
        int refused = 0;
        for (const QJsonValue &item : json.value(QStringLiteral("encapsulationKeyCheck")).toArray()) {
            const QJsonObject t = item.toObject();
            const bool ok = t.value("testPassed").toBool();
            QCOMPARE(MlKem::isValidPublicKey(hex(t.value("ek"))), ok);
            QCOMPARE(MlKem::encapsulate(hex(t.value("ek"))).has_value(), ok);
            refused += !ok;
        }
        QVERIFY(refused > 0);
        refused = 0;
        for (const QJsonValue &item : json.value(QStringLiteral("decapsulationKeyCheck")).toArray()) {
            const QJsonObject t = item.toObject();
            QCOMPARE(MlKem::isValidSecretKey(hex(t.value("dk"))), t.value("testPassed").toBool());
            refused += !t.value("testPassed").toBool();
        }
        QVERIFY(refused > 0);
    }

    void mlKemAgrees()
    {
        const MlKem::KeyPair bob = MlKem::KeyPair::generate();
        QVERIFY(bob.isValid());
        QVERIFY(MlKem::isValidPublicKey(bob.publicKey) && MlKem::isValidSecretKey(bob.secretKey));
        QVERIFY(bob.publicKey != MlKem::KeyPair::generate().publicKey);
        const auto alice = MlKem::encapsulate(bob.publicKey);
        QVERIFY(alice);
        QCOMPARE(alice->sharedSecret.size(), MlKem::SharedSecretSize);
        QCOMPARE(MlKem::decapsulate(bob.secretKey, alice->ciphertext).value_or(QByteArray()), alice->sharedSecret);
        QVERIFY(alice->sharedSecret != MlKem::encapsulate(bob.publicKey)->sharedSecret); // random each time
        // A changed ciphertext: another secret, no error (implicit rejection).
        QByteArray changed = alice->ciphertext;
        changed[100] = char(changed.at(100) ^ 1);
        const auto other = MlKem::decapsulate(bob.secretKey, changed);
        QVERIFY(other && *other != alice->sharedSecret);
        // Wrong sizes.
        QVERIFY(!MlKem::encapsulate(bob.publicKey.left(100)));
        QVERIFY(!MlKem::decapsulate(bob.secretKey, alice->ciphertext.left(100)));
        QVERIFY(!MlKem::decapsulate(bob.secretKey.left(100), alice->ciphertext));
        QVERIFY(!MlKem::KeyPair::fromSeed(QByteArray(10, 'x')).isValid());
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

    void sealedBoxes()
    {
        const auto pair = Crypto::DhKeyPair::generate();
        const QByteArray sealed = Crypto::sealTo(pair.publicKey, "the account's keys");
        QVERIFY(!sealed.contains("account"));
        QCOMPARE(Crypto::openSealed(pair, sealed), std::optional<QByteArray>("the account's keys"));
        QVERIFY(Crypto::sealTo(pair.publicKey, "x") != Crypto::sealTo(pair.publicKey, "x"));
        QVERIFY(!Crypto::openSealed(Crypto::DhKeyPair::generate(), sealed)); // another key
        QByteArray changed = sealed;
        changed[changed.size() - 1] = char(changed.at(changed.size() - 1) ^ 1);
        QVERIFY(!Crypto::openSealed(pair, changed));
        QVERIFY(!Crypto::openSealed(pair, "short"));
        QVERIFY(Crypto::sealTo("not a key", "x").isEmpty());
    }

    void pqxdhAgrees_data()
    {
        QTest::addColumn<bool>("oneTime");
        QTest::newRow("with a one-time prekey (and its KEM prekey)") << true;
        QTest::newRow("without: the last-resort KEM prekey") << false;
    }

    void pqxdhAgrees()
    {
        QFETCH(bool, oneTime);
        const auto alice = Crypto::IdentityKeyPair::generate();
        const auto bob = Crypto::IdentityKeyPair::generate();
        const X3dh::SignedPreKey signedPreKey = X3dh::makeSignedPreKey(bob, 7);
        QVERIFY(signedPreKey.hasKem());
        QVERIFY(Crypto::verify(bob.publicKey(), X3dh::kemSigned(signedPreKey.kemPair.publicKey), signedPreKey.kemSignature));
        const QList<X3dh::OneTimePreKey> oneTimeKeys = X3dh::makeOneTimePreKeys(bob, 100, 5);
        QCOMPARE(oneTimeKeys.size(), 5);
        QCOMPARE(oneTimeKeys.last().id, 104u);
        QVERIFY(oneTimeKeys.first().hasKem());
        QVERIFY(oneTimeKeys.at(0).kemPair.publicKey != oneTimeKeys.at(1).kemPair.publicKey);

        const X3dh::PreKeyBundle bundle =
            X3dh::bundleFor(bob.publicKey(), signedPreKey, oneTime ? &oneTimeKeys.at(2) : nullptr);
        QString error;
        const auto start = X3dh::initiate(alice, bundle, &error);
        QVERIFY2(start, qPrintable(error));
        QCOMPARE(start->sharedKey.size(), 32);
        QCOMPARE(start->signedPreKeyId, 7u);
        QCOMPARE(start->oneTimePreKeyId, bundle.oneTimePreKeyId);
        QCOMPARE(start->kemLastResort, !oneTime);
        QCOMPARE(start->kemCiphertext.size(), MlKem::CiphertextSize);
        QCOMPARE(start->remoteRatchetKey, signedPreKey.pair.publicKey);
        QCOMPARE(start->associatedData, alice.publicKey() + bob.publicKey());

        const QByteArray kemSecret = oneTime ? oneTimeKeys.at(2).kemPair.secretKey : signedPreKey.kemPair.secretKey;
        const X3dh::OneTimePreKey *used = oneTime ? &oneTimeKeys.at(2) : nullptr;
        const auto answer = X3dh::respond(bob, signedPreKey, used, kemSecret, start->kemCiphertext, alice.publicKey(),
                                          start->ephemeralKey);
        QVERIFY(answer);
        QCOMPARE(answer->sharedKey, start->sharedKey);
        QCOMPARE(answer->associatedData, start->associatedData);

        // The wrong one-time key, or none when one was used: another secret.
        const auto wrong = X3dh::respond(bob, signedPreKey, oneTime ? &oneTimeKeys.at(3) : &oneTimeKeys.at(0), kemSecret,
                                         start->kemCiphertext, alice.publicKey(), start->ephemeralKey);
        QVERIFY(wrong && wrong->sharedKey != start->sharedKey);
        // The wrong KEM secret, or a changed ciphertext: another secret (all the
        // DH results the same: ML-KEM's part counts).
        const auto otherKem = X3dh::respond(bob, signedPreKey, used, oneTimeKeys.at(4).kemPair.secretKey,
                                            start->kemCiphertext, alice.publicKey(), start->ephemeralKey);
        QVERIFY(otherKem && otherKem->sharedKey != start->sharedKey);
        QByteArray changed = start->kemCiphertext;
        changed[7] = char(changed.at(7) ^ 0x40);
        const auto changedKem =
            X3dh::respond(bob, signedPreKey, used, kemSecret, changed, alice.publicKey(), start->ephemeralKey);
        QVERIFY(changedKem && changedKem->sharedKey != start->sharedKey);
        QVERIFY(!X3dh::respond(bob, signedPreKey, used, kemSecret, changed.left(100), alice.publicKey(),
                               start->ephemeralKey));
        // Someone else claiming to be Alice: another secret.
        const auto impostor = X3dh::respond(bob, signedPreKey, used, kemSecret, start->kemCiphertext,
                                            Crypto::IdentityKeyPair::generate().publicKey(), start->ephemeralKey);
        QVERIFY(impostor && impostor->sharedKey != start->sharedKey);
        // A one-time prekey without a KEM prekey (made before PQXDH): the last-resort one.
        X3dh::OneTimePreKey old = oneTimeKeys.at(1);
        old.kemPair = {};
        const auto fallback = X3dh::initiate(alice, X3dh::bundleFor(bob.publicKey(), signedPreKey, &old));
        QVERIFY(fallback && fallback->kemLastResort && fallback->oneTimePreKeyId == 101u);
    }

    void pqxdhRefusesBadBundles()
    {
        const auto alice = Crypto::IdentityKeyPair::generate();
        const auto bob = Crypto::IdentityKeyPair::generate();
        const X3dh::SignedPreKey signedPreKey = X3dh::makeSignedPreKey(bob, 1);
        const X3dh::OneTimePreKey oneTime = X3dh::makeOneTimePreKeys(bob, 5, 1).first();
        const X3dh::PreKeyBundle good = X3dh::bundleFor(bob.publicKey(), signedPreKey);
        QVERIFY(X3dh::initiate(alice, good));
        QString error;

        // The server swapped the signed prekey for its own.
        X3dh::PreKeyBundle swapped = good;
        swapped.signedPreKey = Crypto::DhKeyPair::generate().publicKey;
        QVERIFY(!X3dh::initiate(alice, swapped, &error));
        QVERIFY(!error.isEmpty());
        // ... or the identity key.
        X3dh::PreKeyBundle otherIdentity = good;
        otherIdentity.identityKey = Crypto::IdentityKeyPair::generate().publicKey();
        QVERIFY(!X3dh::initiate(alice, otherIdentity));
        // ... or a KEM prekey: its own (signature), another of Bob's under the
        // wrong signature, or a one-time one with the last-resort signature.
        X3dh::PreKeyBundle kemSwapped = good;
        kemSwapped.signedKemKey = MlKem::KeyPair::generate().publicKey;
        QVERIFY(!X3dh::initiate(alice, kemSwapped, &error));
        QVERIFY(error.contains("signature"));
        X3dh::PreKeyBundle oneTimeSwapped = X3dh::bundleFor(bob.publicKey(), signedPreKey, &oneTime);
        QVERIFY(X3dh::initiate(alice, oneTimeSwapped));
        oneTimeSwapped.oneTimeKemSignature = signedPreKey.kemSignature;
        QVERIFY(!X3dh::initiate(alice, oneTimeSwapped));
        // The signature signs the type byte too: the bare key's signature is not it.
        X3dh::PreKeyBundle bareSigned = good;
        bareSigned.signedKemSignature = bob.sign(signedPreKey.kemPair.publicKey);
        QVERIFY(!X3dh::initiate(alice, bareSigned));
        // A KEM key that fails FIPS 203's check, even when Bob signed it.
        X3dh::PreKeyBundle notAKey = good;
        notAKey.signedKemKey = QByteArray(MlKem::PublicKeySize, char(0xFF));
        notAKey.signedKemSignature = bob.sign(X3dh::kemSigned(notAKey.signedKemKey));
        QVERIFY(!X3dh::initiate(alice, notAKey, &error));
        QCOMPARE(error, QString("bad KEM prekey"));
        // No KEM prekey at all (a version before PQXDH, or stripped by the
        // server): no conversation, and the reason says so. No X3DH fallback.
        X3dh::PreKeyBundle noKem = good;
        noKem.signedKemKey.clear();
        noKem.signedKemSignature.clear();
        QVERIFY(!X3dh::initiate(alice, noKem, &error));
        QCOMPARE(error, QString(X3dh::NoKemError));
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
        const auto start = X3dh::initiate(alice, good);
        QVERIFY(!X3dh::respond(bob, signedPreKey, nullptr, signedPreKey.kemPair.secretKey, start->kemCiphertext,
                               alice.publicKey(), QByteArray(32, '\0')));
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
