#pragma once

#include "Crypto.h"

#include <QByteArray>
#include <QList>

#include <optional>

// The Double Ratchet, as in Signal's specification
// (signal.org/docs/specifications/doubleratchet): one conversation's keys
// between two devices, started from X3DH's secret. Every message gets its own
// key (the symmetric ratchet: forward secrecy) and every reply brings a new
// Diffie-Hellman exchange (the DH ratchet: a stolen key stops working).
//
//   KDF_RK(rk, dh)  = HKDF(dh, salt rk, "ArgonautChat Ratchet") -> rk', ck
//   KDF_CK(ck)      = HMAC(ck, 0x01) -> message key, HMAC(ck, 0x02) -> ck'
//   ENCRYPT(mk, ..) = XChaCha20-Poly1305, key and nonce = HKDF(mk, "ArgonautChat Message")
//                     associated data = X3DH's AD | the header
//
// A message on the wire: version (1 byte, 1) | ratchet key (32) |
// previous chain's length (4, big endian) | number in the chain (4) |
// ciphertext with its tag.
//
// Messages may arrive late or out of order: the keys of the skipped ones are
// kept (at most MaxSkip from one chain, MaxStoredSkipped in all). A message
// that does not decrypt changes nothing, so a stranger cannot break the
// session by sending rubbish; one decrypted already does not decrypt again.
class RatchetSession
{
public:
    static constexpr quint32 MaxSkip = 1000;
    static constexpr int MaxStoredSkipped = 2000;
    static constexpr int HeaderSize = 1 + Crypto::KeySize + 4 + 4;

    RatchetSession() = default; // invalid

    // Alice, after X3dh::initiate: she can send at once.
    static RatchetSession forInitiator(const QByteArray &sharedKey, const QByteArray &remoteRatchetKey,
                                       const QByteArray &associatedData);
    // Bob, after X3dh::respond: his signed prekey is his first ratchet key.
    // He can send once Alice's first message has been decrypted.
    static RatchetSession forResponder(const QByteArray &sharedKey, const Crypto::DhKeyPair &ratchetKey,
                                       const QByteArray &associatedData);

    bool isValid() const;
    bool canSend() const { return isValid() && m_sendChain.size() == Crypto::KeySize; }

    // The message for the wire; empty if it cannot send.
    QByteArray encrypt(const QByteArray &plaintext);
    // Nothing if the message is not good for this session; then it is unchanged.
    std::optional<QByteArray> decrypt(const QByteArray &message);

    // Everything, secrets included: for the (encrypted) local database only.
    QByteArray serialize() const;
    // An invalid session if `data` is not one.
    static RatchetSession deserialize(const QByteArray &data);

private:
    struct SkippedKey
    {
        QByteArray ratchetKey;
        quint32 number = 0;
        QByteArray messageKey;
    };

    bool skipTo(quint32 until);
    bool turn(const QByteArray &remoteRatchetKey);

    Crypto::DhKeyPair m_self;  // DHs
    QByteArray m_remote;       // DHr (empty until the first message, for Bob)
    QByteArray m_root;         // RK
    QByteArray m_sendChain;    // CKs
    QByteArray m_receiveChain; // CKr
    quint32 m_sent = 0;        // Ns
    quint32 m_received = 0;    // Nr
    quint32 m_previous = 0;    // PN
    QList<SkippedKey> m_skipped; // oldest first
    QByteArray m_associatedData;
};
