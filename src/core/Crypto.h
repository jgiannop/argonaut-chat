#pragma once

#include <QByteArray>
#include <QString>

#include <memory>
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
// Memory: the identity secret, which lives as long as the program, is kept in
// SecretBytes (locked, out of core dumps, wiped). QByteArray copies are not
// wiped by themselves; wipe() clears the temporaries we own.
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

// Bytes in libsodium's guarded memory (sodium_malloc): locked so they are
// never written to swap, left out of core dumps, between guard pages, and
// wiped when the last copy goes. Copies share the same memory; nothing
// changes it once made.
class SecretBytes
{
public:
    SecretBytes() = default;
    // A copy of `bytes` (the caller wipes its own).
    explicit SecretBytes(const QByteArray &bytes);
    // `size` zero bytes for `fill` to write into; empty if it fails.
    template<typename Fill> static SecretBytes make(int size, Fill fill)
    {
        SecretBytes out = allocate(size);
        if (out.m_size > 0 && !fill(out.m_data.get()))
            return {};
        return out;
    }

    bool isEmpty() const { return m_size == 0; }
    int size() const { return m_size; }
    const unsigned char *data() const { return m_data.get(); }
    // A copy in ordinary memory, for saving or sending: wipe it afterwards.
    QByteArray toByteArray() const;
    // Whether the memory is locked (false where the system refused).
    bool isLocked() const { return m_locked; }

private:
    static SecretBytes allocate(int size);
    std::shared_ptr<unsigned char> m_data;
    int m_size = 0;
    bool m_locked = false;
};

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
    // 64 bytes: to keep, never to send. A copy in ordinary memory: wipe it.
    QByteArray secretKey() const { return m_secret.toByteArray(); }
    bool isSecretLocked() const { return m_secret.isLocked(); }
    QByteArray sign(const QByteArray &message) const;
    // The same key as X25519, for X3DH.
    QByteArray dhSecretKey() const;
    QByteArray dhPublicKey() const;

private:
    QByteArray m_public;
    SecretBytes m_secret;
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

// Passphrases (the app lock): libsodium's Argon2id with its "interactive"
// limits (64 MiB, a fraction of a second), salted, in its own text form
// ("$argon2id$v=19$m=…"). Never the passphrase itself.
QString hashPassphrase(const QString &passphrase); // empty if it could not be made
bool verifyPassphrase(const QString &hash, const QString &passphrase);

} // namespace Crypto
