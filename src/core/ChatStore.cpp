#include "ChatStore.h"

#include "CipherSql.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
}

namespace {

const QString kMessageColumns = QStringLiteral("id, conversation, author, body, sent_at, status, uid");

ChatStore::Message messageFrom(const QSqlQuery &q)
{
    ChatStore::Message m;
    m.id = q.value(0).toLongLong();
    m.conversation = q.value(1).toLongLong();
    m.author = q.value(2).toString();
    m.body = q.value(3).toString();
    m.sentAt = QDateTime::fromMSecsSinceEpoch(q.value(4).toLongLong(), Qt::UTC);
    m.status = q.value(5).toString();
    m.uid = q.value(6).toString();
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
    q.exec(QStringLiteral("SELECT c.id, c.kind, c.title, c.peer, c.last_activity,"
                          " (SELECT body FROM messages m WHERE m.conversation = c.id ORDER BY sent_at DESC, id DESC LIMIT 1),"
                          " (SELECT COUNT(*) FROM messages m WHERE m.conversation = c.id AND m.seen = 0 AND m.author <> '')"
                          " FROM conversations c ORDER BY c.last_activity DESC, c.id DESC"));
    while (q.next()) {
        Conversation c;
        c.id = q.value(0).toLongLong();
        c.kind = q.value(1).toString();
        c.title = q.value(2).toString();
        c.peer = q.value(3).toString();
        c.lastActivity = QDateTime::fromMSecsSinceEpoch(q.value(4).toLongLong(), Qt::UTC);
        c.lastText = q.value(5).toString();
        c.unread = q.value(6).toInt();
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

qint64 ChatStore::addConversation(const QString &kind, const QString &title, const QString &peer)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO conversations (kind, title, peer, created, last_activity) VALUES (?, ?, ?, ?, ?)"));
    q.addBindValue(text(kind));
    q.addBindValue(text(title));
    q.addBindValue(text(peer));
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return 0;
    }
    emit conversationsChanged();
    return q.lastInsertId().toLongLong();
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
