#include "Crypto.h"

#include <sodium.h>

#include <cstring>

namespace Crypto {

namespace {

const unsigned char *bytes(const QByteArray &array)
{
    return reinterpret_cast<const unsigned char *>(array.constData());
}

unsigned char *bytes(QByteArray &array)
{
    return reinterpret_cast<unsigned char *>(array.data());
}

} // namespace

bool init()
{
    static const bool ok = sodium_init() >= 0;
    return ok;
}

QByteArray randomBytes(int count)
{
    init();
    QByteArray out(count, Qt::Uninitialized);
    randombytes_buf(out.data(), size_t(out.size()));
    return out;
}

void wipe(QByteArray &array)
{
    if (!array.isEmpty())
        sodium_memzero(array.data(), size_t(array.size()));
    array.clear();
}

SecretBytes SecretBytes::allocate(int size)
{
    SecretBytes out;
    if (size <= 0 || !init())
        return out;
    auto *memory = static_cast<unsigned char *>(sodium_malloc(size_t(size)));
    if (!memory)
        return out;
    sodium_memzero(memory, size_t(size));
    // sodium_malloc locks the memory if it can and goes on if not: ask again
    // to know (locking twice is harmless).
    out.m_locked = sodium_mlock(memory, size_t(size)) == 0;
    out.m_data = std::shared_ptr<unsigned char>(memory, [](unsigned char *p) { sodium_free(p); }); // wipes
    out.m_size = size;
    return out;
}

SecretBytes::SecretBytes(const QByteArray &bytes)
{
    *this = make(int(bytes.size()), [&bytes](unsigned char *out) {
        memcpy(out, bytes.constData(), size_t(bytes.size()));
        return true;
    });
}

QByteArray SecretBytes::toByteArray() const
{
    if (isEmpty())
        return {};
    return QByteArray(reinterpret_cast<const char *>(data()), m_size);
}

QByteArray sha512(const QByteArray &data)
{
    init();
    QByteArray out(crypto_hash_sha512_BYTES, Qt::Uninitialized);
    crypto_hash_sha512(bytes(out), bytes(data), quint64(data.size()));
    return out;
}

QByteArray sha256(const QByteArray &data)
{
    init();
    QByteArray out(crypto_hash_sha256_BYTES, Qt::Uninitialized);
    crypto_hash_sha256(bytes(out), bytes(data), quint64(data.size()));
    return out;
}

QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
{
    init();
    crypto_auth_hmacsha256_state state;
    QByteArray out(crypto_auth_hmacsha256_BYTES, Qt::Uninitialized);
    crypto_auth_hmacsha256_init(&state, bytes(key), size_t(key.size()));
    crypto_auth_hmacsha256_update(&state, bytes(data), quint64(data.size()));
    crypto_auth_hmacsha256_final(&state, bytes(out));
    sodium_memzero(&state, sizeof state);
    return out;
}

QByteArray hkdf(const QByteArray &inputKey, const QByteArray &salt, const QByteArray &info, int length)
{
    constexpr int hashSize = crypto_auth_hmacsha256_BYTES;
    if (length <= 0 || length > 255 * hashSize)
        return {};
    // Extract.
    QByteArray prk = hmacSha256(salt.isEmpty() ? QByteArray(hashSize, '\0') : salt, inputKey);
    // Expand: T(i) = HMAC(PRK, T(i-1) | info | i).
    QByteArray okm;
    QByteArray block;
    for (int i = 1; okm.size() < length; ++i) {
        QByteArray input = block + info + char(i);
        block = hmacSha256(prk, input);
        wipe(input);
        okm += block;
    }
    wipe(block);
    wipe(prk);
    okm.truncate(length);
    return okm;
}

DhKeyPair DhKeyPair::generate()
{
    init();
    DhKeyPair pair;
    pair.publicKey = QByteArray(crypto_box_PUBLICKEYBYTES, Qt::Uninitialized);
    pair.secretKey = QByteArray(crypto_box_SECRETKEYBYTES, Qt::Uninitialized);
    crypto_box_keypair(bytes(pair.publicKey), bytes(pair.secretKey));
    return pair;
}

QByteArray dh(const QByteArray &secretKey, const QByteArray &publicKey)
{
    init();
    if (secretKey.size() != crypto_scalarmult_SCALARBYTES || publicKey.size() != crypto_scalarmult_BYTES)
        return {};
    QByteArray out(crypto_scalarmult_BYTES, Qt::Uninitialized);
    if (crypto_scalarmult(bytes(out), bytes(secretKey), bytes(publicKey)) != 0) {
        wipe(out);
        return {};
    }
    return out;
}

IdentityKeyPair IdentityKeyPair::generate()
{
    init();
    IdentityKeyPair pair;
    QByteArray pub(crypto_sign_PUBLICKEYBYTES, Qt::Uninitialized);
    pair.m_secret = SecretBytes::make(crypto_sign_SECRETKEYBYTES, [&pub](unsigned char *secret) {
        return crypto_sign_keypair(bytes(pub), secret) == 0;
    });
    if (!pair.m_secret.isEmpty())
        pair.m_public = pub;
    return pair;
}

IdentityKeyPair IdentityKeyPair::fromSecretKey(const QByteArray &secretKey)
{
    init();
    IdentityKeyPair pair;
    if (secretKey.size() != crypto_sign_SECRETKEYBYTES)
        return pair;
    // Made again from its seed: it must give the same key, or it is not one.
    const SecretBytes seed = SecretBytes::make(crypto_sign_SEEDBYTES, [&secretKey](unsigned char *out) {
        return crypto_sign_ed25519_sk_to_seed(out, bytes(secretKey)) == 0;
    });
    if (seed.isEmpty())
        return pair;
    QByteArray pub(crypto_sign_PUBLICKEYBYTES, Qt::Uninitialized);
    const SecretBytes secret = SecretBytes::make(crypto_sign_SECRETKEYBYTES, [&pub, &seed](unsigned char *out) {
        return crypto_sign_seed_keypair(bytes(pub), out, seed.data()) == 0;
    });
    if (secret.isEmpty() || sodium_memcmp(secret.data(), secretKey.constData(), size_t(secret.size())) != 0)
        return pair;
    pair.m_public = pub;
    pair.m_secret = secret;
    return pair;
}

QByteArray IdentityKeyPair::sign(const QByteArray &message) const
{
    if (!isValid())
        return {};
    QByteArray signature(crypto_sign_BYTES, Qt::Uninitialized);
    crypto_sign_detached(bytes(signature), nullptr, bytes(message), quint64(message.size()), m_secret.data());
    return signature;
}

QByteArray IdentityKeyPair::dhSecretKey() const
{
    if (!isValid())
        return {};
    QByteArray out(crypto_scalarmult_curve25519_BYTES, Qt::Uninitialized);
    crypto_sign_ed25519_sk_to_curve25519(bytes(out), m_secret.data());
    return out;
}

QByteArray IdentityKeyPair::dhPublicKey() const
{
    return dhPublicKeyOf(m_public);
}

bool verify(const QByteArray &identityKey, const QByteArray &message, const QByteArray &signature)
{
    init();
    if (identityKey.size() != crypto_sign_PUBLICKEYBYTES || signature.size() != crypto_sign_BYTES)
        return false;
    return crypto_sign_verify_detached(bytes(signature), bytes(message), quint64(message.size()),
                                       bytes(identityKey))
           == 0;
}

QByteArray dhPublicKeyOf(const QByteArray &identityKey)
{
    init();
    if (identityKey.size() != crypto_sign_PUBLICKEYBYTES)
        return {};
    QByteArray out(crypto_scalarmult_curve25519_BYTES, Qt::Uninitialized);
    if (crypto_sign_ed25519_pk_to_curve25519(bytes(out), bytes(identityKey)) != 0)
        return {};
    return out;
}

QByteArray sealTo(const QByteArray &publicKey, const QByteArray &plaintext)
{
    init();
    if (publicKey.size() != crypto_box_PUBLICKEYBYTES)
        return {};
    QByteArray out(plaintext.size() + crypto_box_SEALBYTES, Qt::Uninitialized);
    if (crypto_box_seal(bytes(out), bytes(plaintext), quint64(plaintext.size()), bytes(publicKey)) != 0)
        return {};
    return out;
}

std::optional<QByteArray> openSealed(const DhKeyPair &pair, const QByteArray &ciphertext)
{
    init();
    if (!pair.isValid() || ciphertext.size() < int(crypto_box_SEALBYTES))
        return std::nullopt;
    QByteArray out(ciphertext.size() - crypto_box_SEALBYTES, Qt::Uninitialized);
    if (crypto_box_seal_open(bytes(out), bytes(ciphertext), quint64(ciphertext.size()), bytes(pair.publicKey),
                             bytes(pair.secretKey))
        != 0)
        return std::nullopt;
    return out;
}

QByteArray seal(const QByteArray &key, const QByteArray &nonce, const QByteArray &plaintext,
                const QByteArray &associatedData)
{
    init();
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES
        || nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES)
        return {};
    QByteArray out(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, Qt::Uninitialized);
    unsigned long long length = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(bytes(out), &length, bytes(plaintext), quint64(plaintext.size()),
                                               bytes(associatedData), quint64(associatedData.size()), nullptr,
                                               bytes(nonce), bytes(key));
    out.truncate(int(length));
    return out;
}

std::optional<QByteArray> open(const QByteArray &key, const QByteArray &nonce, const QByteArray &ciphertext,
                               const QByteArray &associatedData)
{
    init();
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES
        || nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
        || ciphertext.size() < int(crypto_aead_xchacha20poly1305_ietf_ABYTES))
        return std::nullopt;
    QByteArray out(ciphertext.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES, Qt::Uninitialized);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(bytes(out), &length, nullptr, bytes(ciphertext),
                                                   quint64(ciphertext.size()), bytes(associatedData),
                                                   quint64(associatedData.size()), bytes(nonce), bytes(key))
        != 0)
        return std::nullopt;
    out.truncate(int(length));
    return out;
}

QString hashPassphrase(const QString &passphrase)
{
    if (!init())
        return {};
    QByteArray utf8 = passphrase.toUtf8();
    char out[crypto_pwhash_STRBYTES];
    const int result = crypto_pwhash_str(out, utf8.constData(), quint64(utf8.size()),
                                         crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE);
    wipe(utf8);
    return result == 0 ? QString::fromLatin1(out) : QString();
}

bool verifyPassphrase(const QString &hash, const QString &passphrase)
{
    const QByteArray stored = hash.toLatin1();
    if (!init() || stored.isEmpty() || stored.size() >= crypto_pwhash_STRBYTES)
        return false;
    QByteArray utf8 = passphrase.toUtf8();
    const bool ok = crypto_pwhash_str_verify(stored.constData(), utf8.constData(), quint64(utf8.size())) == 0;
    wipe(utf8);
    return ok;
}

} // namespace Crypto
