#pragma once

#include "Crypto.h"
#include "MlKem.h"

#include <QList>
#include <QString>

#include <optional>

// PQXDH, "Post-Quantum Extended Diffie-Hellman", as in Signal's specification
// (signal.org/docs/specifications/pqxdh): X3DH plus ML-KEM-1024. How Alice
// starts a conversation with Bob while Bob is offline, from the keys Bob left
// on the server. Both end up with the same 32-byte secret, which starts their
// Double Ratchet (RatchetSession), and the same associated data (both
// identity keys), which binds every message to the two identities. An
// attacker must break both X25519 (now) and ML-KEM (later, with a quantum
// computer) to learn it.
//
//   DH1 = DH(IK_A, SPK_B)  DH2 = DH(EK_A, IK_B)  DH3 = DH(EK_A, SPK_B)
//   DH4 = DH(EK_A, OPK_B)  (when Bob had a one-time prekey left)
//   (CT, SS) = ML-KEM-Encaps(PQPK_B)  (PQPK_B: a one-time KEM prekey, or the
//              last-resort one that goes with the signed prekey)
//   SK  = HKDF(F | DH1 | DH2 | DH3 [| DH4] | SS),  F = 32 bytes of 0xFF
//   AD  = IK_A | IK_B
//
// Every KEM prekey is signed by the identity key (`kemSigned`). Here each
// one-time prekey carries a one-time KEM prekey, and the signed prekey the
// last-resort one: they are made, uploaded, counted and deleted together.
// Plain X3DH (without a KEM) is never started: PQXDH is required.
namespace X3dh {

// What an identity key signs for a KEM prekey: a type byte (0x08, as
// Signal's EncodeKEM for ML-KEM-1024) and the public key.
QByteArray kemSigned(const QByteArray &kemPublicKey);

// Bob's medium-term key, signed by his identity key; changed now and then.
struct SignedPreKey
{
    quint32 id = 0;
    Crypto::DhKeyPair pair;
    QByteArray signature; // of pair.publicKey, by the identity key
    // The last-resort KEM prekey (none in keys made before PQXDH).
    MlKem::KeyPair kemPair;
    QByteArray kemSignature; // of kemSigned(kemPair.publicKey)
    bool hasKem() const { return kemPair.isValid(); }
};

// Used once, then deleted by both the server and Bob.
struct OneTimePreKey
{
    quint32 id = 0;
    Crypto::DhKeyPair pair;
    // Its one-time KEM prekey (none in keys made before PQXDH).
    MlKem::KeyPair kemPair;
    QByteArray kemSignature;
    bool hasKem() const { return kemPair.isValid(); }
};

SignedPreKey makeSignedPreKey(const Crypto::IdentityKeyPair &identity, quint32 id);
QList<OneTimePreKey> makeOneTimePreKeys(const Crypto::IdentityKeyPair &identity, quint32 firstId, int count);

// What the server gives Alice for Bob: data from a stranger, checked by
// initiate().
struct PreKeyBundle
{
    QByteArray identityKey;
    quint32 signedPreKeyId = 0;
    QByteArray signedPreKey;
    QByteArray signature;
    QByteArray signedKemKey; // the last-resort KEM prekey (empty: a version before PQXDH)
    QByteArray signedKemSignature;
    std::optional<quint32> oneTimePreKeyId; // none when the server ran out
    QByteArray oneTimePreKey;
    QByteArray oneTimeKemKey; // empty: none with it
    QByteArray oneTimeKemSignature;
};

// The bundle a device's keys make (what the server hands out): tests, and
// a check of what the server gives.
PreKeyBundle bundleFor(const QByteArray &identityKey, const SignedPreKey &signedPreKey,
                       const OneTimePreKey *oneTimePreKey = nullptr);

// Alice's side.
struct Initiation
{
    QByteArray sharedKey;      // SK: starts the ratchet
    QByteArray associatedData; // AD
    // What Alice's first message must carry for Bob to do the same:
    QByteArray ephemeralKey;   // EK_A's public key
    quint32 signedPreKeyId = 0;
    std::optional<quint32> oneTimePreKeyId;
    QByteArray kemCiphertext; // CT
    bool kemLastResort = false; // CT is for the signed prekey's KEM key, not the one-time prekey's
    // Bob's signed prekey: the first ratchet key Alice sends to.
    QByteArray remoteRatchetKey;
};

// What `error` says for a bundle with no KEM prekey: they need a newer version.
constexpr const char *NoKemError = "no-kem";

// Nothing if the bundle is bad (wrong sizes, a signature that does not
// match, a low-order key, no KEM prekey: NoKemError); `error` then says why.
std::optional<Initiation> initiate(const Crypto::IdentityKeyPair &me, const PreKeyBundle &them,
                                   QString *error = nullptr);

// Bob's side, when Alice's first message arrives: the prekeys it names
// (`oneTimePreKey` null if it named none), the secret half of the KEM prekey
// it names and its ciphertext, her identity and ephemeral keys.
struct Response
{
    QByteArray sharedKey;
    QByteArray associatedData;
};

std::optional<Response> respond(const Crypto::IdentityKeyPair &me, const SignedPreKey &signedPreKey,
                                const OneTimePreKey *oneTimePreKey, const QByteArray &kemSecretKey,
                                const QByteArray &kemCiphertext, const QByteArray &theirIdentityKey,
                                const QByteArray &theirEphemeralKey);

} // namespace X3dh
