#include "ChatStore.h"

#include "CipherSql.h"

#include <QTimeZone>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

namespace {

// An empty text, never NULL (a null QString is bound as NULL).
QString text(const QString &s)
{
    return s.isNull() ? QStringLiteral("") : s;
}

} // namespace

ChatStore::ChatStore(const QString &file, const QByteArray &key, QObject *parent)
    : QObject(parent)
    , m_connection(QStringLiteral("chat-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    const QString path = file.isEmpty() ? defaultFile() : file;
    const QString folder = QFileInfo(path).absolutePath();
    QDir().mkpath(folder);
    QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    m_attachments = folder + QStringLiteral("/attachments");
    QDir().mkpath(m_attachments);
    QFile::setPermissions(m_attachments, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    CipherSql::registerDriver();
    if (!key.isEmpty()) {
        if (CipherSql::password(key).isEmpty()) {
            m_error = tr("The database key is not a key.");
            return;
        }
        // From before the encryption: encrypted now, in place.
        if (QFileInfo::exists(path) && CipherSql::isPlainSqlite(path) && !CipherSql::encryptFile(path, key, &m_error)) {
            m_error = tr("The history could not be encrypted: %1").arg(m_error);
            return;
        }
    }
    m_db = QSqlDatabase::addDatabase(CipherSql::DriverName, m_connection);
    m_db.setDatabaseName(path);
    m_db.setPassword(CipherSql::password(key));
    if (!m_db.open()) {
        m_error = m_db.lastError().text();
        return;
    }
    m_encrypted = !key.isEmpty();
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner); // the history is private
    QSqlQuery(m_db).exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    QSqlQuery(m_db).exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    createTables();
}

ChatStore::~ChatStore()
{
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connection);
}

QString ChatStore::defaultFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/chat.sqlite");
}

void ChatStore::createTables()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS conversations ("
                          " id INTEGER PRIMARY KEY,"
                          " kind TEXT NOT NULL,"
                          " title TEXT NOT NULL DEFAULT '',"
                          " peer TEXT NOT NULL DEFAULT '',"
                          " created INTEGER NOT NULL,"
                          " last_activity INTEGER NOT NULL)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS messages ("
                          " id INTEGER PRIMARY KEY,"
                          " conversation INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,"
                          " author TEXT NOT NULL DEFAULT '',"
                          " body TEXT NOT NULL,"
                          " sent_at INTEGER NOT NULL,"
                          " status TEXT NOT NULL,"
                          " seen INTEGER NOT NULL DEFAULT 0)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS messages_by_conversation ON messages(conversation, sent_at)"));
    // Added with the network: the message's id on the wire.
    bool hasUid = false;
    q.exec(QStringLiteral("PRAGMA table_info(messages)"));
    while (q.next())
        hasUid = hasUid || q.value(1).toString() == QLatin1String("uid");
    if (!hasUid)
        q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN uid TEXT NOT NULL DEFAULT ''"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS messages_by_uid ON messages(conversation, uid)"));
    // Added with message requests.
    bool hasState = false;
    q.exec(QStringLiteral("PRAGMA table_info(conversations)"));
    while (q.next())
        hasState = hasState || q.value(1).toString() == QLatin1String("state");
    if (!hasState)
        q.exec(QStringLiteral("ALTER TABLE conversations ADD COLUMN state TEXT NOT NULL DEFAULT 'accepted'"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS blocked ("
                          " username TEXT PRIMARY KEY,"
                          " since INTEGER NOT NULL)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS attachments ("
                          " id INTEGER PRIMARY KEY,"
                          " message INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
                          " name TEXT NOT NULL,"
                          " type TEXT NOT NULL,"
                          " size INTEGER NOT NULL,"
                          " key BLOB NOT NULL,"
                          " digest BLOB NOT NULL,"
                          " remote_id TEXT NOT NULL DEFAULT '',"
                          " file TEXT NOT NULL DEFAULT '',"
                          " state TEXT NOT NULL)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS attachments_by_message ON attachments(message)"));
    // Added with disappearing messages.
    const auto addColumn = [this](const QString &table, const QString &column, const QString &definition) {
        QSqlQuery info(m_db);
        info.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table));
        while (info.next()) {
            if (info.value(1).toString() == column)
                return;
        }
        QSqlQuery(m_db).exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, column, definition));
    };
    addColumn(QStringLiteral("conversations"), QStringLiteral("expire_seconds"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    addColumn(QStringLiteral("messages"), QStringLiteral("expire_seconds"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    addColumn(QStringLiteral("messages"), QStringLiteral("expires_at"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    addColumn(QStringLiteral("messages"), QStringLiteral("system"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    // Added with voice messages.
    addColumn(QStringLiteral("attachments"), QStringLiteral("voice"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    // Added with editing.
    addColumn(QStringLiteral("messages"), QStringLiteral("edited_at"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    addColumn(QStringLiteral("messages"), QStringLiteral("edit_pending"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    // Added with deleting for everyone.
    addColumn(QStringLiteral("messages"), QStringLiteral("deleted"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    addColumn(QStringLiteral("messages"), QStringLiteral("delete_pending"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    // Added with replies.
    addColumn(QStringLiteral("messages"), QStringLiteral("quote_uid"), QStringLiteral("TEXT NOT NULL DEFAULT ''"));
    addColumn(QStringLiteral("messages"), QStringLiteral("quote_author"), QStringLiteral("TEXT NOT NULL DEFAULT ''"));
    addColumn(QStringLiteral("messages"), QStringLiteral("quote_text"), QStringLiteral("TEXT NOT NULL DEFAULT ''"));
    // Added with mute, archive and pin.
    addColumn(QStringLiteral("conversations"), QStringLiteral("muted_until"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    addColumn(QStringLiteral("conversations"), QStringLiteral("archived"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    addColumn(QStringLiteral("conversations"), QStringLiteral("pinned"), QStringLiteral("INTEGER NOT NULL DEFAULT 0"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS messages_by_expiry ON messages(expires_at)"));
    // Reactions: one per person per message; mine with author ''. A removal
    // of mine stays (emoji '') until it has been sent.
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS reactions ("
                          " message INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
                          " author TEXT NOT NULL,"
                          " emoji TEXT NOT NULL,"
                          " at INTEGER NOT NULL,"
                          " pending INTEGER NOT NULL DEFAULT 0,"
                          " PRIMARY KEY (message, author))"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS group_info ("
                          " conversation INTEGER PRIMARY KEY REFERENCES conversations(id) ON DELETE CASCADE,"
                          " admin TEXT NOT NULL,"
                          " revision INTEGER NOT NULL)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS group_members ("
                          " conversation INTEGER NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,"
                          " username TEXT NOT NULL,"
                          " PRIMARY KEY (conversation, username))"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS my_profile ("
                          " id INTEGER PRIMARY KEY CHECK (id = 1),"
                          " name TEXT NOT NULL DEFAULT '',"
                          " avatar BLOB,"
                          " version INTEGER NOT NULL DEFAULT 0,"
                          " avatar_pointer TEXT NOT NULL DEFAULT '',"
                          " avatar_uploaded INTEGER NOT NULL DEFAULT 0)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS profiles ("
                          " username TEXT PRIMARY KEY,"
                          " name TEXT NOT NULL DEFAULT '',"
                          " avatar BLOB,"
                          " pending_avatar TEXT NOT NULL DEFAULT '',"
                          " updated INTEGER NOT NULL)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS profile_sent ("
                          " username TEXT PRIMARY KEY,"
                          " version INTEGER NOT NULL)"));
}

namespace {

const QString kMessageColumns =
    QStringLiteral("id, conversation, author, body, sent_at, status, uid, expire_seconds, expires_at, system,"
                   " quote_uid, quote_author, quote_text, deleted, edited_at");

ChatStore::Message messageFrom(const QSqlQuery &q)
{
    ChatStore::Message m;
    m.id = q.value(0).toLongLong();
    m.conversation = q.value(1).toLongLong();
    m.author = q.value(2).toString();
    m.body = q.value(3).toString();
    m.sentAt = QDateTime::fromMSecsSinceEpoch(q.value(4).toLongLong(), QTimeZone::utc());
    m.status = q.value(5).toString();
    m.uid = q.value(6).toString();
    m.expireSeconds = q.value(7).toLongLong();
    m.expiresAt = q.value(8).toLongLong();
    m.system = q.value(9).toBool();
    m.quoteUid = q.value(10).toString();
    m.quoteAuthor = q.value(11).toString();
    m.quoteText = q.value(12).toString();
    m.deleted = q.value(13).toBool();
    m.editedAt = q.value(14).toLongLong();
    return m;
}

int statusRank(const QString &status)
{
    if (status == QLatin1String("read"))
        return 3;
    if (status == QLatin1String("delivered"))
        return 2;
    if (status == QLatin1String("sent"))
        return 1;
    return 0;
}

} // namespace

QList<ChatStore::Conversation> ChatStore::conversations() const
{
    QList<Conversation> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT c.id, c.kind, c.title, c.peer, c.last_activity, c.state, c.expire_seconds,"
                          " c.muted_until, c.archived, c.pinned,"
                          // The newest message: its text, or else its first attachment's name.
                          " (SELECT CASE WHEN m.body = '' THEN COALESCE((SELECT CASE WHEN a.voice = 1 THEN '🎤 Voice message'"
                          "  ELSE '📎 ' || a.name END FROM attachments a WHERE a.message = m.id ORDER BY a.id LIMIT 1), '')"
                          "  ELSE m.body END"
                          "  FROM messages m WHERE m.conversation = c.id ORDER BY sent_at DESC, id DESC LIMIT 1),"
                          " (SELECT COUNT(*) FROM messages m WHERE m.conversation = c.id AND m.seen = 0 AND m.author <> ''),"
                          " (SELECT m.deleted FROM messages m WHERE m.conversation = c.id ORDER BY sent_at DESC, id DESC LIMIT 1)"
                          // Pinned first, in the order pinned; then the newest.
                          " FROM conversations c ORDER BY CASE WHEN c.pinned > 0 THEN 0 ELSE 1 END, c.pinned,"
                          " c.last_activity DESC, c.id DESC"));
    while (q.next()) {
        Conversation c;
        c.id = q.value(0).toLongLong();
        c.kind = q.value(1).toString();
        c.title = q.value(2).toString();
        c.peer = q.value(3).toString();
        c.lastActivity = QDateTime::fromMSecsSinceEpoch(q.value(4).toLongLong(), QTimeZone::utc());
        c.request = q.value(5).toString() == QLatin1String("request");
        c.left = q.value(5).toString() == QLatin1String("left");
        c.expireSeconds = q.value(6).toLongLong();
        c.mutedUntil = q.value(7).toLongLong();
        c.archived = q.value(8).toBool();
        c.pinned = q.value(9).toInt();
        c.lastText = q.value(10).toString();
        c.unread = q.value(11).toInt();
        c.lastDeleted = q.value(12).toBool();
        out.append(c);
    }
    return out;
}

ChatStore::Conversation ChatStore::conversation(qint64 id) const
{
    for (const Conversation &c : conversations()) {
        if (c.id == id)
            return c;
    }
    return {};
}

qint64 ChatStore::noteToSelf()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT id FROM conversations WHERE kind = 'self' LIMIT 1"));
    if (q.next())
        return q.value(0).toLongLong();
    return addConversation(QStringLiteral("self"), tr("Note to Self"), QString());
}

qint64 ChatStore::directConversation(const QString &peer) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM conversations WHERE kind = 'direct' AND peer = ? ORDER BY id LIMIT 1"));
    q.addBindValue(text(peer));
    if (q.exec() && q.next())
        return q.value(0).toLongLong();
    return 0;
}

qint64 ChatStore::addConversation(const QString &kind, const QString &title, const QString &peer, bool request)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO conversations (kind, title, peer, created, last_activity, state)"
                             " VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(text(kind));
    q.addBindValue(text(title));
    q.addBindValue(text(peer));
    q.addBindValue(now);
    q.addBindValue(now);
    q.addBindValue(request ? QStringLiteral("request") : QStringLiteral("accepted"));
    if (!q.exec()) {
        m_error = q.lastError().text();
        return 0;
    }
    emit conversationsChanged();
    return q.lastInsertId().toLongLong();
}

void ChatStore::setAccepted(qint64 conversation)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE conversations SET state = 'accepted' WHERE id = ? AND state <> 'accepted'"));
    q.addBindValue(conversation);
    if (q.exec() && q.numRowsAffected() > 0)
        emit conversationsChanged();
}

void ChatStore::setMutedUntil(qint64 conversation, qint64 untilMs)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE conversations SET muted_until = ? WHERE id = ?"));
    q.addBindValue(qMax<qint64>(0, untilMs));
    q.addBindValue(conversation);
    if (q.exec() && q.numRowsAffected() > 0)
        emit conversationsChanged();
}

void ChatStore::setArchived(qint64 conversation, bool archived)
{
    QSqlQuery q(m_db);
    q.prepare(archived ? QStringLiteral("UPDATE conversations SET archived = 1, pinned = 0 WHERE id = ?")
                       : QStringLiteral("UPDATE conversations SET archived = 0 WHERE id = ?"));
    q.addBindValue(conversation);
    if (q.exec() && q.numRowsAffected() > 0)
        emit conversationsChanged();
}

bool ChatStore::setPinned(qint64 conversation, bool pinned)
{
    QSqlQuery q(m_db);
    if (!pinned) {
        q.prepare(QStringLiteral("UPDATE conversations SET pinned = 0 WHERE id = ?"));
        q.addBindValue(conversation);
    } else {
        q.exec(QStringLiteral("SELECT COUNT(*), COALESCE(MAX(pinned), 0) FROM conversations WHERE pinned > 0"));
        if (!q.next())
            return false;
        if (q.value(0).toInt() >= MaxPinned)
            return false;
        const int next = q.value(1).toInt() + 1;
        q.prepare(QStringLiteral("UPDATE conversations SET pinned = ?, archived = 0 WHERE id = ? AND pinned = 0"));
        q.addBindValue(next);
        q.addBindValue(conversation);
    }
    if (q.exec() && q.numRowsAffected() > 0)
        emit conversationsChanged();
    return true;
}

void ChatStore::deleteConversation(qint64 conversation)
{
    if (!m_db.transaction())
        return;
    QSqlQuery q(m_db);
    // Never Note to Self.
    q.prepare(QStringLiteral("SELECT 1 FROM conversations WHERE id = ? AND kind <> 'self'"));
    q.addBindValue(conversation);
    if (!q.exec() || !q.next()) {
        m_db.rollback();
        return;
    }
    // Its attachments' files go too.
    QStringList files;
    q.prepare(QStringLiteral("SELECT a.file FROM attachments a JOIN messages m ON a.message = m.id"
                             " WHERE m.conversation = ? AND a.file <> ''"));
    q.addBindValue(conversation);
    q.exec();
    while (q.next())
        files << q.value(0).toString();
    q.prepare(QStringLiteral("DELETE FROM attachments WHERE message IN"
                             " (SELECT id FROM messages WHERE conversation = ?)"));
    q.addBindValue(conversation);
    q.exec();
    q.prepare(QStringLiteral("DELETE FROM messages WHERE conversation = ?"));
    q.addBindValue(conversation);
    q.exec();
    for (const char *table : {"group_members", "group_info"}) {
        q.prepare(QStringLiteral("DELETE FROM %1 WHERE conversation = ?").arg(QLatin1String(table)));
        q.addBindValue(conversation);
        q.exec();
    }
    for (const QString &name : files)
        QFile::remove(m_attachments + QLatin1Char('/') + name);
    q.prepare(QStringLiteral("DELETE FROM conversations WHERE id = ? AND kind <> 'self'"));
    q.addBindValue(conversation);
    q.exec();
    m_db.commit();
    emit messagesChanged(conversation);
    emit conversationsChanged();
}

void ChatStore::block(const QString &username)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO blocked (username, since) VALUES (?, ?)"));
    q.addBindValue(text(username));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

void ChatStore::unblock(const QString &username)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM blocked WHERE username = ?"));
    q.addBindValue(text(username));
    q.exec();
}

bool ChatStore::isBlocked(const QString &username) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM blocked WHERE username = ?"));
    q.addBindValue(text(username));
    return q.exec() && q.next();
}

QStringList ChatStore::blocked() const
{
    QStringList out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT username FROM blocked ORDER BY username"));
    while (q.next())
        out << q.value(0).toString();
    return out;
}

ChatStore::Message ChatStore::addMessage(qint64 conversation, const QString &body, const QString &author,
                                        const QDateTime &sentAt, const QString &uid, const QString &status)
{
    Message m;
    m.conversation = conversation;
    m.author = author;
    m.body = body;
    m.sentAt = sentAt.toUTC();
    m.uid = uid;
    // A note to self is there at once.
    m.status = !status.isEmpty() ? status : author.isEmpty() ? QStringLiteral("sent") : QStringLiteral("received");
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO messages (conversation, author, body, sent_at, status, seen, uid)"
                             " VALUES (?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(conversation);
    q.addBindValue(text(author));
    q.addBindValue(text(body));
    q.addBindValue(m.sentAt.toMSecsSinceEpoch());
    q.addBindValue(m.status);
    q.addBindValue(author.isEmpty() ? 1 : 0);
    q.addBindValue(text(uid));
    if (!q.exec()) {
        m_error = q.lastError().text();
        return {};
    }
    m.id = q.lastInsertId().toLongLong();
    QSqlQuery touch(m_db);
    touch.prepare(QStringLiteral("UPDATE conversations SET last_activity = MAX(last_activity, ?) WHERE id = ?"));
    touch.addBindValue(m.sentAt.toMSecsSinceEpoch());
    touch.addBindValue(conversation);
    touch.exec();
    if (!author.isEmpty()) {
        // Theirs brings an archived conversation back, unless it is muted (as Signal's).
        QSqlQuery back(m_db);
        back.prepare(QStringLiteral("UPDATE conversations SET archived = 0 WHERE id = ? AND archived = 1 AND muted_until <= ?"));
        back.addBindValue(conversation);
        back.addBindValue(QDateTime::currentMSecsSinceEpoch());
        back.exec();
    }
    emit messagesChanged(conversation);
    emit conversationsChanged();
    return m;
}

QList<ChatStore::Message> ChatStore::messages(qint64 conversation, int limit) const
{
    QList<Message> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM messages WHERE conversation = ?"
                             " ORDER BY sent_at DESC, id DESC LIMIT ?").arg(kMessageColumns));
    q.addBindValue(conversation);
    q.addBindValue(limit);
    q.exec();
    while (q.next())
        out.prepend(messageFrom(q));
    return out;
}

QList<qint64> ChatStore::findMessages(qint64 conversation, const QString &text) const
{
    QList<qint64> out;
    const QString wanted = text.simplified();
    if (wanted.isEmpty())
        return out;
    // Compared here, not by SQL's LIKE (which knows the case of ASCII only).
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, body FROM messages WHERE conversation = ? AND system = 0 AND deleted = 0"
                             " AND body <> '' ORDER BY sent_at, id"));
    q.addBindValue(conversation);
    q.exec();
    while (q.next()) {
        if (q.value(1).toString().simplified().contains(wanted, Qt::CaseInsensitive))
            out << q.value(0).toLongLong();
    }
    return out;
}

int ChatStore::messageCount(qint64 conversation) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM messages WHERE conversation = ?"));
    q.addBindValue(conversation);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

ChatStore::Message ChatStore::message(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM messages WHERE id = ?").arg(kMessageColumns));
    q.addBindValue(id);
    if (q.exec() && q.next())
        return messageFrom(q);
    return {};
}

bool ChatStore::hasMessage(qint64 conversation, const QString &uid) const
{
    if (uid.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM messages WHERE conversation = ? AND uid = ? LIMIT 1"));
    q.addBindValue(conversation);
    q.addBindValue(uid);
    return q.exec() && q.next();
}

namespace {

// The files of a message's attachments, and their rows, gone (in a transaction of the caller's).
QStringList dropAttachments(QSqlDatabase &db, qint64 message)
{
    QStringList names;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT file FROM attachments WHERE message = ? AND file <> ''"));
    q.addBindValue(message);
    q.exec();
    while (q.next())
        names << q.value(0).toString();
    q.prepare(QStringLiteral("DELETE FROM attachments WHERE message = ?"));
    q.addBindValue(message);
    q.exec();
    return names;
}

} // namespace

void ChatStore::editMessage(qint64 message, const QString &body, qint64 at, bool pending)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET body = ?, edited_at = ?, edit_pending = ? WHERE id = ? AND deleted = 0"));
    q.addBindValue(text(body));
    q.addBindValue(at);
    q.addBindValue(pending ? 1 : 0);
    q.addBindValue(message);
    if (q.exec()) {
        emit messagesChanged(this->message(message).conversation);
        emit conversationsChanged();
    }
}

QList<qint64> ChatStore::pendingEdits() const
{
    QList<qint64> out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT id FROM messages WHERE edit_pending = 1 AND deleted = 0 ORDER BY id"))) {
        while (q.next())
            out << q.value(0).toLongLong();
    }
    return out;
}

void ChatStore::setEditSent(qint64 message, qint64 at)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET edit_pending = 0 WHERE id = ? AND edited_at = ?"));
    q.addBindValue(message);
    q.addBindValue(at);
    q.exec();
}

void ChatStore::deleteMessage(qint64 message)
{
    const qint64 conversation = this->message(message).conversation;
    if (conversation == 0 || !m_db.transaction())
        return;
    const QStringList names = dropAttachments(m_db, message);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM messages WHERE id = ?"));
    q.addBindValue(message);
    q.exec();
    m_db.commit();
    for (const QString &name : names)
        QFile::remove(m_attachments + QLatin1Char('/') + name);
    emit messagesChanged(conversation);
    emit conversationsChanged();
}

void ChatStore::markDeleted(qint64 message, bool pending)
{
    const Message m = this->message(message);
    if (m.id == 0 || !m_db.transaction())
        return;
    const QStringList names = dropAttachments(m_db, message);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM reactions WHERE message = ?"));
    q.addBindValue(message);
    q.exec();
    q.prepare(QStringLiteral("UPDATE messages SET body = '', deleted = 1, delete_pending = ?,"
                             " quote_uid = '', quote_author = '', quote_text = '' WHERE id = ?"));
    q.addBindValue(pending ? 1 : 0);
    q.addBindValue(message);
    q.exec();
    // Replies keep only that they answered something deleted.
    if (!m.uid.isEmpty()) {
        q.prepare(QStringLiteral("UPDATE messages SET quote_text = '' WHERE conversation = ? AND quote_uid = ?"));
        q.addBindValue(m.conversation);
        q.addBindValue(m.uid);
        q.exec();
    }
    m_db.commit();
    for (const QString &name : names)
        QFile::remove(m_attachments + QLatin1Char('/') + name);
    emit messagesChanged(m.conversation);
    emit conversationsChanged();
}

QList<qint64> ChatStore::pendingDeletes() const
{
    QList<qint64> out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT id FROM messages WHERE delete_pending = 1 ORDER BY id"))) {
        while (q.next())
            out << q.value(0).toLongLong();
    }
    return out;
}

void ChatStore::setDeleteSent(qint64 message)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET delete_pending = 0 WHERE id = ?"));
    q.addBindValue(message);
    q.exec();
}

void ChatStore::setReaction(qint64 message, const QString &author, const QString &emoji, qint64 at, bool pending)
{
    QSqlQuery q(m_db);
    if (emoji.isEmpty() && !pending) {
        q.prepare(QStringLiteral("DELETE FROM reactions WHERE message = ? AND author = ?"));
        q.addBindValue(message);
        q.addBindValue(text(author));
    } else {
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO reactions (message, author, emoji, at, pending) VALUES (?, ?, ?, ?, ?)"));
        q.addBindValue(message);
        q.addBindValue(text(author));
        q.addBindValue(text(emoji));
        q.addBindValue(at);
        q.addBindValue(pending ? 1 : 0);
    }
    if (q.exec())
        emit messagesChanged(this->message(message).conversation);
}

QList<ChatStore::Reaction> ChatStore::reactions(qint64 message) const
{
    QList<Reaction> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT message, author, emoji, at FROM reactions WHERE message = ? AND emoji != ''"
                             " ORDER BY at, author"));
    q.addBindValue(message);
    if (q.exec()) {
        while (q.next())
            out.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toLongLong()});
    }
    return out;
}

QString ChatStore::myReaction(qint64 message) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT emoji FROM reactions WHERE message = ? AND author = ''"));
    q.addBindValue(message);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

QList<ChatStore::Reaction> ChatStore::pendingReactions() const
{
    QList<Reaction> out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT message, author, emoji, at FROM reactions WHERE pending = 1 ORDER BY at"))) {
        while (q.next())
            out.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toLongLong()});
    }
    return out;
}

void ChatStore::setReactionSent(qint64 message, qint64 at)
{
    // A removal, once sent, is gone; an emoji stays, no longer pending.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM reactions WHERE message = ? AND author = '' AND emoji = '' AND at = ?"));
    q.addBindValue(message);
    q.addBindValue(at);
    q.exec();
    q.prepare(QStringLiteral("UPDATE reactions SET pending = 0 WHERE message = ? AND author = '' AND at = ?"));
    q.addBindValue(message);
    q.addBindValue(at);
    q.exec();
}

qint64 ChatStore::messageByUid(qint64 conversation, const QString &uid) const
{
    if (uid.isEmpty())
        return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM messages WHERE conversation = ? AND uid = ? LIMIT 1"));
    q.addBindValue(conversation);
    q.addBindValue(uid);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

void ChatStore::setQuote(qint64 message, const QString &uid, const QString &author, const QString &excerpt)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET quote_uid = ?, quote_author = ?, quote_text = ? WHERE id = ?"));
    q.addBindValue(text(uid));
    q.addBindValue(text(author));
    q.addBindValue(text(excerpt));
    q.addBindValue(message);
    if (q.exec())
        emit messagesChanged(this->message(message).conversation);
}

QList<ChatStore::Message> ChatStore::outbox() const
{
    QList<Message> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT %1 FROM messages WHERE author = '' AND status = 'sending' ORDER BY id")
               .arg(kMessageColumns));
    while (q.next())
        out.append(messageFrom(q));
    return out;
}

void ChatStore::advanceStatus(qint64 conversation, const QStringList &uids, const QString &status)
{
    const int rank = statusRank(status);
    if (rank < 2)
        return;
    bool changed = false;
    QSqlQuery find(m_db);
    find.prepare(QStringLiteral("SELECT id, status FROM messages WHERE conversation = ? AND author = '' AND uid = ?"));
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral("UPDATE messages SET status = ? WHERE id = ?"));
    for (const QString &uid : uids) {
        find.bindValue(0, conversation);
        find.bindValue(1, uid);
        if (!find.exec())
            continue;
        while (find.next()) {
            // "sending" too: the answer to the send can come after the receipt.
            if (statusRank(find.value(1).toString()) >= rank || find.value(1).toString() == QLatin1String("failed"))
                continue;
            update.bindValue(0, status);
            update.bindValue(1, find.value(0));
            changed = update.exec() || changed;
        }
    }
    if (changed)
        emit messagesChanged(conversation);
}

QStringList ChatStore::unseenUids(qint64 conversation) const
{
    QStringList out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT uid FROM messages WHERE conversation = ? AND seen = 0 AND author <> '' AND uid <> ''"
                             " ORDER BY id"));
    q.addBindValue(conversation);
    q.exec();
    while (q.next())
        out << q.value(0).toString();
    return out;
}

void ChatStore::setStatus(qint64 message, const QString &status)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET status = ? WHERE id = ?"));
    q.addBindValue(status);
    q.addBindValue(message);
    q.exec();
    QSqlQuery which(m_db);
    which.prepare(QStringLiteral("SELECT conversation FROM messages WHERE id = ?"));
    which.addBindValue(message);
    if (which.exec() && which.next())
        emit messagesChanged(which.value(0).toLongLong());
}

void ChatStore::markRead(qint64 conversation)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET seen = 1 WHERE conversation = ? AND seen = 0"));
    q.addBindValue(conversation);
    q.exec();
    if (q.numRowsAffected() > 0)
        emit conversationsChanged();
}

namespace {

const QString kAttachmentColumns =
    QStringLiteral("id, message, name, type, size, key, digest, remote_id, file, state, voice");

ChatStore::StoredAttachment attachmentFrom(const QSqlQuery &q)
{
    ChatStore::StoredAttachment a;
    a.id = q.value(0).toLongLong();
    a.message = q.value(1).toLongLong();
    a.name = q.value(2).toString();
    a.type = q.value(3).toString();
    a.size = q.value(4).toLongLong();
    a.key = q.value(5).toByteArray();
    a.digest = q.value(6).toByteArray();
    a.remoteId = q.value(7).toString();
    a.file = q.value(8).toString();
    a.state = q.value(9).toString();
    a.voice = q.value(10).toBool();
    return a;
}

void updateAttachment(const QSqlDatabase &db, qint64 id, const QString &column, const QVariant &value)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE attachments SET %1 = ? WHERE id = ?").arg(column));
    q.addBindValue(value);
    q.addBindValue(id);
    q.exec();
}

} // namespace

qint64 ChatStore::addAttachment(const StoredAttachment &a)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO attachments (message, name, type, size, key, digest, remote_id, file, state, voice)"
                             " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(a.message);
    q.addBindValue(text(a.name));
    q.addBindValue(text(a.type));
    q.addBindValue(a.size);
    q.addBindValue(a.key);
    q.addBindValue(a.digest);
    q.addBindValue(text(a.remoteId));
    q.addBindValue(text(a.file));
    q.addBindValue(text(a.state));
    q.addBindValue(a.voice ? 1 : 0);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return 0;
    }
    const qint64 id = q.lastInsertId().toLongLong();
    emit messagesChanged(message(a.message).conversation);
    emit conversationsChanged();
    return id;
}

ChatStore::StoredAttachment ChatStore::attachment(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM attachments WHERE id = ?").arg(kAttachmentColumns));
    q.addBindValue(id);
    if (q.exec() && q.next())
        return attachmentFrom(q);
    return {};
}

QList<ChatStore::StoredAttachment> ChatStore::attachments(qint64 messageId) const
{
    QList<StoredAttachment> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM attachments WHERE message = ? ORDER BY id").arg(kAttachmentColumns));
    q.addBindValue(messageId);
    q.exec();
    while (q.next())
        out.append(attachmentFrom(q));
    return out;
}

QList<ChatStore::StoredAttachment> ChatStore::pendingDownloads() const
{
    QList<StoredAttachment> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT a.id, a.message, a.name, a.type, a.size, a.key, a.digest, a.remote_id, a.file,"
                          " a.state FROM attachments a JOIN messages m ON a.message = m.id"
                          " JOIN conversations c ON m.conversation = c.id"
                          " WHERE a.state = 'download' AND c.state = 'accepted' ORDER BY a.id"));
    while (q.next())
        out.append(attachmentFrom(q));
    return out;
}

void ChatStore::setAttachmentState(qint64 id, const QString &state)
{
    updateAttachment(m_db, id, QStringLiteral("state"), state);
    emit messagesChanged(message(attachment(id).message).conversation);
}

void ChatStore::setAttachmentUploaded(qint64 id, const QString &remoteId)
{
    updateAttachment(m_db, id, QStringLiteral("remote_id"), remoteId);
    updateAttachment(m_db, id, QStringLiteral("state"), QStringLiteral("sent"));
}

void ChatStore::setAttachmentFile(qint64 id, const QString &file)
{
    updateAttachment(m_db, id, QStringLiteral("file"), file);
}

QString ChatStore::writeBlob(const QByteArray &blob)
{
    for (int tries = 0; tries < 5; ++tries) {
        const QString name = QUuid::createUuid().toString(QUuid::Id128);
        QFile file(m_attachments + QLatin1Char('/') + name);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            continue; // taken: another name
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        if (file.write(blob) == blob.size() && file.flush())
            return name;
        file.remove();
        return {};
    }
    return {};
}

QByteArray ChatStore::readBlob(const QString &name) const
{
    // Only names of ours: never a way out of the folder.
    static const QRegularExpression ours(QStringLiteral("^[0-9a-f]{32}$"));
    if (!ours.match(name).hasMatch())
        return {};
    QFile file(m_attachments + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

// --- Profiles ---

namespace {

QString pointerText(const std::optional<Attachment::Pointer> &pointer)
{
    return pointer ? QString::fromUtf8(QJsonDocument(Attachment::toJson(*pointer)).toJson(QJsonDocument::Compact))
                   : QStringLiteral("");
}

std::optional<Attachment::Pointer> pointerFrom(const QString &text)
{
    if (text.isEmpty())
        return std::nullopt;
    return Attachment::fromJson(QJsonDocument::fromJson(text.toUtf8()).object());
}

} // namespace

ChatStore::Profile ChatStore::myProfile() const
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT name, avatar FROM my_profile WHERE id = 1"));
    if (q.next())
        return {q.value(0).toString(), q.value(1).toByteArray()};
    return {};
}

int ChatStore::myProfileVersion() const
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT version FROM my_profile WHERE id = 1"));
    return q.next() ? q.value(0).toInt() : 0;
}

void ChatStore::setMyProfile(const QString &name, const QByteArray &avatar)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO my_profile (id, name, avatar, version, avatar_pointer, avatar_uploaded)"
                             " VALUES (1, ?, ?, 1, '', 0) ON CONFLICT(id) DO UPDATE SET name = excluded.name,"
                             " avatar = excluded.avatar, version = my_profile.version + 1, avatar_pointer = '',"
                             " avatar_uploaded = 0"));
    q.addBindValue(text(name));
    q.addBindValue(avatar.isEmpty() ? QVariant() : QVariant(avatar));
    q.exec();
    emit profileChanged(QString());
}

std::optional<Attachment::Pointer> ChatStore::myAvatarPointer(qint64 notBefore) const
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT avatar_pointer, avatar_uploaded FROM my_profile WHERE id = 1"));
    if (!q.next() || q.value(1).toLongLong() < notBefore)
        return std::nullopt;
    return pointerFrom(q.value(0).toString());
}

void ChatStore::setMyAvatarPointer(const Attachment::Pointer &pointer, qint64 uploadedAt)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE my_profile SET avatar_pointer = ?, avatar_uploaded = ? WHERE id = 1"));
    q.addBindValue(pointerText(pointer));
    q.addBindValue(uploadedAt);
    q.exec();
}

ChatStore::Profile ChatStore::profile(const QString &username) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT name, avatar FROM profiles WHERE username = ?"));
    q.addBindValue(text(username));
    if (q.exec() && q.next())
        return {q.value(0).toString(), q.value(1).toByteArray()};
    return {};
}

void ChatStore::setProfile(const QString &username, const QString &name,
                           const std::optional<Attachment::Pointer> &avatarPointer)
{
    QSqlQuery q(m_db);
    // No picture: the old one goes. Another picture: the old one stays until it is here.
    q.prepare(QStringLiteral("INSERT INTO profiles (username, name, avatar, pending_avatar, updated)"
                             " VALUES (?, ?, NULL, ?, ?) ON CONFLICT(username) DO UPDATE SET name = excluded.name,"
                             " pending_avatar = excluded.pending_avatar, updated = excluded.updated,"
                             " avatar = CASE WHEN excluded.pending_avatar = '' THEN NULL ELSE profiles.avatar END"));
    q.addBindValue(text(username));
    q.addBindValue(text(name));
    q.addBindValue(pointerText(avatarPointer));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
    emit profileChanged(username);
}

std::optional<Attachment::Pointer> ChatStore::pendingAvatar(const QString &username) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT pending_avatar FROM profiles WHERE username = ?"));
    q.addBindValue(text(username));
    if (q.exec() && q.next())
        return pointerFrom(q.value(0).toString());
    return std::nullopt;
}

void ChatStore::setAvatar(const QString &username, const QByteArray &avatar)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE profiles SET avatar = ?, pending_avatar = '' WHERE username = ?"));
    q.addBindValue(avatar.isEmpty() ? QVariant() : QVariant(avatar));
    q.addBindValue(text(username));
    q.exec();
    emit profileChanged(username);
}

int ChatStore::profileSentVersion(const QString &username) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT version FROM profile_sent WHERE username = ?"));
    q.addBindValue(text(username));
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

void ChatStore::setProfileSent(const QString &username, int version)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO profile_sent (username, version) VALUES (?, ?)"));
    q.addBindValue(text(username));
    q.addBindValue(version);
    q.exec();
}

// --- Groups ---

qint64 ChatStore::groupConversation(const QString &groupId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM conversations WHERE kind = 'group' AND peer = ? LIMIT 1"));
    q.addBindValue(text(groupId));
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

ChatStore::Group ChatStore::group(qint64 conversation) const
{
    Group g;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT c.peer, c.title, i.admin, i.revision FROM conversations c"
                             " JOIN group_info i ON i.conversation = c.id WHERE c.id = ? AND c.kind = 'group'"));
    q.addBindValue(conversation);
    if (!q.exec() || !q.next())
        return g;
    g.conversation = conversation;
    g.id = q.value(0).toString();
    g.name = q.value(1).toString();
    g.admin = q.value(2).toString();
    g.revision = q.value(3).toInt();
    q.prepare(QStringLiteral("SELECT username FROM group_members WHERE conversation = ? ORDER BY username"));
    q.addBindValue(conversation);
    q.exec();
    while (q.next())
        g.members << q.value(0).toString();
    return g;
}

namespace {

void writeMembers(const QSqlDatabase &db, qint64 conversation, const QStringList &members)
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM group_members WHERE conversation = ?"));
    q.addBindValue(conversation);
    q.exec();
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO group_members (conversation, username) VALUES (?, ?)"));
    for (const QString &member : members) {
        q.bindValue(0, conversation);
        q.bindValue(1, member);
        q.exec();
    }
}

} // namespace

qint64 ChatStore::addGroup(const Group &g, bool request)
{
    const qint64 conversation = addConversation(QStringLiteral("group"), g.name, g.id, request);
    if (conversation == 0)
        return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO group_info (conversation, admin, revision) VALUES (?, ?, ?)"));
    q.addBindValue(conversation);
    q.addBindValue(text(g.admin));
    q.addBindValue(g.revision);
    q.exec();
    writeMembers(m_db, conversation, g.members);
    emit conversationsChanged();
    return conversation;
}

void ChatStore::updateGroup(const Group &g)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE conversations SET title = ? WHERE id = ?"));
    q.addBindValue(text(g.name));
    q.addBindValue(g.conversation);
    q.exec();
    q.prepare(QStringLiteral("UPDATE group_info SET admin = ?, revision = ? WHERE conversation = ?"));
    q.addBindValue(text(g.admin));
    q.addBindValue(g.revision);
    q.addBindValue(g.conversation);
    q.exec();
    writeMembers(m_db, g.conversation, g.members);
    emit conversationsChanged();
    emit messagesChanged(g.conversation);
}

void ChatStore::setLeft(qint64 conversation)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE conversations SET state = 'left' WHERE id = ? AND kind = 'group'"));
    q.addBindValue(conversation);
    q.exec();
    emit conversationsChanged();
    emit messagesChanged(conversation);
}

bool ChatStore::isMember(qint64 conversation, const QString &username) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM group_members WHERE conversation = ? AND username = ?"));
    q.addBindValue(conversation);
    q.addBindValue(text(username));
    return q.exec() && q.next();
}

void ChatStore::advanceStatusFrom(const QString &from, const QStringList &uids, const QString &status)
{
    // Every conversation with them: direct, and the groups they are in.
    QList<qint64> conversations;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM conversations WHERE kind = 'direct' AND peer = ?"
                             " UNION SELECT conversation FROM group_members WHERE username = ?"));
    q.addBindValue(text(from));
    q.addBindValue(text(from));
    q.exec();
    while (q.next())
        conversations << q.value(0).toLongLong();
    for (qint64 conversation : conversations)
        advanceStatus(conversation, uids, status);
}

QHash<QString, QStringList> ChatStore::unseenUidsByAuthor(qint64 conversation) const
{
    QHash<QString, QStringList> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT author, uid FROM messages WHERE conversation = ? AND seen = 0 AND author <> ''"
                             " AND uid <> '' ORDER BY id"));
    q.addBindValue(conversation);
    q.exec();
    while (q.next())
        out[q.value(0).toString()] << q.value(1).toString();
    return out;
}

void ChatStore::markSeenUids(const QStringList &uids)
{
    QSet<qint64> conversations;
    QSqlQuery find(m_db);
    find.prepare(QStringLiteral("SELECT conversation FROM messages WHERE uid = ? AND author <> '' AND seen = 0"));
    QSqlQuery mark(m_db);
    mark.prepare(QStringLiteral("UPDATE messages SET seen = 1 WHERE uid = ? AND author <> ''"));
    for (const QString &uid : uids) {
        if (uid.isEmpty())
            continue;
        find.bindValue(0, uid);
        if (find.exec()) {
            while (find.next())
                conversations.insert(find.value(0).toLongLong());
        }
        mark.bindValue(0, uid);
        mark.exec();
    }
    if (conversations.isEmpty())
        return;
    for (qint64 conversation : std::as_const(conversations))
        emit messagesChanged(conversation);
    emit conversationsChanged();
}

// --- Disappearing messages ---

void ChatStore::setExpireSeconds(qint64 conversation, qint64 seconds)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE conversations SET expire_seconds = ? WHERE id = ?"));
    q.addBindValue(seconds);
    q.addBindValue(conversation);
    q.exec();
    emit conversationsChanged();
}

void ChatStore::setMessageExpiry(qint64 message, qint64 seconds, qint64 startAt)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET expire_seconds = ?, expires_at = ? WHERE id = ?"));
    q.addBindValue(seconds);
    q.addBindValue(seconds > 0 && startAt > 0 ? startAt + seconds * 1000 : 0);
    q.addBindValue(message);
    q.exec();
    emit messagesChanged(this->message(message).conversation); // its ⏱ shows
}

void ChatStore::startExpiry(qint64 conversation, qint64 now)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET expires_at = ? + expire_seconds * 1000"
                             " WHERE conversation = ? AND expire_seconds > 0 AND expires_at = 0 AND seen = 1"));
    q.addBindValue(now);
    q.addBindValue(conversation);
    q.exec();
}

void ChatStore::startExpiryForUids(const QStringList &uids, qint64 now)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET expires_at = ? + expire_seconds * 1000"
                             " WHERE uid = ? AND expire_seconds > 0 AND expires_at = 0"));
    for (const QString &uid : uids) {
        q.bindValue(0, now);
        q.bindValue(1, uid);
        q.exec();
    }
}

int ChatStore::deleteExpired(qint64 now)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, conversation FROM messages WHERE expires_at > 0 AND expires_at <= ?"));
    q.addBindValue(now);
    q.exec();
    QList<qint64> ids;
    QSet<qint64> conversations;
    while (q.next()) {
        ids << q.value(0).toLongLong();
        conversations.insert(q.value(1).toLongLong());
    }
    if (ids.isEmpty() || !m_db.transaction())
        return 0;
    QSqlQuery files(m_db);
    files.prepare(QStringLiteral("SELECT file FROM attachments WHERE message = ? AND file <> ''"));
    QSqlQuery del(m_db);
    QStringList names;
    for (qint64 id : ids) {
        files.bindValue(0, id);
        files.exec();
        while (files.next())
            names << files.value(0).toString();
        del.prepare(QStringLiteral("DELETE FROM attachments WHERE message = ?"));
        del.addBindValue(id);
        del.exec();
        del.prepare(QStringLiteral("DELETE FROM messages WHERE id = ?"));
        del.addBindValue(id);
        del.exec();
    }
    m_db.commit();
    for (const QString &name : names)
        QFile::remove(m_attachments + QLatin1Char('/') + name);
    for (qint64 conversation : std::as_const(conversations))
        emit messagesChanged(conversation);
    emit conversationsChanged();
    return int(ids.size());
}

qint64 ChatStore::nextExpiry() const
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT MIN(expires_at) FROM messages WHERE expires_at > 0"));
    return q.next() ? q.value(0).toLongLong() : 0;
}

qint64 ChatStore::addSystemNote(qint64 conversation, const QString &text, const QDateTime &at)
{
    const Message note = addMessage(conversation, text, QString(), at, QString(), QStringLiteral("note"));
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET system = 1 WHERE id = ?"));
    q.addBindValue(note.id);
    q.exec();
    emit messagesChanged(conversation);
    return note.id;
}
