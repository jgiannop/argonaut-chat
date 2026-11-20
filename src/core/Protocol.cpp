#include "Protocol.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <QtEndian>

#include <limits>

namespace Protocol {

namespace {

constexpr char kMessage = 1;
constexpr char kPreKeyMessage = 2;
constexpr int kPreKeyHeaderSize = 1 + 32 + 32 + 4 + 1 + 4;
// Envelope times and sent times: up to the year 9999.
constexpr qint64 kMaxTime = 253402300799999;

void appendUint32(QByteArray &out, quint32 value)
{
    char bytes[4];
    qToBigEndian(value, bytes);
    out.append(bytes, 4);
}

QString receiptName(Content::Receipt kind)
{
    return kind == Content::Receipt::Read ? QStringLiteral("read") : QStringLiteral("delivered");
}

} // namespace

namespace Json {

QString base64(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64());
}

std::optional<QByteArray> bytes(const QJsonValue &value, int size, int maxSize)
{
    if (!value.isString())
        return std::nullopt;
    const QString text = value.toString();
    if (text.size() > 4 * ((size >= 0 ? size : maxSize) / 3 + 1))
        return std::nullopt;
    auto decoded = QByteArray::fromBase64Encoding(text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return std::nullopt;
    if (size >= 0 ? decoded.decoded.size() != size : decoded.decoded.isEmpty())
        return std::nullopt;
    return decoded.decoded;
}

std::optional<qint64> integer(const QJsonValue &value, qint64 min, qint64 max)
{
    if (!value.isDouble())
        return std::nullopt;
    const double d = value.toDouble();
    if (d < double(min) || d > double(max))
        return std::nullopt;
    const qint64 i = qint64(d);
    if (double(i) != d)
        return std::nullopt;
    return i;
}

std::optional<quint32> uint32(const QJsonValue &value)
{
    const auto i = integer(value, 0, std::numeric_limits<quint32>::max());
    if (!i)
        return std::nullopt;
    return quint32(*i);
}

} // namespace Json

using Json::base64;

PublicSignedPreKey publicPart(const X3dh::SignedPreKey &key)
{
    return {key.id, key.pair.publicKey, key.signature};
}

QList<PublicOneTimePreKey> publicParts(const QList<X3dh::OneTimePreKey> &keys)
{
    QList<PublicOneTimePreKey> out;
    for (const X3dh::OneTimePreKey &key : keys)
        out.append({key.id, key.pair.publicKey});
    return out;
}

QJsonObject signedPreKeyToJson(const PublicSignedPreKey &key)
{
    return {{QStringLiteral("id"), double(key.id)},
            {QStringLiteral("key"), base64(key.key)},
            {QStringLiteral("signature"), base64(key.signature)}};
}

std::optional<PublicSignedPreKey> signedPreKeyFromJson(const QJsonValue &json)
{
    if (!json.isObject())
        return std::nullopt;
    const QJsonObject object = json.toObject();
    const auto id = Json::uint32(object.value(QStringLiteral("id")));
    const auto key = Json::bytes(object.value(QStringLiteral("key")), Crypto::KeySize);
    const auto signature = Json::bytes(object.value(QStringLiteral("signature")), Crypto::SignatureSize);
    if (!id || !key || !signature)
        return std::nullopt;
    return PublicSignedPreKey{*id, *key, *signature};
}

QJsonArray oneTimePreKeysToJson(const QList<PublicOneTimePreKey> &keys)
{
    QJsonArray out;
    for (const PublicOneTimePreKey &key : keys)
        out.append(QJsonObject{{QStringLiteral("id"), double(key.id)}, {QStringLiteral("key"), base64(key.key)}});
    return out;
}

std::optional<QList<PublicOneTimePreKey>> oneTimePreKeysFromJson(const QJsonValue &json)
{
    if (!json.isArray() || json.toArray().size() > MaxOneTimePreKeysPerUpload)
        return std::nullopt;
    QList<PublicOneTimePreKey> keys;
    for (const QJsonValue &item : json.toArray()) {
        const auto id = Json::uint32(item.toObject().value(QStringLiteral("id")));
        const auto key = Json::bytes(item.toObject().value(QStringLiteral("key")), Crypto::KeySize);
        if (!item.isObject() || !id || !key)
            return std::nullopt;
        for (const PublicOneTimePreKey &other : keys) {
            if (other.id == *id)
                return std::nullopt;
        }
        keys.append({*id, *key});
    }
    return keys;
}

QByteArray authMessage(const QByteArray &nonce, const QString &username, quint32 device)
{
    QByteArray out = QByteArrayLiteral("ArgonautChat sign-in v1");
    out += '\0';
    out += nonce;
    char number[4];
    qToBigEndian(device, number);
    out.append(number, 4);
    out += username.toUtf8();
    return out;
}

QString normalizeUsername(const QString &username)
{
    const QString name = username.trimmed().toLower();
    return isValidUsername(name) ? name : QString();
}

bool isValidUsername(const QString &name)
{
    if (name.size() < 3 || name.size() > 32)
        return false;
    if (name.front() < u'a' || name.front() > u'z' || name.back() == u'.' || name.contains(QLatin1String("..")))
        return false;
    for (const QChar c : name) {
        const bool ok = (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9') || c == u'_' || c == u'.';
        if (!ok)
            return false;
    }
    return true;
}

QString newMessageId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool isValidMessageId(const QString &id)
{
    // The form newMessageId() makes: 36 characters, lower case, no braces.
    return id.size() == 36 && id == id.toLower() && !QUuid::fromString(id).isNull();
}

Content Content::text(const QString &body)
{
    Content content;
    content.type = Type::Text;
    content.id = newMessageId();
    content.sentAt = QDateTime::currentMSecsSinceEpoch();
    content.body = body;
    return content;
}

Content Content::receiptOf(Receipt kind, const QStringList &ids)
{
    Content content;
    content.type = Type::Receipt;
    content.id = newMessageId();
    content.sentAt = QDateTime::currentMSecsSinceEpoch();
    content.receipt = kind;
    content.receiptFor = ids;
    return content;
}

QByteArray pad(const QByteArray &data)
{
    QByteArray out = data;
    out += char(0x80);
    const int rest = out.size() % PaddingBlock;
    if (rest != 0)
        out += QByteArray(PaddingBlock - rest, '\0');
    return out;
}

std::optional<QByteArray> unpad(const QByteArray &padded)
{
    if (padded.isEmpty() || padded.size() % PaddingBlock != 0)
        return std::nullopt;
    int end = padded.size() - 1;
    while (end >= 0 && padded.at(end) == '\0')
        --end;
    if (end < 0 || padded.at(end) != char(0x80))
        return std::nullopt;
    return padded.left(end);
}

QByteArray encodeContent(const Content &content)
{
    if (!isValidMessageId(content.id) || content.sentAt < 0 || content.sentAt > kMaxTime)
        return {};
    QJsonObject json{{QStringLiteral("v"), Version},
                     {QStringLiteral("id"), content.id},
                     {QStringLiteral("sent"), double(content.sentAt)}};
    if (content.type == Content::Type::Text) {
        if (content.body.toUtf8().size() > MaxTextBytes)
            return {};
        json.insert(QStringLiteral("t"), QStringLiteral("text"));
        json.insert(QStringLiteral("body"), content.body);
    } else {
        if (content.receiptFor.isEmpty() || content.receiptFor.size() > MaxReceiptIds)
            return {};
        for (const QString &id : content.receiptFor) {
            if (!isValidMessageId(id))
                return {};
        }
        json.insert(QStringLiteral("t"), QStringLiteral("receipt"));
        json.insert(QStringLiteral("kind"), receiptName(content.receipt));
        json.insert(QStringLiteral("for"), QJsonArray::fromStringList(content.receiptFor));
    }
    return pad(QJsonDocument(json).toJson(QJsonDocument::Compact));
}

std::optional<Content> decodeContent(const QByteArray &padded)
{
    if (padded.size() > MaxContentBytes)
        return std::nullopt;
    const auto data = unpad(padded);
    if (!data)
        return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(*data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    const QJsonObject json = document.object();
    if (Json::integer(json.value(QStringLiteral("v")), 1, 1000) != Version)
        return std::nullopt;

    Content content;
    content.id = json.value(QStringLiteral("id")).toString();
    const auto sent = Json::integer(json.value(QStringLiteral("sent")), 0, kMaxTime);
    if (!isValidMessageId(content.id) || !sent)
        return std::nullopt;
    content.sentAt = *sent;

    const QString type = json.value(QStringLiteral("t")).toString();
    if (type == QLatin1String("text")) {
        const QJsonValue body = json.value(QStringLiteral("body"));
        if (!body.isString() || body.toString().toUtf8().size() > MaxTextBytes)
            return std::nullopt;
        content.type = Content::Type::Text;
        content.body = body.toString();
        return content;
    }
    if (type == QLatin1String("receipt")) {
        content.type = Content::Type::Receipt;
        const QString kind = json.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("delivered"))
            content.receipt = Content::Receipt::Delivered;
        else if (kind == QLatin1String("read"))
            content.receipt = Content::Receipt::Read;
        else
            return std::nullopt;
        const QJsonValue ids = json.value(QStringLiteral("for"));
        if (!ids.isArray() || ids.toArray().isEmpty() || ids.toArray().size() > MaxReceiptIds)
            return std::nullopt;
        for (const QJsonValue &id : ids.toArray()) {
            if (!id.isString() || !isValidMessageId(id.toString()))
                return std::nullopt;
            content.receiptFor << id.toString();
        }
        return content;
    }
    return std::nullopt;
}

QByteArray encodeCipherMessage(const CipherMessage &message)
{
    if (message.ratchetMessage.isEmpty())
        return {};
    QByteArray out;
    if (!message.preKey) {
        out += kMessage;
    } else {
        if (message.identityKey.size() != Crypto::KeySize || message.baseKey.size() != Crypto::KeySize)
            return {};
        out += kPreKeyMessage;
        out += message.identityKey;
        out += message.baseKey;
        appendUint32(out, message.signedPreKeyId);
        out += char(message.oneTimePreKeyId ? 1 : 0);
        appendUint32(out, message.oneTimePreKeyId.value_or(0));
    }
    out += message.ratchetMessage;
    if (out.size() > MaxCipherMessageBytes)
        return {};
    return out;
}

std::optional<CipherMessage> decodeCipherMessage(const QByteArray &data)
{
    if (data.size() < 2 || data.size() > MaxCipherMessageBytes)
        return std::nullopt;
    CipherMessage message;
    if (data.at(0) == kMessage) {
        message.ratchetMessage = data.mid(1);
        return message;
    }
    if (data.at(0) != kPreKeyMessage || data.size() <= kPreKeyHeaderSize)
        return std::nullopt;
    const char *p = data.constData() + 1;
    message.preKey = true;
    message.identityKey = data.mid(1, 32);
    message.baseKey = data.mid(33, 32);
    message.signedPreKeyId = qFromBigEndian<quint32>(p + 64);
    const char hasOneTime = p[68];
    const quint32 oneTimeId = qFromBigEndian<quint32>(p + 69);
    if (hasOneTime == 1)
        message.oneTimePreKeyId = oneTimeId;
    else if (hasOneTime != 0 || oneTimeId != 0)
        return std::nullopt;
    message.ratchetMessage = data.mid(kPreKeyHeaderSize);
    return message;
}

QJsonObject bundleToJson(const X3dh::PreKeyBundle &bundle)
{
    QJsonObject json{
        {QStringLiteral("identityKey"), base64(bundle.identityKey)},
        {QStringLiteral("signedPreKey"),
         QJsonObject{{QStringLiteral("id"), double(bundle.signedPreKeyId)},
                     {QStringLiteral("key"), base64(bundle.signedPreKey)},
                     {QStringLiteral("signature"), base64(bundle.signature)}}},
    };
    if (bundle.oneTimePreKeyId) {
        json.insert(QStringLiteral("oneTimePreKey"),
                    QJsonObject{{QStringLiteral("id"), double(*bundle.oneTimePreKeyId)},
                                {QStringLiteral("key"), base64(bundle.oneTimePreKey)}});
    }
    return json;
}

std::optional<X3dh::PreKeyBundle> bundleFromJson(const QJsonObject &json)
{
    X3dh::PreKeyBundle bundle;
    const auto identity = Json::bytes(json.value(QStringLiteral("identityKey")), Crypto::KeySize);
    const QJsonObject signedKey = json.value(QStringLiteral("signedPreKey")).toObject();
    const auto signedId = Json::uint32(signedKey.value(QStringLiteral("id")));
    const auto signedPub = Json::bytes(signedKey.value(QStringLiteral("key")), Crypto::KeySize);
    const auto signature = Json::bytes(signedKey.value(QStringLiteral("signature")), Crypto::SignatureSize);
    if (!identity || !signedId || !signedPub || !signature)
        return std::nullopt;
    bundle.identityKey = *identity;
    bundle.signedPreKeyId = *signedId;
    bundle.signedPreKey = *signedPub;
    bundle.signature = *signature;

    const QJsonValue oneTime = json.value(QStringLiteral("oneTimePreKey"));
    if (!oneTime.isUndefined() && !oneTime.isNull()) {
        if (!oneTime.isObject())
            return std::nullopt;
        const auto id = Json::uint32(oneTime.toObject().value(QStringLiteral("id")));
        const auto key = Json::bytes(oneTime.toObject().value(QStringLiteral("key")), Crypto::KeySize);
        if (!id || !key)
            return std::nullopt;
        bundle.oneTimePreKeyId = *id;
        bundle.oneTimePreKey = *key;
    }
    return bundle;
}

QJsonObject envelopeToJson(const Envelope &envelope)
{
    return {{QStringLiteral("id"), envelope.id},
            {QStringLiteral("from"), envelope.from},
            {QStringLiteral("device"), double(envelope.fromDevice)},
            {QStringLiteral("time"), double(envelope.serverTime)},
            {QStringLiteral("message"), base64(envelope.message)}};
}

std::optional<Envelope> envelopeFromJson(const QJsonObject &json)
{
    Envelope envelope;
    envelope.id = json.value(QStringLiteral("id")).toString();
    if (envelope.id.isEmpty() || envelope.id.size() > 64)
        return std::nullopt;
    for (const QChar c : envelope.id) {
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return std::nullopt;
    }
    const QString from = json.value(QStringLiteral("from")).toString();
    if (!isValidUsername(from))
        return std::nullopt;
    envelope.from = from;
    const auto device = Json::integer(json.value(QStringLiteral("device")), 1, std::numeric_limits<quint32>::max());
    const auto time = Json::integer(json.value(QStringLiteral("time")), 0, kMaxTime);
    const auto message = Json::bytes(json.value(QStringLiteral("message")), -1, MaxCipherMessageBytes);
    if (!device || !time || !message)
        return std::nullopt;
    envelope.fromDevice = quint32(*device);
    envelope.serverTime = *time;
    envelope.message = *message;
    return envelope;
}

} // namespace Protocol
