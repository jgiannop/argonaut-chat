#pragma once

#include "Protocol.h"
#include "RatchetSession.h"

#include <functional>
#include <optional>

// One encrypted conversation with one device of another person: X3DH to start
// it, the Double Ratchet for every message, Protocol::CipherMessage on the
// wire.
//
// Alice: initiate() from Bob's bundle, then encrypt(). Until Bob first
// answers, her messages are prekey messages (each carries the X3DH header, so
// any of them can start the session for Bob, whichever arrives first).
//
// Bob: a prekey message from someone he has no session with goes to accept(),
// which finds his prekeys through the lookups. The caller deletes the
// one-time prekey it used, and checks remoteIdentityKey() against the
// identity it knew for that person (a change means a new safety number).
// Later prekey messages of the same start decrypt with decrypt().
class Session
{
public:
    using SignedPreKeyLookup = std::function<std::optional<X3dh::SignedPreKey>(quint32 id)>;
    using OneTimePreKeyLookup = std::function<std::optional<X3dh::OneTimePreKey>(quint32 id)>;

    struct Accepted;

    Session() = default; // invalid

    static std::optional<Session> initiate(const Crypto::IdentityKeyPair &me, const X3dh::PreKeyBundle &them,
                                           QString *error = nullptr);
    static std::optional<Accepted> accept(const Crypto::IdentityKeyPair &me, const Protocol::CipherMessage &message,
                                          const SignedPreKeyLookup &signedPreKey,
                                          const OneTimePreKeyLookup &oneTimePreKey);

    bool isValid() const { return m_ratchet.isValid() && m_remoteIdentity.size() == Crypto::KeySize; }
    QByteArray remoteIdentityKey() const { return m_remoteIdentity; }
    // Alice before Bob's first answer.
    bool sendsPreKeyMessages() const { return m_pendingPreKey; }

    // An encoded CipherMessage; empty if it cannot send yet.
    QByteArray encrypt(const QByteArray &plaintext);
    // Nothing if the message is not for this session; it is then unchanged.
    std::optional<QByteArray> decrypt(const Protocol::CipherMessage &message);

    // Secrets included: for the encrypted local database only.
    QByteArray serialize() const;
    static Session deserialize(const QByteArray &data);

private:
    RatchetSession m_ratchet;
    QByteArray m_remoteIdentity;
    QByteArray m_baseKey; // X3DH's ephemeral key (Alice's), on both sides
    // Alice, until she hears from Bob:
    bool m_pendingPreKey = false;
    QByteArray m_localIdentity;
    quint32 m_signedPreKeyId = 0;
    std::optional<quint32> m_oneTimePreKeyId;
};

struct Session::Accepted
{
    Session session;
    QByteArray plaintext;
    std::optional<quint32> usedOneTimePreKey; // to delete
};
