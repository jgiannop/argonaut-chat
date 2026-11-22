#pragma once

#include <QByteArray>

#include <optional>

// ML-KEM-1024 (FIPS 203), the post-quantum part of PQXDH, from mlkem-native
// (third_party/mlkem-native: a reviewed implementation, not ours). A key
// encapsulation: Bob publishes a public key; Alice makes a random shared
// secret and a ciphertext of it for that key; only Bob's secret key gets
// the same secret back out.
namespace MlKem {

constexpr int PublicKeySize = 1568;
constexpr int SecretKeySize = 3168;
constexpr int CiphertextSize = 1568;
constexpr int SharedSecretSize = 32;
constexpr int SeedSize = 64;    // d | z, for a key pair made from them (tests)
constexpr int MessageSize = 32; // m, for an encapsulation made from it (tests)

struct KeyPair
{
    QByteArray publicKey;
    QByteArray secretKey;
    static KeyPair generate(); // invalid if the random source failed
    // FIPS 203's ML-KEM.KeyGen_internal(d, z): for test vectors only.
    static KeyPair fromSeed(const QByteArray &seed);
    bool isValid() const { return publicKey.size() == PublicKeySize && secretKey.size() == SecretKeySize; }
};

struct Encapsulation
{
    QByteArray ciphertext;
    QByteArray sharedSecret;
};

// Nothing if the public key is not a valid one (wrong size, or FIPS 203's
// modulus check fails): data from a stranger.
std::optional<Encapsulation> encapsulate(const QByteArray &publicKey);
// ML-KEM.Encaps_internal(ek, m): for test vectors only.
std::optional<Encapsulation> encapsulate(const QByteArray &publicKey, const QByteArray &message);
// Nothing for wrong sizes or a damaged secret key. A changed ciphertext gives
// a different, unrelated secret (FIPS 203's implicit rejection): the
// conversation then fails to decrypt, it does not say why.
std::optional<QByteArray> decapsulate(const QByteArray &secretKey, const QByteArray &ciphertext);

// FIPS 203's input checks.
bool isValidPublicKey(const QByteArray &publicKey);
bool isValidSecretKey(const QByteArray &secretKey);

} // namespace MlKem
