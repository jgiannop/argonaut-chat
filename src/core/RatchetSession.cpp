#include "RatchetSession.h"

#include <QDataStream>
#include <QIODevice>
#include <QtEndian>

#include <limits>

namespace {

constexpr char kVersion = 1;
constexpr quint32 kSerialMagic = 0x41524431; // "ARD1"
const QByteArray kRootInfo = QByteArrayLiteral("ArgonautChat Ratchet");
const QByteArray kMessageInfo = QByteArrayLiteral("ArgonautChat Message");

struct Header
{
    QByteArray ratchetKey;
    quint32 previous = 0;
    quint32 number = 0;
};

QByteArray encodeHeader(const Header &header)
{
    QByteArray out;
    out.reserve(RatchetSession::HeaderSize);
    out += kVersion;
    out += header.ratchetKey;
    char number[4];
    qToBigEndian(header.previous, number);
    out += QByteArray(number, 4);
    qToBigEndian(header.number, number);
    out += QByteArray(number, 4);
    return out;
}

std::optional<Header> decodeHeader(const QByteArray &message)
{
    if (message.size() < RatchetSession::HeaderSize + Crypto::AeadTagSize || message.at(0) != kVersion)
        return std::nullopt;
    Header header;
    header.ratchetKey = message.mid(1, Crypto::KeySize);
    header.previous = qFromBigEndian<quint32>(message.constData() + 1 + Crypto::KeySize);
    header.number = qFromBigEndian<quint32>(message.constData() + 1 + Crypto::KeySize + 4);
    return header;
}

// KDF_RK: the new root key and a chain key.
bool rootStep(QByteArray &root, QByteArray &chain, QByteArray dhOutput)
{
    if (dhOutput.isEmpty())
        return false;
    QByteArray out = Crypto::hkdf(dhOutput, root, kRootInfo, 2 * Crypto::KeySize);
    Crypto::wipe(dhOutput);
    root = out.left(Crypto::KeySize);
    chain = out.mid(Crypto::KeySize);
    Crypto::wipe(out);
    return true;
}

// KDF_CK: moves the chain one step and returns that step's message key.
QByteArray chainStep(QByteArray &chain)
{
    QByteArray messageKey = Crypto::hmacSha256(chain, QByteArray(1, '\x01'));
    chain = Crypto::hmacSha256(chain, QByteArray(1, '\x02'));
    return messageKey;
}

QByteArray sealMessage(QByteArray messageKey, const QByteArray &plaintext, const QByteArray &associatedData)
{
    QByteArray keys = Crypto::hkdf(messageKey, QByteArray(), kMessageInfo, Crypto::KeySize + Crypto::AeadNonceSize);
    Crypto::wipe(messageKey);
    const QByteArray out = Crypto::seal(keys.left(Crypto::KeySize), keys.mid(Crypto::KeySize), plaintext,
                                        associatedData);
    Crypto::wipe(keys);
    return out;
}

std::optional<QByteArray> openMessage(const QByteArray &messageKey, const QByteArray &ciphertext,
                                      const QByteArray &associatedData)
{
    QByteArray keys = Crypto::hkdf(messageKey, QByteArray(), kMessageInfo, Crypto::KeySize + Crypto::AeadNonceSize);
    auto out = Crypto::open(keys.left(Crypto::KeySize), keys.mid(Crypto::KeySize), ciphertext, associatedData);
    Crypto::wipe(keys);
    return out;
}

bool keySizeOrEmpty(const QByteArray &key)
{
    return key.isEmpty() || key.size() == Crypto::KeySize;
}

} // namespace

RatchetSession RatchetSession::forInitiator(const QByteArray &sharedKey, const QByteArray &remoteRatchetKey,
                                            const QByteArray &associatedData)
{
    RatchetSession session;
    if (sharedKey.size() != Crypto::KeySize || remoteRatchetKey.size() != Crypto::KeySize
        || associatedData.isEmpty())
        return session;
    session.m_self = Crypto::DhKeyPair::generate();
    session.m_remote = remoteRatchetKey;
    session.m_root = sharedKey;
    session.m_associatedData = associatedData;
    if (!rootStep(session.m_root, session.m_sendChain, Crypto::dh(session.m_self.secretKey, remoteRatchetKey)))
        return RatchetSession();
    return session;
}

RatchetSession RatchetSession::forResponder(const QByteArray &sharedKey, const Crypto::DhKeyPair &ratchetKey,
                                            const QByteArray &associatedData)
{
    RatchetSession session;
    if (sharedKey.size() != Crypto::KeySize || !ratchetKey.isValid() || associatedData.isEmpty())
        return session;
    session.m_self = ratchetKey;
    session.m_root = sharedKey;
    session.m_associatedData = associatedData;
    return session;
}

bool RatchetSession::isValid() const
{
    return m_root.size() == Crypto::KeySize && m_self.isValid() && !m_associatedData.isEmpty();
}

QByteArray RatchetSession::encrypt(const QByteArray &plaintext)
{
    if (!canSend() || m_sent == std::numeric_limits<quint32>::max())
        return {};
    QByteArray messageKey = chainStep(m_sendChain);
    const QByteArray header = encodeHeader({m_self.publicKey, m_previous, m_sent});
    ++m_sent;
    return header + sealMessage(messageKey, plaintext, m_associatedData + header);
}

std::optional<QByteArray> RatchetSession::decrypt(const QByteArray &message)
{
    if (!isValid())
        return std::nullopt;
    const std::optional<Header> header = decodeHeader(message);
    if (!header)
        return std::nullopt;
    const QByteArray associatedData = m_associatedData + message.left(HeaderSize);
    const QByteArray ciphertext = message.mid(HeaderSize);

    // One that came late: its key was kept.
    for (int i = 0; i < m_skipped.size(); ++i) {
        const SkippedKey &skipped = m_skipped.at(i);
        if (skipped.number == header->number && skipped.ratchetKey == header->ratchetKey) {
            auto plaintext = openMessage(skipped.messageKey, ciphertext, associatedData);
            if (plaintext) {
                Crypto::wipe(m_skipped[i].messageKey);
                m_skipped.removeAt(i);
            }
            return plaintext;
        }
    }

    // Worked on a copy, kept only if the message decrypts.
    RatchetSession next = *this;
    if (header->ratchetKey != next.m_remote) {
        if (!next.skipTo(header->previous) || !next.turn(header->ratchetKey))
            return std::nullopt;
    }
    if (!next.skipTo(header->number) || next.m_receiveChain.isEmpty())
        return std::nullopt;
    const QByteArray messageKey = chainStep(next.m_receiveChain);
    ++next.m_received;
    auto plaintext = openMessage(messageKey, ciphertext, associatedData);
    if (plaintext)
        *this = std::move(next);
    return plaintext;
}

bool RatchetSession::skipTo(quint32 until)
{
    if (quint64(m_received) + MaxSkip < until)
        return false;
    if (m_receiveChain.isEmpty())
        return true;
    while (m_received < until) {
        m_skipped.append({m_remote, m_received, chainStep(m_receiveChain)});
        ++m_received;
    }
    while (m_skipped.size() > MaxStoredSkipped) {
        Crypto::wipe(m_skipped.first().messageKey);
        m_skipped.removeFirst();
    }
    return true;
}

bool RatchetSession::turn(const QByteArray &remoteRatchetKey)
{
    m_previous = m_sent;
    m_sent = 0;
    m_received = 0;
    m_remote = remoteRatchetKey;
    if (!rootStep(m_root, m_receiveChain, Crypto::dh(m_self.secretKey, m_remote)))
        return false;
    m_self = Crypto::DhKeyPair::generate();
    return rootStep(m_root, m_sendChain, Crypto::dh(m_self.secretKey, m_remote));
}

QByteArray RatchetSession::serialize() const
{
    if (!isValid())
        return {};
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_4);
    out << kSerialMagic << m_self.publicKey << m_self.secretKey << m_remote << m_root << m_sendChain
        << m_receiveChain << m_sent << m_received << m_previous << m_associatedData << qint32(m_skipped.size());
    for (const SkippedKey &skipped : m_skipped)
        out << skipped.ratchetKey << skipped.number << skipped.messageKey;
    return data;
}

RatchetSession RatchetSession::deserialize(const QByteArray &data)
{
    QDataStream in(data);
    in.setVersion(QDataStream::Qt_6_4);
    RatchetSession session;
    quint32 magic = 0;
    qint32 skippedCount = 0;
    in >> magic;
    if (magic != kSerialMagic)
        return {};
    in >> session.m_self.publicKey >> session.m_self.secretKey >> session.m_remote >> session.m_root
        >> session.m_sendChain >> session.m_receiveChain >> session.m_sent >> session.m_received
        >> session.m_previous >> session.m_associatedData >> skippedCount;
    if (in.status() != QDataStream::Ok || skippedCount < 0 || skippedCount > MaxStoredSkipped)
        return {};
    for (qint32 i = 0; i < skippedCount; ++i) {
        SkippedKey skipped;
        in >> skipped.ratchetKey >> skipped.number >> skipped.messageKey;
        if (in.status() != QDataStream::Ok || skipped.ratchetKey.size() != Crypto::KeySize
            || skipped.messageKey.size() != Crypto::KeySize)
            return {};
        session.m_skipped.append(skipped);
    }
    if (in.status() != QDataStream::Ok || !in.atEnd() || !session.isValid() || !keySizeOrEmpty(session.m_remote)
        || !keySizeOrEmpty(session.m_sendChain) || !keySizeOrEmpty(session.m_receiveChain))
        return {};
    return session;
}
