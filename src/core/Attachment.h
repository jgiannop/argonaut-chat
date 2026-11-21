#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

// Attachments, as Signal sends them: the file is encrypted on the sender's
// computer with a new random key and uploaded; only the encrypted bytes ever
// reach the server. The key travels inside the (end-to-end encrypted)
// message, in a Pointer, with a digest of what was uploaded, so the
// recipient can check it before decrypting.
//
//   blob = nonce (24) | XChaCha20-Poly1305(key, nonce, padded file)
//   padded file = the file, then zeros up to Signal's size bucket
//                 (max(541, 1.05^ceil(log_1.05(size)))): the size shows little
//   digest = SHA-256(blob)
//
// Kept on disk as the blob too (the key in the encrypted database), so a
// file is never in the clear on disk.
namespace Attachment {

constexpr qint64 MaxFileBytes = 25LL * 1024 * 1024; // the largest file
constexpr int KeySize = 32;
constexpr int DigestSize = 32;
constexpr int MaxNameLength = 255;
constexpr int MaxTypeLength = 100;

// What the message says of an attachment.
struct Pointer
{
    QString id;        // the server's
    QByteArray key;    // 32 bytes
    QByteArray digest; // SHA-256 of the blob
    qint64 size = 0;   // the file's own size (before padding)
    QString name;      // the file's name, without folders
    QString type;      // a MIME type, e.g. "image/jpeg"
    bool voice = false; // a voice message (Voice.h), shown with a play button
};

struct Encrypted
{
    QByteArray blob;
    QByteArray key;
    QByteArray digest;
};

// The size of the padded file for a file of `size` bytes.
qint64 paddedSize(qint64 size);
// Nothing if the file is too large.
std::optional<Encrypted> encrypt(const QByteArray &file);
// The blob's size for a file of `size` bytes (to check an upload's length).
qint64 blobSize(qint64 size);
// Nothing if the digest, the key or the size do not match: the server, or
// someone on the way, changed it.
std::optional<QByteArray> decrypt(const QByteArray &blob, const QByteArray &key, const QByteArray &digest,
                                  qint64 size);

// A file name as it may be shown and saved: no folders, no control
// characters, not empty, at most MaxNameLength.
QString safeName(const QString &name);

// The server's ids: 32 lower-case hex digits.
bool isValidId(const QString &id);
// A MIME type as a pointer may carry it ("image/jpeg").
bool isValidType(const QString &type);

QJsonObject toJson(const Pointer &pointer);
// Strict: data from a stranger.
std::optional<Pointer> fromJson(const QJsonValue &json);

} // namespace Attachment
