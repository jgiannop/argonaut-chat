#include "Attachment.h"

#include "Crypto.h"
#include "Protocol.h"

#include <QRegularExpression>

#include <cmath>

namespace Attachment {

qint64 paddedSize(qint64 size)
{
    if (size <= 541)
        return 541;
    const double bucket = std::floor(std::pow(1.05, std::ceil(std::log(double(size)) / std::log(1.05))));
    return qMax(size, qint64(bucket));
}

qint64 blobSize(qint64 size)
{
    return Crypto::AeadNonceSize + paddedSize(size) + Crypto::AeadTagSize;
}

std::optional<Encrypted> encrypt(const QByteArray &file)
{
    if (file.size() > MaxFileBytes)
        return std::nullopt;
    QByteArray padded = file;
    padded.append(QByteArray(int(paddedSize(file.size()) - file.size()), '\0'));
    Encrypted out;
    out.key = Crypto::randomBytes(KeySize);
    const QByteArray nonce = Crypto::randomBytes(Crypto::AeadNonceSize);
    out.blob = nonce + Crypto::seal(out.key, nonce, padded, QByteArray());
    Crypto::wipe(padded);
    out.digest = Crypto::sha256(out.blob);
    return out;
}

std::optional<QByteArray> decrypt(const QByteArray &blob, const QByteArray &key, const QByteArray &digest,
                                  qint64 size)
{
    if (key.size() != KeySize || digest.size() != DigestSize || size < 0 || size > MaxFileBytes
        || blob.size() != blobSize(size) || Crypto::sha256(blob) != digest)
        return std::nullopt;
    auto padded = Crypto::open(key, blob.left(Crypto::AeadNonceSize), blob.mid(Crypto::AeadNonceSize), QByteArray());
    if (!padded)
        return std::nullopt;
    padded->truncate(int(size));
    return padded;
}

QString safeName(const QString &name)
{
    QString out = name;
    // No folders: only the last part.
    const qsizetype slash = qMax(out.lastIndexOf(QLatin1Char('/')), out.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0)
        out = out.mid(slash + 1);
    // No control characters, and no direction marks (a right-to-left
    // override, U+202E, can make "invoice<U+202E>fdp.exe" show as "invoiceexe.pdf").
    QString clean;
    for (const QChar c : out) {
        const char16_t u = c.unicode();
        const bool control = u < 0x20 || u == 0x7f || (u >= 0x80 && u < 0xa0);
        const bool direction = (u >= 0x200e && u <= 0x200f) || (u >= 0x202a && u <= 0x202e)
                               || (u >= 0x2066 && u <= 0x2069);
        if (!control && !direction)
            clean += c;
    }
    clean = clean.trimmed();
    if (clean.isEmpty() || clean == QLatin1String(".") || clean == QLatin1String(".."))
        clean = QStringLiteral("file");
    return clean.left(MaxNameLength);
}

bool isValidId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(id).hasMatch();
}

bool isValidType(const QString &type)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z0-9][a-z0-9!#$&^_.+-]*/[a-z0-9][a-z0-9!#$&^_.+-]*$"));
    return type.size() <= MaxTypeLength && pattern.match(type).hasMatch();
}

QJsonObject toJson(const Pointer &pointer)
{
    QJsonObject json{{QStringLiteral("id"), pointer.id},
                     {QStringLiteral("key"), Protocol::Json::base64(pointer.key)},
                     {QStringLiteral("digest"), Protocol::Json::base64(pointer.digest)},
                     {QStringLiteral("size"), double(pointer.size)},
                     {QStringLiteral("name"), pointer.name},
                     {QStringLiteral("type"), pointer.type}};
    if (pointer.voice)
        json.insert(QStringLiteral("voice"), true); // older versions ignore it: a .wav file to them
    return json;
}

std::optional<Pointer> fromJson(const QJsonValue &json)
{
    if (!json.isObject())
        return std::nullopt;
    const QJsonObject object = json.toObject();
    Pointer pointer;
    pointer.id = object.value(QStringLiteral("id")).toString();
    const auto key = Protocol::Json::bytes(object.value(QStringLiteral("key")), KeySize);
    const auto digest = Protocol::Json::bytes(object.value(QStringLiteral("digest")), DigestSize);
    const auto size = Protocol::Json::integer(object.value(QStringLiteral("size")), 0, MaxFileBytes);
    const QJsonValue name = object.value(QStringLiteral("name"));
    const QJsonValue type = object.value(QStringLiteral("type"));
    if (!isValidId(pointer.id) || !key || !digest || !size || !name.isString() || !type.isString()
        || name.toString().size() > MaxNameLength || !isValidType(type.toString()))
        return std::nullopt;
    pointer.key = *key;
    pointer.digest = *digest;
    pointer.size = *size;
    pointer.name = safeName(name.toString());
    pointer.type = type.toString();
    // A voice message: only audio/wav (the rest of what "voice" claims is checked when played).
    const QJsonValue voice = object.value(QStringLiteral("voice"));
    if (!voice.isUndefined() && (!voice.isBool() || (voice.toBool() && pointer.type != QLatin1String("audio/wav"))))
        return std::nullopt;
    pointer.voice = voice.toBool();
    return pointer;
}

} // namespace Attachment
