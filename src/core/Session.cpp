#include "Session.h"

#include <QDataStream>
#include <QIODevice>

namespace {

constexpr quint32 kSerialMagic = 0x41535332;   // "ASS2": with PQXDH's ciphertext
constexpr quint32 kSerialMagicV1 = 0x41535331; // "ASS1": before PQXDH

} // namespace

std::optional<Session> Session::initiate(const Crypto::IdentityKeyPair &me, const X3dh::PreKeyBundle &them,
                                         QString *error)
{
    const auto start = X3dh::initiate(me, them, error);
    if (!start)
        return std::nullopt;
    Session session;
    session.m_ratchet =
        RatchetSession::forInitiator(start->sharedKey, start->remoteRatchetKey, start->associatedData);
    session.m_remoteIdentity = them.identityKey;
    session.m_baseKey = start->ephemeralKey;
    session.m_pendingPreKey = true;
    session.m_localIdentity = me.publicKey();
    session.m_signedPreKeyId = start->signedPreKeyId;
    session.m_oneTimePreKeyId = start->oneTimePreKeyId;
    session.m_kemCiphertext = start->kemCiphertext;
    session.m_kemLastResort = start->kemLastResort;
    if (!session.isValid())
        return std::nullopt;
    return session;
}

std::optional<Session::Accepted> Session::accept(const Crypto::IdentityKeyPair &me,
                                                 const Protocol::CipherMessage &message,
                                                 const SignedPreKeyLookup &signedPreKey,
                                                 const OneTimePreKeyLookup &oneTimePreKey)
{
    // PQXDH only: an X3DH start (an older version, or someone stripping the KEM) is refused.
    if (!message.isPqxdh() || !signedPreKey)
        return std::nullopt;
    const std::optional<X3dh::SignedPreKey> signedKey = signedPreKey(message.signedPreKeyId);
    if (!signedKey)
        return std::nullopt;
    std::optional<X3dh::OneTimePreKey> oneTimeKey;
    if (message.oneTimePreKeyId) {
        // One that was used already, or never was: refused (a replayed start).
        if (!oneTimePreKey)
            return std::nullopt;
        oneTimeKey = oneTimePreKey(*message.oneTimePreKeyId);
        if (!oneTimeKey)
            return std::nullopt;
    }
    // The KEM prekey it names: the one-time prekey's, or the signed prekey's last-resort one.
    if (!message.kemLastResort && !oneTimeKey)
        return std::nullopt;
    const MlKem::KeyPair &kem = message.kemLastResort ? signedKey->kemPair : oneTimeKey->kemPair;
    if (!kem.isValid())
        return std::nullopt;
    const auto answer = X3dh::respond(me, *signedKey, oneTimeKey ? &*oneTimeKey : nullptr, kem.secretKey,
                                      message.kemCiphertext, message.identityKey, message.baseKey);
    if (!answer)
        return std::nullopt;

    Accepted accepted;
    accepted.session.m_ratchet =
        RatchetSession::forResponder(answer->sharedKey, signedKey->pair, answer->associatedData);
    accepted.session.m_remoteIdentity = message.identityKey;
    accepted.session.m_baseKey = message.baseKey;
    auto plaintext = accepted.session.m_ratchet.decrypt(message.ratchetMessage);
    if (!plaintext)
        return std::nullopt;
    accepted.plaintext = *plaintext;
    accepted.usedOneTimePreKey = message.oneTimePreKeyId;
    return accepted;
}

QByteArray Session::encrypt(const QByteArray &plaintext)
{
    if (!isValid())
        return {};
    Protocol::CipherMessage message;
    message.ratchetMessage = m_ratchet.encrypt(plaintext);
    if (message.ratchetMessage.isEmpty())
        return {};
    if (m_pendingPreKey) {
        message.preKey = true;
        message.identityKey = m_localIdentity;
        message.baseKey = m_baseKey;
        message.signedPreKeyId = m_signedPreKeyId;
        message.oneTimePreKeyId = m_oneTimePreKeyId;
        message.kemCiphertext = m_kemCiphertext;
        message.kemLastResort = m_kemLastResort;
    }
    return Protocol::encodeCipherMessage(message);
}

std::optional<QByteArray> Session::decrypt(const Protocol::CipherMessage &message)
{
    if (!isValid())
        return std::nullopt;
    // A prekey message belongs here only if it is from the same start.
    if (message.preKey && (message.baseKey != m_baseKey || message.identityKey != m_remoteIdentity))
        return std::nullopt;
    auto plaintext = m_ratchet.decrypt(message.ratchetMessage);
    if (plaintext)
        m_pendingPreKey = false; // Bob has answered (or, for Bob, nothing changes)
    return plaintext;
}

QByteArray Session::serialize() const
{
    if (!isValid())
        return {};
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_4);
    out << kSerialMagic << m_ratchet.serialize() << m_remoteIdentity << m_baseKey << m_pendingPreKey
        << m_localIdentity << m_signedPreKeyId << bool(m_oneTimePreKeyId) << m_oneTimePreKeyId.value_or(0)
        << m_kemCiphertext << m_kemLastResort;
    return data;
}

Session Session::deserialize(const QByteArray &data)
{
    QDataStream in(data);
    in.setVersion(QDataStream::Qt_6_4);
    quint32 magic = 0;
    QByteArray ratchet;
    bool hasOneTime = false;
    quint32 oneTimeId = 0;
    Session session;
    in >> magic;
    if (magic != kSerialMagic && magic != kSerialMagicV1)
        return {};
    in >> ratchet >> session.m_remoteIdentity >> session.m_baseKey >> session.m_pendingPreKey
        >> session.m_localIdentity >> session.m_signedPreKeyId >> hasOneTime >> oneTimeId;
    if (magic == kSerialMagic)
        in >> session.m_kemCiphertext >> session.m_kemLastResort;
    if (in.status() != QDataStream::Ok || !in.atEnd()
        || (!session.m_kemCiphertext.isEmpty() && session.m_kemCiphertext.size() != MlKem::CiphertextSize))
        return {};
    session.m_ratchet = RatchetSession::deserialize(ratchet);
    if (hasOneTime)
        session.m_oneTimePreKeyId = oneTimeId;
    if (!session.isValid() || session.m_baseKey.size() != Crypto::KeySize
        || (session.m_pendingPreKey && session.m_localIdentity.size() != Crypto::KeySize))
        return {};
    return session;
}
