#include "SafetyNumber.h"

#include "Crypto.h"

#include <QStringList>

namespace SafetyNumber {

namespace {

constexpr int kIterations = 5200;

QString half(const QByteArray &identityKey, const QString &id)
{
    QByteArray hash = Crypto::sha512(QByteArray(2, '\0') + identityKey + id.toUtf8());
    for (int i = 0; i < kIterations; ++i)
        hash = Crypto::sha512(hash + identityKey);
    QString digits;
    for (int chunk = 0; chunk < 6; ++chunk) {
        quint64 value = 0;
        for (int b = 0; b < 5; ++b)
            value = (value << 8) | quint8(hash.at(chunk * 5 + b));
        digits += QStringLiteral("%1").arg(value % 100000, 5, 10, QLatin1Char('0'));
    }
    return digits;
}

} // namespace

QString compute(const QByteArray &myIdentityKey, const QString &myId, const QByteArray &theirIdentityKey,
                const QString &theirId)
{
    if (myIdentityKey.size() != Crypto::KeySize || theirIdentityKey.size() != Crypto::KeySize)
        return {};
    const QString mine = half(myIdentityKey, myId);
    const QString theirs = half(theirIdentityKey, theirId);
    return mine < theirs ? mine + theirs : theirs + mine;
}

QString display(const QString &digits)
{
    QStringList groups;
    for (int i = 0; i < digits.size(); i += 5)
        groups << digits.mid(i, 5);
    return groups.join(QLatin1Char(' '));
}

} // namespace SafetyNumber
