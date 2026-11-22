#include "X3dh.h"

namespace X3dh {

namespace {

// Names the curve, the hash and the KEM, as Signal's info string does.
const QByteArray kInfo = QByteArrayLiteral("ArgonautChat_CURVE25519_SHA-256_ML-KEM-1024");

// SK from the DH outputs and the KEM's shared secret, which it wipes.
QByteArray deriveSharedKey(QList<QByteArray> &dhOutputs, QByteArray &kemSecret)
{
    QByteArray input(Crypto::KeySize, char(0xFF));
    for (QByteArray &output : dhOutputs) {
        input += output;
        Crypto::wipe(output);
    }
    input += kemSecret;
    Crypto::wipe(kemSecret);
    QByteArray key = Crypto::hkdf(input, QByteArray(), kInfo, Crypto::KeySize);
    Crypto::wipe(input);
    return key;
}

bool anyEmpty(const QList<QByteArray> &list)
{
    for (const QByteArray &item : list) {
        if (item.isEmpty())
            return true;
    }
    return false;
}

void fail(QString *error, const QString &why)
{
    if (error)
        *error = why;
}

} // namespace

QByteArray kemSigned(const QByteArray &kemPublicKey)
{
    return QByteArray(1, char(0x08)) + kemPublicKey;
}

SignedPreKey makeSignedPreKey(const Crypto::IdentityKeyPair &identity, quint32 id)
{
    SignedPreKey key;
    key.id = id;
    key.pair = Crypto::DhKeyPair::generate();
    key.signature = identity.sign(key.pair.publicKey);
    key.kemPair = MlKem::KeyPair::generate();
    key.kemSignature = identity.sign(kemSigned(key.kemPair.publicKey));
    return key;
}

QList<OneTimePreKey> makeOneTimePreKeys(const Crypto::IdentityKeyPair &identity, quint32 firstId, int count)
{
    QList<OneTimePreKey> keys;
    keys.reserve(count);
    for (int i = 0; i < count; ++i) {
        OneTimePreKey key;
        key.id = firstId + quint32(i);
        key.pair = Crypto::DhKeyPair::generate();
        key.kemPair = MlKem::KeyPair::generate();
        key.kemSignature = identity.sign(kemSigned(key.kemPair.publicKey));
        keys.append(key);
    }
    return keys;
}

PreKeyBundle bundleFor(const QByteArray &identityKey, const SignedPreKey &signedPreKey,
                       const OneTimePreKey *oneTimePreKey)
{
    PreKeyBundle bundle;
    bundle.identityKey = identityKey;
    bundle.signedPreKeyId = signedPreKey.id;
    bundle.signedPreKey = signedPreKey.pair.publicKey;
    bundle.signature = signedPreKey.signature;
    if (signedPreKey.hasKem()) {
        bundle.signedKemKey = signedPreKey.kemPair.publicKey;
        bundle.signedKemSignature = signedPreKey.kemSignature;
    }
    if (oneTimePreKey) {
        bundle.oneTimePreKeyId = oneTimePreKey->id;
        bundle.oneTimePreKey = oneTimePreKey->pair.publicKey;
        if (oneTimePreKey->hasKem()) {
            bundle.oneTimeKemKey = oneTimePreKey->kemPair.publicKey;
            bundle.oneTimeKemSignature = oneTimePreKey->kemSignature;
        }
    }
    return bundle;
}

std::optional<Initiation> initiate(const Crypto::IdentityKeyPair &me, const PreKeyBundle &them, QString *error)
{
    if (!me.isValid()) {
        fail(error, QStringLiteral("no identity key"));
        return std::nullopt;
    }
    const QByteArray theirIdentityDh = Crypto::dhPublicKeyOf(them.identityKey);
    if (theirIdentityDh.isEmpty()) {
        fail(error, QStringLiteral("bad identity key"));
        return std::nullopt;
    }
    if (them.signedPreKey.size() != Crypto::KeySize
        || !Crypto::verify(them.identityKey, them.signedPreKey, them.signature)) {
        fail(error, QStringLiteral("the signed prekey's signature does not match"));
        return std::nullopt;
    }
    if (them.oneTimePreKeyId && them.oneTimePreKey.size() != Crypto::KeySize) {
        fail(error, QStringLiteral("bad one-time prekey"));
        return std::nullopt;
    }
    // The KEM prekey: the one-time prekey's if it has one, else the last-resort one.
    const bool oneTimeKem = them.oneTimePreKeyId && !them.oneTimeKemKey.isEmpty();
    const QByteArray kemKey = oneTimeKem ? them.oneTimeKemKey : them.signedKemKey;
    const QByteArray kemSignature = oneTimeKem ? them.oneTimeKemSignature : them.signedKemSignature;
    if (kemKey.isEmpty()) {
        fail(error, QString::fromLatin1(NoKemError));
        return std::nullopt;
    }
    if (kemKey.size() != MlKem::PublicKeySize || !Crypto::verify(them.identityKey, kemSigned(kemKey), kemSignature)) {
        fail(error, QStringLiteral("the KEM prekey's signature does not match"));
        return std::nullopt;
    }
    auto kem = MlKem::encapsulate(kemKey);
    if (!kem) {
        fail(error, QStringLiteral("bad KEM prekey"));
        return std::nullopt;
    }

    const Crypto::DhKeyPair ephemeral = Crypto::DhKeyPair::generate();
    QByteArray identitySecret = me.dhSecretKey();
    QList<QByteArray> outputs{
        Crypto::dh(identitySecret, them.signedPreKey),
        Crypto::dh(ephemeral.secretKey, theirIdentityDh),
        Crypto::dh(ephemeral.secretKey, them.signedPreKey),
    };
    if (them.oneTimePreKeyId)
        outputs.append(Crypto::dh(ephemeral.secretKey, them.oneTimePreKey));
    Crypto::wipe(identitySecret);
    if (anyEmpty(outputs)) {
        for (QByteArray &output : outputs)
            Crypto::wipe(output);
        Crypto::wipe(kem->sharedSecret);
        fail(error, QStringLiteral("a low-order key"));
        return std::nullopt;
    }

    Initiation result;
    result.sharedKey = deriveSharedKey(outputs, kem->sharedSecret);
    result.associatedData = me.publicKey() + them.identityKey;
    result.ephemeralKey = ephemeral.publicKey;
    result.signedPreKeyId = them.signedPreKeyId;
    result.oneTimePreKeyId = them.oneTimePreKeyId;
    result.kemCiphertext = kem->ciphertext;
    result.kemLastResort = !oneTimeKem;
    result.remoteRatchetKey = them.signedPreKey;
    return result;
}

std::optional<Response> respond(const Crypto::IdentityKeyPair &me, const SignedPreKey &signedPreKey,
                                const OneTimePreKey *oneTimePreKey, const QByteArray &kemSecretKey,
                                const QByteArray &kemCiphertext, const QByteArray &theirIdentityKey,
                                const QByteArray &theirEphemeralKey)
{
    if (!me.isValid() || !signedPreKey.pair.isValid() || (oneTimePreKey && !oneTimePreKey->pair.isValid()))
        return std::nullopt;
    const QByteArray theirIdentityDh = Crypto::dhPublicKeyOf(theirIdentityKey);
    if (theirIdentityDh.isEmpty() || theirEphemeralKey.size() != Crypto::KeySize)
        return std::nullopt;
    auto kemSecret = MlKem::decapsulate(kemSecretKey, kemCiphertext);
    if (!kemSecret)
        return std::nullopt;

    QByteArray identitySecret = me.dhSecretKey();
    QList<QByteArray> outputs{
        Crypto::dh(signedPreKey.pair.secretKey, theirIdentityDh),
        Crypto::dh(identitySecret, theirEphemeralKey),
        Crypto::dh(signedPreKey.pair.secretKey, theirEphemeralKey),
    };
    if (oneTimePreKey)
        outputs.append(Crypto::dh(oneTimePreKey->pair.secretKey, theirEphemeralKey));
    Crypto::wipe(identitySecret);
    if (anyEmpty(outputs)) {
        for (QByteArray &output : outputs)
            Crypto::wipe(output);
        Crypto::wipe(*kemSecret);
        return std::nullopt;
    }

    Response result;
    result.sharedKey = deriveSharedKey(outputs, *kemSecret);
    result.associatedData = theirIdentityKey + me.publicKey();
    return result;
}

} // namespace X3dh
