#pragma once

#include "Crypto.h"

#include <QList>
#include <QString>

#include <optional>

// X3DH, "Extended Triple Diffie-Hellman", as in Signal's specification
// (signal.org/docs/specifications/x3dh): how Alice starts a conversation with
// Bob while Bob is offline, from the keys Bob left on the server. Both end up
// with the same 32-byte secret, which starts their Double Ratchet
// (RatchetSession), and the same associated data (both identity keys), which
// binds every message to the two identities.
//
//   DH1 = DH(IK_A, SPK_B)  DH2 = DH(EK_A, IK_B)  DH3 = DH(EK_A, SPK_B)
//   DH4 = DH(EK_A, OPK_B)  (when Bob had a one-time prekey left)
//   SK  = HKDF(F | DH1 | DH2 | DH3 [| DH4]),  F = 32 bytes of 0xFF
//   AD  = IK_A | IK_B
//
// PQXDH (the post-quantum version) later.
namespace X3dh {

// Bob's medium-term key, signed by his identity key; changed now and then.
struct SignedPreKey
{
    quint32 id = 0;
    Crypto::DhKeyPair pair;
    QByteArray signature; // of pair.publicKey, by the identity key
};

// Used once, then deleted by both the server and Bob.
struct OneTimePreKey
{
    quint32 id = 0;
    Crypto::DhKeyPair pair;
};

SignedPreKey makeSignedPreKey(const Crypto::IdentityKeyPair &identity, quint32 id);
QList<OneTimePreKey> makeOneTimePreKeys(quint32 firstId, int count);

// What the server gives Alice for Bob: data from a stranger, checked by
// initiate().
struct PreKeyBundle
{
    QByteArray identityKey;
    quint32 signedPreKeyId = 0;
    QByteArray signedPreKey;
    QByteArray signature;
    std::optional<quint32> oneTimePreKeyId; // none when the server ran out
    QByteArray oneTimePreKey;
};

// Alice's side.
struct Initiation
{
    QByteArray sharedKey;      // SK: starts the ratchet
    QByteArray associatedData; // AD
    // What Alice's first message must carry for Bob to do the same:
    QByteArray ephemeralKey;   // EK_A's public key
    quint32 signedPreKeyId = 0;
    std::optional<quint32> oneTimePreKeyId;
    // Bob's signed prekey: the first ratchet key Alice sends to.
    QByteArray remoteRatchetKey;
};

// Nothing if the bundle is bad (wrong sizes, a signature that does not
// match, a low-order key); `error` then says why.
std::optional<Initiation> initiate(const Crypto::IdentityKeyPair &me, const PreKeyBundle &them,
                                   QString *error = nullptr);

// Bob's side, when Alice's first message arrives: the prekeys it names
// (`oneTimePreKey` null if it named none), her identity and ephemeral keys.
struct Response
{
    QByteArray sharedKey;
    QByteArray associatedData;
};

std::optional<Response> respond(const Crypto::IdentityKeyPair &me, const SignedPreKey &signedPreKey,
                                const OneTimePreKey *oneTimePreKey, const QByteArray &theirIdentityKey,
                                const QByteArray &theirEphemeralKey);

} // namespace X3dh
