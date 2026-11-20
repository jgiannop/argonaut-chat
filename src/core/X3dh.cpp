#include "X3dh.h"

namespace X3dh {

namespace {

const QByteArray kInfo = QByteArrayLiteral("ArgonautChat X3DH");

// SK from the DH outputs, which it wipes.
QByteArray deriveSharedKey(QList<QByteArray> &dhOutputs)
{
    QByteArray input(Crypto::KeySize, char(0xFF));
    for (QByteArray &output : dhOutputs) {
        input += output;
        Crypto::wipe(output);
    }
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

SignedPreKey makeSignedPreKey(const Crypto::IdentityKeyPair &identity, quint32 id)
{
    SignedPreKey key;
    key.id = id;
    key.pair = Crypto::DhKeyPair::generate();
    key.signature = identity.sign(key.pair.publicKey);
    return key;
}

QList<OneTimePreKey> makeOneTimePreKeys(quint32 firstId, int count)
{
    QList<OneTimePreKey> keys;
    keys.reserve(count);
    for (int i = 0; i < count; ++i)
        keys.append({firstId + quint32(i), Crypto::DhKeyPair::generate()});
    return keys;
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
        fail(error, QStringLiteral("a low-order key"));
        return std::nullopt;
    }

    Initiation result;
    result.sharedKey = deriveSharedKey(outputs);
    result.associatedData = me.publicKey() + them.identityKey;
    result.ephemeralKey = ephemeral.publicKey;
    result.signedPreKeyId = them.signedPreKeyId;
    result.oneTimePreKeyId = them.oneTimePreKeyId;
    result.remoteRatchetKey = them.signedPreKey;
    return result;
}

std::optional<Response> respond(const Crypto::IdentityKeyPair &me, const SignedPreKey &signedPreKey,
                                const OneTimePreKey *oneTimePreKey, const QByteArray &theirIdentityKey,
                                const QByteArray &theirEphemeralKey)
{
    if (!me.isValid() || !signedPreKey.pair.isValid() || (oneTimePreKey && !oneTimePreKey->pair.isValid()))
        return std::nullopt;
    const QByteArray theirIdentityDh = Crypto::dhPublicKeyOf(theirIdentityKey);
    if (theirIdentityDh.isEmpty() || theirEphemeralKey.size() != Crypto::KeySize)
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
        return std::nullopt;
    }

    Response result;
    result.sharedKey = deriveSharedKey(outputs);
    result.associatedData = theirIdentityKey + me.publicKey();
    return result;
}

} // namespace X3dh
