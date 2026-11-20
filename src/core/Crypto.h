#pragma once

#include <QByteArray>

#include <optional>

// The few primitives the Signal Protocol's design needs, every one from
// libsodium (a reviewed library): nothing here is invented, it only gives the
// calls Qt types. See docs/how-signal-works.md.
//
//   - X25519 for Diffie-Hellman, Ed25519 for signatures; an identity key is
//     one Ed25519 key pair, converted by libsodium to X25519 for the DH steps
//   - HMAC-SHA256, and HKDF on top of it (RFC 5869: libsodium 1.0.18, Ubuntu
//     24.04's, has no HKDF of its own)
//   - XChaCha20-Poly1305 (IETF) to encrypt each message
//
// Memory: QByteArray copies are not wiped; wipe() clears the temporaries we
// own. Locked, wiped memory for the long-term keys comes with the keyring.
namespace Crypto {

constexpr int KeySize = 32;       // X25519 keys, Ed25519 public keys, chain keys
constexpr int SignatureSize = 64; // Ed25519
constexpr int AeadNonceSize = 24; // XChaCha20-Poly1305
constexpr int AeadTagSize = 16;

// libsodium's start; every function here calls it. False: nothing works.
bool init();

QByteArray randomBytes(int count);
// Zeroes the bytes, then empties the array.
void wipe(QByteArray &bytes);

QByteArray sha512(const QByteArray &data);
QByteArray sha256(const QByteArray &data);
QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);
// RFC 5869 with SHA-256. An empty salt is 32 zero bytes. Empty if `length`
// is not 1..8160.
QByteArray hkdf(const QByteArray &inputKey, const QByteArray &salt, const QByteArray &info, int length);

// An X25519 key pair (32 + 32 bytes).
struct DhKeyPair
{
    QByteArray publicKey;
    QByteArray secretKey;
    static DhKeyPair generate();
    bool isValid() const { return publicKey.size() == KeySize && secretKey.size() == KeySize; }
};

// X25519. Empty when a key has the wrong size or the result is all zeros (a
// low-order public key from someone trying to fix the secret).
QByteArray dh(const QByteArray &secretKey, const QByteArray &publicKey);

// A device's long-term identity: an Ed25519 key pair. Its public key is who
// the device is (the safety number is made from it).
class IdentityKeyPair
{
public:
    static IdentityKeyPair generate();
    // From secretKey(): an invalid pair if it is not one.
    static IdentityKeyPair fromSecretKey(const QByteArray &secretKey);

    bool isValid() const { return m_public.size() == KeySize; }
    QByteArray publicKey() const { return m_public; } // Ed25519, 32 bytes
    QByteArray secretKey() const { return m_secret; } // 64 bytes: to keep, never to send
    QByteArray sign(const QByteArray &message) const;
    // The same key as X25519, for X3DH.
    QByteArray dhSecretKey() const;
    QByteArray dhPublicKey() const;

private:
    QByteArray m_public;
    QByteArray m_secret;
};

bool verify(const QByteArray &identityKey, const QByteArray &message, const QByteArray &signature);
// Someone's identity key as X25519; empty if it is not a valid Ed25519 key.
QByteArray dhPublicKeyOf(const QByteArray &identityKey);

// libsodium's sealed box (X25519, XSalsa20-Poly1305): anyone can encrypt
// to a public key, only its secret key opens it, and nothing says who sent
// it. For linking a device: the account's keys, to the new device's
// one-time key.
QByteArray sealTo(const QByteArray &publicKey, const QByteArray &plaintext);
std::optional<QByteArray> openSealed(const DhKeyPair &pair, const QByteArray &ciphertext);

// XChaCha20-Poly1305. `key` 32 bytes, `nonce` 24, never the same pair twice.
QByteArray seal(const QByteArray &key, const QByteArray &nonce, const QByteArray &plaintext,
                const QByteArray &associatedData);
// Nothing if the ciphertext or the associated data were changed.
std::optional<QByteArray> open(const QByteArray &key, const QByteArray &nonce, const QByteArray &ciphertext,
                               const QByteArray &associatedData);

} // namespace Crypto
