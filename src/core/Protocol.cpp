#include "Protocol.h"

#include "Crypto.h"

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

Content Content::groupUpdate(const QString &group, const QString &name, const QString &admin,
                             const QStringList &members, int revision)
{
    Content content;
    content.type = Type::Group;
    content.id = newMessageId();
    content.sentAt = QDateTime::currentMSecsSinceEpoch();
    content.group = group;
    content.groupName = cleanDisplayName(name);
    content.groupAdmin = admin;
    content.groupMembers = members;
    content.groupRevision = revision;
    return content;
}

Content Content::timer(qint64 seconds, const QString &group)
{
    Content content;
    content.type = Type::Timer;
    content.id = newMessageId();
    content.sentAt = QDateTime::currentMSecsSinceEpoch();
    content.expireSeconds = seconds;
    content.group = group;
    return content;
}

Content Content::reaction(const QString &targetId, const QString &targetAuthor, const QString &emoji, bool remove,
                          const QString &group)
{
    Content content;
    content.type = Type::Reaction;
    content.id = newMessageId();
    content.sentAt = QDateTime::currentMSecsSinceEpoch();
    content.quote = Quote{targetId, targetAuthor, QString()};
    content.emoji = remove ? QString() : emoji;
    content.remove = remove;
    content.group = group;
    return content;
}

Content Content::typingNow(bool started, const QString &group)
{
    Content content;
    content.type = Type::Typing;
    content.id = newMessageId();
    content.sentAt = QDateTime::currentMSecsSinceEpoch();
    content.typing = started;
    content.group = group;
    return content;
}

Content Content::edit(const QString &targetId, const QString &me, const QString &body, const QString &group)
{
    Content content;
    content.type = Type::Edit;
    content.id = newMessageId();
    content.sentAt = QDateTime::currentMSecsSinceEpoch();
    content.quote = Quote{targetId, me, QString()};
    content.body = body;
    content.group = group;
    return content;
}

Content Content::deletion(const QString &targetId, const QString &me, const QString &group)
{
    Content content;
    content.type = Type::Delete;
    content.id = newMessageId();
    content.sentAt = QDateTime::currentMSecsSinceEpoch();
    content.quote = Quote{targetId, me, QString()};
    content.group = group;
    return content;
}

bool isValidReaction(const QString &emoji)
{
    const QByteArray utf8 = emoji.toUtf8();
    if (utf8.isEmpty() || utf8.size() > MaxReactionBytes)
        return false;
    bool symbol = false;
    for (const char32_t c : emoji.toUcs4()) {
        const QChar::Category category = QChar::category(c);
        if (QChar::isLetterOrNumber(c) || QChar::isSpace(c) || category == QChar::Other_Control
            || category == QChar::Other_NotAssigned || category == QChar::Other_PrivateUse
            || (c >= 0x200E && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069))
            return false;
        if (category == QChar::Symbol_Other || category == QChar::Symbol_Math || category == QChar::Symbol_Modifier
            || category == QChar::Punctuation_Other)
            symbol = true;
    }
    return symbol;
}

Content Content::leave(const QString &group)
{
    Content content;
    content.type = Type::Leave;
    content.id = newMessageId();
    content.sentAt = QDateTime::currentMSecsSinceEpoch();
    content.group = group;
    return content;
}

bool isValidGroupId(const QString &id)
{
    return Attachment::isValidId(id); // the same form: 32 lower-case hex digits
}

QString newGroupId()
{
    return QString::fromLatin1(Crypto::randomBytes(16).toHex());
}

Content Content::profile(const QString &name, const std::optional<Attachment::Pointer> &avatar)
{
    Content content;
    content.type = Type::Profile;
    content.id = newMessageId();
    content.sentAt = QDateTime::currentMSecsSinceEpoch();
    content.name = cleanDisplayName(name);
    content.avatar = avatar;
    return content;
}

QString cleanDisplayName(const QString &name)
{
    QString clean;
    for (const QChar c : name) {
        const char16_t u = c.unicode();
        const bool control = u < 0x20 || u == 0x7f || (u >= 0x80 && u < 0xa0);
        const bool direction = (u >= 0x200e && u <= 0x200f) || (u >= 0x202a && u <= 0x202e)
                               || (u >= 0x2066 && u <= 0x2069);
        clean += control ? QChar(u' ') : direction ? QChar() : c;
    }
    clean.remove(QChar());
    return clean.simplified().left(MaxDisplayName).trimmed();
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
        if (!content.group.isEmpty()) {
            if (!isValidGroupId(content.group))
                return {};
            json.insert(QStringLiteral("group"), content.group);
        }
        if (!content.sentTo.isEmpty()) {
            if (!isValidUsername(content.sentTo))
                return {};
            json.insert(QStringLiteral("sentTo"), content.sentTo);
        }
        if (content.expireSeconds < 0 || content.expireSeconds > MaxExpireSeconds)
            return {};
        if (content.expireSeconds > 0)
            json.insert(QStringLiteral("expire"), double(content.expireSeconds));
        json.insert(QStringLiteral("body"), content.body);
        if (content.quote) {
            if (!isValidMessageId(content.quote->id) || !isValidUsername(content.quote->author)
                || content.quote->text.size() > MaxQuoteChars)
                return {};
            json.insert(QStringLiteral("quote"), QJsonObject{{QStringLiteral("id"), content.quote->id},
                                                             {QStringLiteral("author"), content.quote->author},
                                                             {QStringLiteral("text"), content.quote->text}});
        }
        if (content.attachments.size() > MaxAttachments)
            return {};
        if (!content.attachments.isEmpty()) {
            QJsonArray list;
            for (const Attachment::Pointer &pointer : content.attachments) {
                if (!Attachment::fromJson(Attachment::toJson(pointer))) // the same checks as on arrival
                    return {};
                list.append(Attachment::toJson(pointer));
            }
            json.insert(QStringLiteral("attachments"), list);
        }
    } else if (content.type == Content::Type::Group) {
        if (!isValidGroupId(content.group) || content.groupName != cleanDisplayName(content.groupName)
            || !isValidUsername(content.groupAdmin) || !content.groupMembers.contains(content.groupAdmin)
            || content.groupMembers.size() > MaxGroupMembers || content.groupRevision < 1)
            return {};
        for (const QString &member : content.groupMembers) {
            if (!isValidUsername(member) || content.groupMembers.count(member) > 1)
                return {};
        }
        json.insert(QStringLiteral("t"), QStringLiteral("group"));
        json.insert(QStringLiteral("group"), content.group);
        json.insert(QStringLiteral("name"), content.groupName);
        json.insert(QStringLiteral("admin"), content.groupAdmin);
        json.insert(QStringLiteral("members"), QJsonArray::fromStringList(content.groupMembers));
        json.insert(QStringLiteral("revision"), content.groupRevision);
    } else if (content.type == Content::Type::Timer) {
        if (content.expireSeconds < 0 || content.expireSeconds > MaxExpireSeconds
            || (!content.group.isEmpty() && !isValidGroupId(content.group))
            || (!content.sentTo.isEmpty() && !isValidUsername(content.sentTo)))
            return {};
        json.insert(QStringLiteral("t"), QStringLiteral("timer"));
        json.insert(QStringLiteral("expire"), double(content.expireSeconds));
        if (!content.group.isEmpty())
            json.insert(QStringLiteral("group"), content.group);
        if (!content.sentTo.isEmpty())
            json.insert(QStringLiteral("sentTo"), content.sentTo);
    } else if (content.type == Content::Type::Typing) {
        if (!content.group.isEmpty() && !isValidGroupId(content.group))
            return {};
        json.insert(QStringLiteral("t"), QStringLiteral("typing"));
        json.insert(QStringLiteral("action"), content.typing ? QStringLiteral("started") : QStringLiteral("stopped"));
        if (!content.group.isEmpty())
            json.insert(QStringLiteral("group"), content.group);
    } else if (content.type == Content::Type::Edit) {
        if (!content.quote || !isValidMessageId(content.quote->id) || !isValidUsername(content.quote->author)
            || content.body.toUtf8().size() > MaxTextBytes
            || (!content.group.isEmpty() && !isValidGroupId(content.group))
            || (!content.sentTo.isEmpty() && !isValidUsername(content.sentTo)))
            return {};
        json.insert(QStringLiteral("t"), QStringLiteral("edit"));
        json.insert(QStringLiteral("target"), QJsonObject{{QStringLiteral("id"), content.quote->id},
                                                          {QStringLiteral("author"), content.quote->author}});
        json.insert(QStringLiteral("body"), content.body);
        if (!content.group.isEmpty())
            json.insert(QStringLiteral("group"), content.group);
        if (!content.sentTo.isEmpty())
            json.insert(QStringLiteral("sentTo"), content.sentTo);
    } else if (content.type == Content::Type::Delete) {
        if (!content.quote || !isValidMessageId(content.quote->id) || !isValidUsername(content.quote->author)
            || (!content.group.isEmpty() && !isValidGroupId(content.group))
            || (!content.sentTo.isEmpty() && !isValidUsername(content.sentTo)))
            return {};
        json.insert(QStringLiteral("t"), QStringLiteral("delete"));
        json.insert(QStringLiteral("target"), QJsonObject{{QStringLiteral("id"), content.quote->id},
                                                          {QStringLiteral("author"), content.quote->author}});
        if (!content.group.isEmpty())
            json.insert(QStringLiteral("group"), content.group);
        if (!content.sentTo.isEmpty())
            json.insert(QStringLiteral("sentTo"), content.sentTo);
    } else if (content.type == Content::Type::Reaction) {
        if (!content.quote || !isValidMessageId(content.quote->id) || !isValidUsername(content.quote->author)
            || (content.remove ? !content.emoji.isEmpty() : !isValidReaction(content.emoji))
            || (!content.group.isEmpty() && !isValidGroupId(content.group))
            || (!content.sentTo.isEmpty() && !isValidUsername(content.sentTo)))
            return {};
        json.insert(QStringLiteral("t"), QStringLiteral("reaction"));
        json.insert(QStringLiteral("target"), QJsonObject{{QStringLiteral("id"), content.quote->id},
                                                          {QStringLiteral("author"), content.quote->author}});
        if (content.remove)
            json.insert(QStringLiteral("remove"), true);
        else
            json.insert(QStringLiteral("emoji"), content.emoji);
        if (!content.group.isEmpty())
            json.insert(QStringLiteral("group"), content.group);
        if (!content.sentTo.isEmpty())
            json.insert(QStringLiteral("sentTo"), content.sentTo);
    } else if (content.type == Content::Type::Leave) {
        if (!isValidGroupId(content.group))
            return {};
        json.insert(QStringLiteral("t"), QStringLiteral("leave"));
        json.insert(QStringLiteral("group"), content.group);
    } else if (content.type == Content::Type::Profile) {
        if (content.name != cleanDisplayName(content.name))
            return {};
        json.insert(QStringLiteral("t"), QStringLiteral("profile"));
        json.insert(QStringLiteral("name"), content.name);
        if (content.avatar) {
            if (!Attachment::fromJson(Attachment::toJson(*content.avatar)))
                return {};
            json.insert(QStringLiteral("avatar"), Attachment::toJson(*content.avatar));
        }
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
        const QJsonValue group = json.value(QStringLiteral("group"));
        if (!group.isUndefined()) {
            if (!group.isString() || !isValidGroupId(group.toString()))
                return std::nullopt;
            content.group = group.toString();
        }
        const QJsonValue sentTo = json.value(QStringLiteral("sentTo"));
        if (!sentTo.isUndefined()) {
            if (!sentTo.isString() || !isValidUsername(sentTo.toString()))
                return std::nullopt;
            content.sentTo = sentTo.toString();
        }
        const QJsonValue expire = json.value(QStringLiteral("expire"));
        if (!expire.isUndefined()) {
            const auto seconds = Json::integer(expire, 0, MaxExpireSeconds);
            if (!seconds)
                return std::nullopt;
            content.expireSeconds = *seconds;
        }
        const QJsonValue quote = json.value(QStringLiteral("quote"));
        if (!quote.isUndefined()) {
            const QJsonObject q = quote.toObject();
            const QJsonValue id = q.value(QStringLiteral("id"));
            const QJsonValue author = q.value(QStringLiteral("author"));
            const QJsonValue text = q.value(QStringLiteral("text"));
            if (!quote.isObject() || !id.isString() || !isValidMessageId(id.toString()) || !author.isString()
                || !isValidUsername(author.toString()) || !text.isString() || text.toString().size() > MaxQuoteChars)
                return std::nullopt;
            content.quote = Quote{id.toString(), author.toString(), text.toString()};
        }
        const QJsonValue attachments = json.value(QStringLiteral("attachments"));
        if (!attachments.isUndefined()) {
            if (!attachments.isArray() || attachments.toArray().isEmpty()
                || attachments.toArray().size() > MaxAttachments)
                return std::nullopt;
            for (const QJsonValue &item : attachments.toArray()) {
                const auto pointer = Attachment::fromJson(item);
                if (!pointer)
                    return std::nullopt;
                content.attachments.append(*pointer);
            }
        }
        return content;
    }
    if (type == QLatin1String("timer")) {
        content.type = Content::Type::Timer;
        const auto seconds = Json::integer(json.value(QStringLiteral("expire")), 0, MaxExpireSeconds);
        const QJsonValue group = json.value(QStringLiteral("group"));
        const QJsonValue sentTo = json.value(QStringLiteral("sentTo"));
        if (!seconds || (!group.isUndefined() && (!group.isString() || !isValidGroupId(group.toString())))
            || (!sentTo.isUndefined() && (!sentTo.isString() || !isValidUsername(sentTo.toString()))))
            return std::nullopt;
        content.expireSeconds = *seconds;
        content.group = group.toString();
        content.sentTo = sentTo.toString();
        return content;
    }
    if (type == QLatin1String("typing")) {
        content.type = Content::Type::Typing;
        const QString action = json.value(QStringLiteral("action")).toString();
        const QJsonValue group = json.value(QStringLiteral("group"));
        if ((action != QLatin1String("started") && action != QLatin1String("stopped"))
            || (!group.isUndefined() && (!group.isString() || !isValidGroupId(group.toString()))))
            return std::nullopt;
        content.typing = action == QLatin1String("started");
        content.group = group.toString();
        return content;
    }
    if (type == QLatin1String("delete") || type == QLatin1String("edit")) {
        content.type = type == QLatin1String("edit") ? Content::Type::Edit : Content::Type::Delete;
        if (content.type == Content::Type::Edit) {
            const QJsonValue body = json.value(QStringLiteral("body"));
            if (!body.isString() || body.toString().toUtf8().size() > MaxTextBytes)
                return std::nullopt;
            content.body = body.toString();
        }
        const QJsonValue target = json.value(QStringLiteral("target"));
        const QJsonValue id = target.toObject().value(QStringLiteral("id"));
        const QJsonValue author = target.toObject().value(QStringLiteral("author"));
        const QJsonValue group = json.value(QStringLiteral("group"));
        const QJsonValue sentTo = json.value(QStringLiteral("sentTo"));
        if (!target.isObject() || !id.isString() || !isValidMessageId(id.toString()) || !author.isString()
            || !isValidUsername(author.toString())
            || (!group.isUndefined() && (!group.isString() || !isValidGroupId(group.toString())))
            || (!sentTo.isUndefined() && (!sentTo.isString() || !isValidUsername(sentTo.toString()))))
            return std::nullopt;
        content.quote = Quote{id.toString(), author.toString(), QString()};
        content.group = group.toString();
        content.sentTo = sentTo.toString();
        return content;
    }
    if (type == QLatin1String("reaction")) {
        content.type = Content::Type::Reaction;
        const QJsonValue target = json.value(QStringLiteral("target"));
        const QJsonValue id = target.toObject().value(QStringLiteral("id"));
        const QJsonValue author = target.toObject().value(QStringLiteral("author"));
        const QJsonValue emoji = json.value(QStringLiteral("emoji"));
        const QJsonValue remove = json.value(QStringLiteral("remove"));
        const QJsonValue group = json.value(QStringLiteral("group"));
        const QJsonValue sentTo = json.value(QStringLiteral("sentTo"));
        if (!target.isObject() || !id.isString() || !isValidMessageId(id.toString()) || !author.isString()
            || !isValidUsername(author.toString()))
            return std::nullopt;
        // Either an emoji, or "remove": true.
        if (remove.isUndefined()) {
            if (!emoji.isString() || !isValidReaction(emoji.toString()))
                return std::nullopt;
            content.emoji = emoji.toString();
        } else if (remove != QJsonValue(true) || !emoji.isUndefined()) {
            return std::nullopt;
        } else {
            content.remove = true;
        }
        if ((!group.isUndefined() && (!group.isString() || !isValidGroupId(group.toString())))
            || (!sentTo.isUndefined() && (!sentTo.isString() || !isValidUsername(sentTo.toString()))))
            return std::nullopt;
        content.quote = Quote{id.toString(), author.toString(), QString()};
        content.group = group.toString();
        content.sentTo = sentTo.toString();
        return content;
    }
    if (type == QLatin1String("group") || type == QLatin1String("leave")) {
        const QJsonValue group = json.value(QStringLiteral("group"));
        if (!group.isString() || !isValidGroupId(group.toString()))
            return std::nullopt;
        content.group = group.toString();
        if (type == QLatin1String("leave")) {
            content.type = Content::Type::Leave;
            return content;
        }
        content.type = Content::Type::Group;
        const QJsonValue name = json.value(QStringLiteral("name"));
        const QJsonValue admin = json.value(QStringLiteral("admin"));
        const QJsonValue members = json.value(QStringLiteral("members"));
        const auto revision = Json::integer(json.value(QStringLiteral("revision")), 1, std::numeric_limits<int>::max());
        if (!name.isString() || name.toString().size() > 4 * MaxDisplayName || !admin.isString()
            || !isValidUsername(admin.toString()) || !members.isArray() || members.toArray().isEmpty()
            || members.toArray().size() > MaxGroupMembers || !revision)
            return std::nullopt;
        for (const QJsonValue &member : members.toArray()) {
            if (!member.isString() || !isValidUsername(member.toString())
                || content.groupMembers.contains(member.toString()))
                return std::nullopt;
            content.groupMembers << member.toString();
        }
        if (!content.groupMembers.contains(admin.toString()))
            return std::nullopt;
        content.groupName = cleanDisplayName(name.toString());
        content.groupAdmin = admin.toString();
        content.groupRevision = int(*revision);
        return content;
    }
    if (type == QLatin1String("profile")) {
        content.type = Content::Type::Profile;
        const QJsonValue name = json.value(QStringLiteral("name"));
        if (!name.isString() || name.toString().size() > 4 * MaxDisplayName)
            return std::nullopt;
        content.name = cleanDisplayName(name.toString());
        const QJsonValue avatar = json.value(QStringLiteral("avatar"));
        if (!avatar.isUndefined()) {
            const auto pointer = Attachment::fromJson(avatar);
            if (!pointer || !pointer->type.startsWith(QLatin1String("image/")))
                return std::nullopt;
            content.avatar = pointer;
        }
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
