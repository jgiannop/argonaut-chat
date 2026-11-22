#include "MlKem.h"

#include "Crypto.h"

#include <mlkem_native.h>

static_assert(MLKEM1024_PUBLICKEYBYTES == MlKem::PublicKeySize, "ML-KEM-1024 sizes");
static_assert(MLKEM1024_SECRETKEYBYTES == MlKem::SecretKeySize, "ML-KEM-1024 sizes");
static_assert(MLKEM1024_CIPHERTEXTBYTES == MlKem::CiphertextSize, "ML-KEM-1024 sizes");
static_assert(MLKEM_BYTES == MlKem::SharedSecretSize, "ML-KEM-1024 sizes");

namespace MlKem {

namespace {

uint8_t *bytes(QByteArray &array)
{
    return reinterpret_cast<uint8_t *>(array.data());
}

const uint8_t *bytes(const QByteArray &array)
{
    return reinterpret_cast<const uint8_t *>(array.constData());
}

} // namespace

KeyPair KeyPair::generate()
{
    KeyPair pair;
    pair.publicKey.resize(PublicKeySize);
    pair.secretKey.resize(SecretKeySize);
    if (!Crypto::init() || chat_mlkem_keypair(bytes(pair.publicKey), bytes(pair.secretKey)) != 0) {
        Crypto::wipe(pair.secretKey);
        return {};
    }
    return pair;
}

KeyPair KeyPair::fromSeed(const QByteArray &seed)
{
    if (seed.size() != SeedSize)
        return {};
    KeyPair pair;
    pair.publicKey.resize(PublicKeySize);
    pair.secretKey.resize(SecretKeySize);
    if (chat_mlkem_keypair_derand(bytes(pair.publicKey), bytes(pair.secretKey), bytes(seed)) != 0) {
        Crypto::wipe(pair.secretKey);
        return {};
    }
    return pair;
}

std::optional<Encapsulation> encapsulate(const QByteArray &publicKey)
{
    if (publicKey.size() != PublicKeySize || !Crypto::init())
        return std::nullopt;
    Encapsulation out;
    out.ciphertext.resize(CiphertextSize);
    out.sharedSecret.resize(SharedSecretSize);
    if (chat_mlkem_enc(bytes(out.ciphertext), bytes(out.sharedSecret), bytes(publicKey)) != 0) {
        Crypto::wipe(out.sharedSecret);
        return std::nullopt;
    }
    return out;
}

std::optional<Encapsulation> encapsulate(const QByteArray &publicKey, const QByteArray &message)
{
    if (publicKey.size() != PublicKeySize || message.size() != MessageSize)
        return std::nullopt;
    Encapsulation out;
    out.ciphertext.resize(CiphertextSize);
    out.sharedSecret.resize(SharedSecretSize);
    if (chat_mlkem_enc_derand(bytes(out.ciphertext), bytes(out.sharedSecret), bytes(publicKey), bytes(message)) != 0) {
        Crypto::wipe(out.sharedSecret);
        return std::nullopt;
    }
    return out;
}

std::optional<QByteArray> decapsulate(const QByteArray &secretKey, const QByteArray &ciphertext)
{
    if (secretKey.size() != SecretKeySize || ciphertext.size() != CiphertextSize)
        return std::nullopt;
    QByteArray secret(SharedSecretSize, Qt::Uninitialized);
    if (chat_mlkem_dec(bytes(secret), bytes(ciphertext), bytes(secretKey)) != 0) {
        Crypto::wipe(secret);
        return std::nullopt;
    }
    return secret;
}

bool isValidPublicKey(const QByteArray &publicKey)
{
    return publicKey.size() == PublicKeySize && chat_mlkem_check_pk(bytes(publicKey)) == 0;
}

bool isValidSecretKey(const QByteArray &secretKey)
{
    return secretKey.size() == SecretKeySize && chat_mlkem_check_sk(bytes(secretKey)) == 0;
}

} // namespace MlKem
