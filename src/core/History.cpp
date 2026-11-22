#include "History.h"

#include "Attachment.h"
#include "ChatStore.h"
#include "Protocol.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>
#include <QSqlDatabase>
#include <QTimeZone>
#include <QtEndian>

#include <algorithm>

static_assert(History::MaxPieces == Protocol::MaxHistoryPieces, "one limit");

namespace History {

namespace {

const QByteArray Header("ACHIST1\n");
constexpr int FrameHead = 5;                      // kind, length
constexpr qint64 MaxRecordBytes = 1024 * 1024;    // a 'C', 'M' or 'E' frame
constexpr qint64 MaxStreamBytes = PieceBytes * MaxPieces;
constexpr qint64 Earliest = 1'500'000'000'000LL;  // 2017: no message is older
constexpr qint64 Slack = 24LL * 3600 * 1000;      // clocks a day apart
constexpr int MaxReactions = 64;

QByteArray compact(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// A JSON object frame, or nothing (not JSON, or not an object).
std::optional<QJsonObject> objectOf(const QByteArray &payload)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    return document.object();
}

std::optional<QString> stringOf(const QJsonObject &o, const QString &key, bool optional = false)
{
    const QJsonValue v = o.value(key);
    if (v.isUndefined() && optional)
        return QString();
    if (!v.isString())
        return std::nullopt;
    return v.toString();
}

// Only the keys a frame may have.
bool onlyKeys(const QJsonObject &o, const QStringList &allowed)
{
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!allowed.contains(it.key()))
            return false;
    }
    return true;
}

} // namespace

Writer::Writer(ChatStore *store, qint64 nowMs)
    : m_store(store)
{
    m_buffer = Header;
    int n = 0;
    for (const ChatStore::Conversation &c : store->conversations()) {
        if (c.request || c.left)
            continue;
        QString peer;
        if (c.kind == QLatin1String("direct") || c.kind == QLatin1String("group"))
            peer = c.peer;
        else if (c.kind != QLatin1String("self"))
            continue;
        m_conversations.insert(c.id, n);
        writeFrame('C', compact({{QStringLiteral("n"), n}, {QStringLiteral("kind"), c.kind}, {QStringLiteral("peer"), peer}}));
        ++n;
    }
    m_ids = store->historyMessages(nowMs - MaxAgeMs);
}

Writer::~Writer() = default;

void Writer::writeFrame(char kind, const QByteArray &payload)
{
    char head[FrameHead];
    head[0] = kind;
    qToBigEndian(quint32(payload.size()), head + 1);
    m_buffer.append(head, FrameHead);
    m_buffer.append(payload);
}

void Writer::writeMessage(qint64 id)
{
    const ChatStore::Message m = m_store->message(id);
    const auto number = m_conversations.constFind(m.conversation);
    if (m.id == 0 || number == m_conversations.constEnd())
        return;
    // Its files that are here, within the budget (past it, the newest have them).
    QJsonArray files;
    QList<QByteArray> data;
    for (const ChatStore::StoredAttachment &a : m_store->attachments(id)) {
        if (a.file.isEmpty() || m_fileBytes + a.size > FileBudget)
            continue;
        const auto plain = Attachment::decrypt(m_store->readBlob(a.file), a.key, a.digest, a.size);
        if (!plain)
            continue;
        m_fileBytes += a.size;
        data << *plain;
        QJsonObject file{{QStringLiteral("name"), a.name},
                         {QStringLiteral("type"), a.type},
                         {QStringLiteral("size"), double(a.size)}};
        if (a.voice)
            file.insert(QStringLiteral("voice"), true);
        files.append(file);
    }
    if (m.body.isEmpty() && files.isEmpty())
        return; // a file that never came: nothing to show
    QJsonObject json{{QStringLiteral("c"), *number},
                     {QStringLiteral("uid"), m.uid},
                     {QStringLiteral("author"), m.author},
                     {QStringLiteral("body"), m.body},
                     {QStringLiteral("at"), double(m.sentAt.toMSecsSinceEpoch())},
                     {QStringLiteral("status"), m.status}};
    if (m.isReply())
        json.insert(QStringLiteral("quote"), QJsonObject{{QStringLiteral("uid"), m.quoteUid},
                                                         {QStringLiteral("author"), m.quoteAuthor},
                                                         {QStringLiteral("text"), m.quoteText}});
    if (!m.mentions.isEmpty()) {
        QJsonArray mentions;
        for (const Protocol::Mention &mention : m.mentions)
            mentions.append(QJsonArray{mention.start, mention.length, mention.username});
        json.insert(QStringLiteral("mentions"), mentions);
    }
    if (m.editedAt > 0)
        json.insert(QStringLiteral("edited"), double(m.editedAt));
    QJsonArray reactions;
    for (const ChatStore::Reaction &r : m_store->reactions(id)) {
        if (!r.emoji.isEmpty() && reactions.size() < MaxReactions)
            reactions.append(QJsonObject{{QStringLiteral("author"), r.author},
                                         {QStringLiteral("emoji"), r.emoji},
                                         {QStringLiteral("at"), double(r.at)}});
    }
    if (!reactions.isEmpty())
        json.insert(QStringLiteral("reactions"), reactions);
    if (!files.isEmpty())
        json.insert(QStringLiteral("files"), files);
    writeFrame('M', compact(json));
    ++m_messages;
    for (const QByteArray &bytes : std::as_const(data)) {
        writeFrame('F', bytes);
        ++m_files;
    }
}

QByteArray Writer::next()
{
    while (m_buffer.size() < PieceBytes && !m_ended) {
        // Room kept for the end, the largest file and a message; past that, it ends.
        if (m_next < m_ids.size() && m_streamed + m_buffer.size() < MaxStreamBytes - Attachment::MaxFileBytes - 2 * MaxRecordBytes) {
            writeMessage(m_ids.at(m_next++));
        } else {
            writeFrame('E', compact({{QStringLiteral("messages"), m_messages}, {QStringLiteral("files"), m_files}}));
            m_ended = true;
        }
    }
    const QByteArray piece = m_buffer.left(PieceBytes);
    m_buffer.remove(0, piece.size());
    m_streamed += piece.size();
    return piece;
}

double Writer::progress() const
{
    if (m_ended)
        return 1.0;
    return m_ids.isEmpty() ? 0.0 : double(m_next) / m_ids.size();
}

Reader::Reader(ChatStore *store, const QString &me, qint64 nowMs)
    : m_store(store)
    , m_me(me)
    , m_now(nowMs)
{
}

Reader::~Reader() = default;

bool Reader::feed(const QByteArray &piece)
{
    if (m_broken || piece.isEmpty() || m_ended) {
        m_broken = true; // nothing may follow the end
        return false;
    }
    m_received += piece.size();
    if (m_received > MaxStreamBytes) {
        m_broken = true;
        return false;
    }
    m_buffer.append(piece);
    if (!m_headerSeen) {
        if (m_buffer.size() < Header.size())
            return true;
        if (!m_buffer.startsWith(Header)) {
            m_broken = true;
            return false;
        }
        m_buffer.remove(0, Header.size());
        m_headerSeen = true;
    }
    // One transaction and one redraw per piece, not per message.
    QSqlDatabase db = m_store->database();
    const bool inTransaction = db.transaction();
    QStringList seen;
    {
        const QSignalBlocker quiet(m_store);
        while (!m_broken && m_buffer.size() >= FrameHead) {
            const char kind = m_buffer.at(0);
            const qint64 length = qFromBigEndian<quint32>(m_buffer.constData() + 1);
            const qint64 limit = kind == 'F' ? Attachment::MaxFileBytes : MaxRecordBytes;
            if (length > limit) {
                m_broken = true;
                break;
            }
            if (m_buffer.size() < FrameHead + length)
                break; // the rest comes with the next piece
            const QByteArray payload = m_buffer.mid(FrameHead, length);
            m_buffer.remove(0, FrameHead + length);
            if (!frame(kind, payload))
                m_broken = true;
        }
        seen.swap(m_seen);
        if (!seen.isEmpty())
            m_store->markSeenUids(seen);
    }
    if (inTransaction)
        db.commit();
    for (qint64 conversation : std::as_const(m_touched))
        emit m_store->messagesChanged(conversation);
    if (!m_touched.isEmpty())
        emit m_store->conversationsChanged();
    m_touched.clear();
    return !m_broken;
}

bool Reader::finish() const
{
    return !m_broken && m_ended && m_buffer.isEmpty() && m_pendingFiles.isEmpty();
}

bool Reader::frame(char kind, const QByteArray &payload)
{
    // A message's files come right after it, all of them.
    if (!m_pendingFiles.isEmpty() && kind != 'F')
        return false;
    if (m_ended)
        return false;
    switch (kind) {
    case 'C':
        return m_messagesRead == 0 && conversation(payload);
    case 'M':
        return message(payload);
    case 'F':
        return file(payload);
    case 'E':
        return end(payload);
    default:
        return false;
    }
}

bool Reader::conversation(const QByteArray &payload)
{
    const auto o = objectOf(payload);
    if (!o || !onlyKeys(*o, {QStringLiteral("n"), QStringLiteral("kind"), QStringLiteral("peer")}))
        return false;
    const auto n = Protocol::Json::integer(o->value(QStringLiteral("n")), 0, 100000);
    const auto kind = stringOf(*o, QStringLiteral("kind"));
    const auto peer = stringOf(*o, QStringLiteral("peer"));
    if (!n || *n != m_conversations.size() || !kind || !peer)
        return false;
    qint64 local = 0;
    if (*kind == QLatin1String("self")) {
        if (!peer->isEmpty())
            return false;
        local = m_store->noteToSelf();
    } else if (*kind == QLatin1String("direct")) {
        if (!Protocol::isValidUsername(*peer) || *peer == m_me)
            return false;
        if (!m_store->isBlocked(*peer)) {
            local = m_store->directConversation(*peer);
            if (local == 0)
                local = m_store->addConversation(QStringLiteral("direct"), *peer, *peer);
            m_store->setAccepted(local); // it was, there
        }
    } else if (*kind == QLatin1String("group")) {
        if (!Protocol::isValidGroupId(*peer))
            return false;
        // Only a group this computer is in (the link brought them).
        local = m_store->groupConversation(*peer);
        if (local != 0 && m_store->conversation(local).left)
            local = 0;
    } else {
        return false;
    }
    m_conversations << local;
    m_kinds << *kind;
    m_peers << *peer;
    return true;
}

bool Reader::message(const QByteArray &payload)
{
    const auto o = objectOf(payload);
    if (!o
        || !onlyKeys(*o, {QStringLiteral("c"), QStringLiteral("uid"), QStringLiteral("author"), QStringLiteral("body"),
                          QStringLiteral("at"), QStringLiteral("status"), QStringLiteral("quote"),
                          QStringLiteral("mentions"), QStringLiteral("edited"), QStringLiteral("reactions"),
                          QStringLiteral("files")}))
        return false;
    ++m_messagesRead;
    const auto c = Protocol::Json::integer(o->value(QStringLiteral("c")), 0, m_conversations.size() - 1);
    const auto uid = stringOf(*o, QStringLiteral("uid"));
    const auto author = stringOf(*o, QStringLiteral("author"));
    const auto body = stringOf(*o, QStringLiteral("body"));
    const auto at = Protocol::Json::integer(o->value(QStringLiteral("at")), Earliest, m_now + Slack);
    const auto status = stringOf(*o, QStringLiteral("status"));
    if (m_conversations.isEmpty() || !c || !uid || !author || !body || !at || !status
        || body->toUtf8().size() > Protocol::MaxTextBytes)
        return false;
    const QString kind = m_kinds.at(int(*c));
    const QString peer = m_peers.at(int(*c));
    // Whose: mine (empty), or the one person of a direct conversation, or
    // anyone in a group; a note is mine.
    if (!author->isEmpty()
        && (!Protocol::isValidUsername(*author) || *author == m_me || kind == QLatin1String("self")
            || (kind == QLatin1String("direct") && *author != peer)))
        return false;
    if (uid->isEmpty() ? kind != QLatin1String("self") : !Protocol::isValidMessageId(*uid))
        return false;
    static const QStringList mine{QStringLiteral("sent"), QStringLiteral("delivered"), QStringLiteral("read"),
                                  QStringLiteral("failed")};
    if (author->isEmpty() ? !mine.contains(*status) : *status != QLatin1String("received"))
        return false;

    std::optional<Protocol::Quote> quote;
    const QJsonValue q = o->value(QStringLiteral("quote"));
    if (!q.isUndefined()) {
        const QJsonObject qo = q.toObject();
        const auto quoteUid = stringOf(qo, QStringLiteral("uid"));
        const auto quoteAuthor = stringOf(qo, QStringLiteral("author"));
        const auto quoteText = stringOf(qo, QStringLiteral("text"));
        if (!q.isObject() || !onlyKeys(qo, {QStringLiteral("uid"), QStringLiteral("author"), QStringLiteral("text")})
            || !quoteUid || !Protocol::isValidMessageId(*quoteUid) || !quoteAuthor
            || !Protocol::isValidUsername(*quoteAuthor) || !quoteText || quoteText->size() > Protocol::MaxQuoteChars)
            return false;
        quote = Protocol::Quote{*quoteUid, *quoteAuthor, *quoteText};
    }

    QList<Protocol::Mention> mentions;
    const QJsonValue m = o->value(QStringLiteral("mentions"));
    if (!m.isUndefined()) {
        if (!m.isArray() || kind != QLatin1String("group"))
            return false;
        for (const QJsonValue &item : m.toArray()) {
            const QJsonArray a = item.toArray();
            const auto start = Protocol::Json::integer(a.at(0), 0, Protocol::MaxTextBytes);
            const auto length = Protocol::Json::integer(a.at(1), 1, Protocol::MaxTextBytes);
            if (!item.isArray() || a.size() != 3 || !start || !length || !a.at(2).isString()
                || !Protocol::isValidUsername(a.at(2).toString()))
                return false;
            mentions << Protocol::Mention{int(*start), int(*length), a.at(2).toString()};
        }
        if (!Protocol::isValidMentions(mentions, *body))
            return false;
    }

    qint64 edited = 0;
    const QJsonValue e = o->value(QStringLiteral("edited"));
    if (!e.isUndefined()) {
        const auto when = Protocol::Json::integer(e, Earliest, m_now + Slack);
        if (!when)
            return false;
        edited = *when;
    }

    QList<ChatStore::Reaction> reactions;
    const QJsonValue r = o->value(QStringLiteral("reactions"));
    if (!r.isUndefined()) {
        if (!r.isArray() || r.toArray().size() > MaxReactions)
            return false;
        for (const QJsonValue &item : r.toArray()) {
            const QJsonObject ro = item.toObject();
            const auto who = stringOf(ro, QStringLiteral("author"));
            const auto emoji = stringOf(ro, QStringLiteral("emoji"));
            const auto when = Protocol::Json::integer(ro.value(QStringLiteral("at")), 0, m_now + Slack);
            if (!item.isObject() || !onlyKeys(ro, {QStringLiteral("author"), QStringLiteral("emoji"), QStringLiteral("at")})
                || !who || (!who->isEmpty() && (!Protocol::isValidUsername(*who) || *who == m_me)) || !emoji
                || !Protocol::isValidReaction(*emoji) || !when)
                return false;
            reactions << ChatStore::Reaction{0, *who, *emoji, *when};
        }
    }

    QList<PendingFile> files;
    const QJsonValue f = o->value(QStringLiteral("files"));
    if (!f.isUndefined()) {
        if (!f.isArray() || f.toArray().isEmpty() || f.toArray().size() > Protocol::MaxAttachments)
            return false;
        for (const QJsonValue &item : f.toArray()) {
            const QJsonObject fo = item.toObject();
            const auto name = stringOf(fo, QStringLiteral("name"));
            const auto type = stringOf(fo, QStringLiteral("type"));
            const auto size = Protocol::Json::integer(fo.value(QStringLiteral("size")), 0, Attachment::MaxFileBytes);
            const QJsonValue voice = fo.value(QStringLiteral("voice"));
            if (!item.isObject()
                || !onlyKeys(fo, {QStringLiteral("name"), QStringLiteral("type"), QStringLiteral("size"),
                                  QStringLiteral("voice")})
                || !name || name->size() > Attachment::MaxNameLength || !type || !Attachment::isValidType(*type) || !size
                || (!voice.isUndefined() && !voice.isBool()))
                return false;
            const bool isVoice = voice.toBool();
            if (isVoice && *type != QLatin1String("audio/wav") && *type != QLatin1String("audio/ogg"))
                return false;
            files << PendingFile{Attachment::safeName(*name), *type, *size, isVoice};
        }
    }
    if (body->isEmpty() && files.isEmpty())
        return false;

    m_pendingFiles = files;
    m_fileMessage = 0;
    const qint64 local = m_conversations.at(int(*c));
    // Not a conversation here, or a message already here (it came since the link): left out.
    if (local == 0 || (!uid->isEmpty() && m_store->messageByUid(local, *uid) != 0))
        return true;
    const ChatStore::Message added =
        m_store->addMessage(local, *body, *author, QDateTime::fromMSecsSinceEpoch(*at, QTimeZone::utc()), *uid, *status);
    if (added.id == 0)
        return false;
    const bool mentionsMe = std::any_of(mentions.begin(), mentions.end(),
                                        [this](const Protocol::Mention &x) { return x.username == m_me; });
    if (quote)
        m_store->setQuote(added.id, quote->id, quote->author, quote->text);
    if (edited > 0)
        m_store->editMessage(added.id, *body, edited, false, mentions, !author->isEmpty() && mentionsMe);
    else if (!mentions.isEmpty())
        m_store->setMentions(added.id, mentions, !author->isEmpty() && mentionsMe);
    for (const ChatStore::Reaction &reaction : std::as_const(reactions))
        m_store->setReaction(added.id, reaction.author, reaction.emoji, reaction.at);
    if (!author->isEmpty())
        m_seen << *uid; // read there: history, not news
    m_fileMessage = added.id;
    m_touched.insert(local);
    ++m_messages;
    return true;
}

bool Reader::file(const QByteArray &payload)
{
    if (m_pendingFiles.isEmpty())
        return false;
    const PendingFile pending = m_pendingFiles.takeFirst();
    ++m_filesRead;
    if (payload.size() != pending.size)
        return false;
    if (m_fileMessage == 0)
        return true; // its message was left out
    // Kept as every file is: encrypted with a key of its own.
    const auto encrypted = Attachment::encrypt(payload);
    if (!encrypted)
        return false;
    const QString blobFile = m_store->writeBlob(encrypted->blob);
    if (blobFile.isEmpty())
        return false;
    ChatStore::StoredAttachment a;
    a.message = m_fileMessage;
    a.name = pending.name;
    a.type = pending.type;
    a.size = pending.size;
    a.key = encrypted->key;
    a.digest = encrypted->digest;
    a.file = blobFile;
    a.state = QStringLiteral("ready");
    a.voice = pending.voice;
    if (m_store->addAttachment(a) == 0)
        return false;
    ++m_files;
    return true;
}

bool Reader::end(const QByteArray &payload)
{
    const auto o = objectOf(payload);
    if (!o || !onlyKeys(*o, {QStringLiteral("messages"), QStringLiteral("files")}))
        return false;
    const auto messages = Protocol::Json::integer(o->value(QStringLiteral("messages")), 0, 100'000'000);
    const auto files = Protocol::Json::integer(o->value(QStringLiteral("files")), 0, 100'000'000);
    if (!messages || !files || *messages != m_messagesRead || *files != m_filesRead)
        return false;
    m_ended = true;
    return true;
}

} // namespace History
