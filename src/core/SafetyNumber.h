#pragma once

#include <QByteArray>
#include <QString>

// Safety numbers, as Signal's: 60 digits made from both people's identity
// keys, the same on both phones. Compared in person (or over a call), they
// prove nobody sits in the middle; they change when either key changes.
//
// Each half: SHA-512 of (version 0 as 2 bytes | identity key | the person's
// id), then 5200 times SHA-512(previous | identity key); the first 30 bytes
// as six 5-byte numbers, each mod 100000. The two halves in sorted order.
namespace SafetyNumber {

// 60 digits; empty if a key is not 32 bytes.
QString compute(const QByteArray &myIdentityKey, const QString &myId, const QByteArray &theirIdentityKey,
                const QString &theirId);
// "12345 67890 ..." in twelve groups of five.
QString display(const QString &digits);

} // namespace SafetyNumber
