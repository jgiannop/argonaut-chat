#include "ServerStore.h"

#include "Attachment.h"
#include "CipherSql.h"
#include "Crypto.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QVariant>

namespace {

Protocol::Envelope envelopeFrom(const QSqlQuery &q)
{
    Protocol::Envelope envelope;
    envelope.id = QString::number(q.value(0).toLongLong());
    envelope.from = q.value(1).toString();
    envelope.fromDevice = q.value(2).toUInt();
    envelope.serverTime = q.value(3).toLongLong();
    envelope.message = q.value(4).toByteArray();
    return envelope;
}

} // namespace

ServerStore::ServerStore(const QString &file)
    : m_connection(QStringLiteral("server-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    const QString path = file.isEmpty() ? defaultFile() : file;
    const QString folder = QFileInfo(path).absolutePath();
    QDir().mkpath(folder);
    QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    // Plain SQLite (public keys and messages it cannot read), through the
    // same library as the app's, so both can live in one process (tests).
    CipherSql::registerDriver();
    m_db = QSqlDatabase::addDatabase(CipherSql::DriverName, m_connection);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        m_error = m_db.lastError().text();
        return;
    }
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    m_attachments = folder + QStringLiteral("/attachments");
    QDir().mkpath(m_attachments);
    QFile::setPermissions(m_attachments, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QSqlQuery(m_db).exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    QSqlQuery(m_db).exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    createTables();
}

ServerStore::~ServerStore()
{
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connection);
}

QString ServerStore::defaultFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/Argonaut/argonaut-chat-server/server.sqlite");
}

void ServerStore::createTables()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS accounts ("
                          " username TEXT PRIMARY KEY,"
                          " created INTEGER NOT NULL)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS devices ("
                          " username TEXT NOT NULL REFERENCES accounts(username) ON DELETE CASCADE,"
                          " device INTEGER NOT NULL,"
                          " identity_key BLOB NOT NULL,"
                          " signed_prekey_id INTEGER NOT NULL,"
                          " signed_prekey BLOB NOT NULL,"
                          " signed_prekey_signature BLOB NOT NULL,"
                          " created INTEGER NOT NULL,"
                          " PRIMARY KEY (username, device))"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS one_time_prekeys ("
                          " username TEXT NOT NULL,"
                          " device INTEGER NOT NULL,"
                          " id INTEGER NOT NULL,"
                          " key BLOB NOT NULL,"
                          " PRIMARY KEY (username, device, id),"
                          " FOREIGN KEY (username, device) REFERENCES devices(username, device) ON DELETE CASCADE)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS queue ("
                          " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                          " to_user TEXT NOT NULL,"
                          " to_device INTEGER NOT NULL,"
                          " from_user TEXT NOT NULL,"
                          " from_device INTEGER NOT NULL,"
                          " received INTEGER NOT NULL,"
                          " message BLOB NOT NULL)"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS queue_by_device ON queue(to_user, to_device, id)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS attachments ("
                          " id TEXT PRIMARY KEY,"
                          " owner TEXT NOT NULL,"
                          " size INTEGER NOT NULL,"
                          " received INTEGER NOT NULL DEFAULT 0,"
                          " created INTEGER NOT NULL)"));
}

QString ServerStore::attachmentFile(const QString &id) const
{
    return m_attachments + QLatin1Char('/') + id;
}

QString ServerStore::createAttachment(const QString &owner, qint64 size)
{
    const QString id = QString::fromLatin1(Crypto::randomBytes(16).toHex());
    QFile file(attachmentFile(id));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return {};
    file.close();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO attachments (id, owner, size, created) VALUES (?, ?, ?, ?)"));
    q.addBindValue(id);
    q.addBindValue(owner);
    q.addBindValue(size);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        QFile::remove(attachmentFile(id));
        return {};
    }
    return id;
}

std::optional<ServerStore::AttachmentInfo> ServerStore::attachment(const QString &id) const
{
    if (!Attachment::isValidId(id))
        return std::nullopt;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT owner, size, received, created FROM attachments WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec() || !q.next())
        return std::nullopt;
    return AttachmentInfo{q.value(0).toString(), q.value(1).toLongLong(), q.value(2).toLongLong(),
                          q.value(3).toLongLong()};
}

bool ServerStore::appendAttachment(const QString &id, const QString &owner, qint64 offset, const QByteArray &data)
{
    const auto info = attachment(id);
    if (!info || info->owner != owner || offset != info->received || data.isEmpty()
        || offset + data.size() > info->size)
        return false;
    QFile file(attachmentFile(id));
    if (!file.open(QIODevice::ReadWrite) || file.size() < offset || !file.resize(offset) || !file.seek(offset)
        || file.write(data) != data.size() || !file.flush())
        return false;
    file.close();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE attachments SET received = ? WHERE id = ? AND received = ?"));
    q.addBindValue(offset + data.size());
    q.addBindValue(id);
    q.addBindValue(offset);
    return q.exec() && q.numRowsAffected() == 1;
}

QByteArray ServerStore::readAttachment(const QString &id, qint64 offset, qint64 length) const
{
    const auto info = attachment(id);
    if (!info || !info->complete() || offset < 0 || length <= 0 || offset >= info->size)
        return {};
    QFile file(attachmentFile(id));
    if (!file.open(QIODevice::ReadOnly) || !file.seek(offset))
        return {};
    return file.read(qMin(length, info->size - offset));
}

qint64 ServerStore::storedAttachmentBytes() const
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT COALESCE(SUM(size), 0) FROM attachments"));
    return q.next() ? q.value(0).toLongLong() : 0;
}

int ServerStore::removeOldAttachments(qint64 now, qint64 keepMs, qint64 unfinishedMs)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM attachments WHERE (received = size AND created < ?)"
                             " OR (received < size AND created < ?)"));
    q.addBindValue(now - keepMs);
    q.addBindValue(now - unfinishedMs);
    q.exec();
    QStringList ids;
    while (q.next())
        ids << q.value(0).toString();
    QSqlQuery del(m_db);
    del.prepare(QStringLiteral("DELETE FROM attachments WHERE id = ?"));
    for (const QString &id : ids) {
        QFile::remove(attachmentFile(id));
        del.bindValue(0, id);
        del.exec();
    }
    return int(ids.size());
}

bool ServerStore::accountExists(const QString &username) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM accounts WHERE username = ?"));
    q.addBindValue(username);
    return q.exec() && q.next();
}

bool ServerStore::createAccount(const QString &username, const QByteArray &identityKey,
                                const Protocol::PublicSignedPreKey &signedPreKey)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_db.transaction())
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO accounts (username, created) VALUES (?, ?)"));
    q.addBindValue(username);
    q.addBindValue(now);
    if (!q.exec()) { // the name is taken
        m_db.rollback();
        return false;
    }
    q.prepare(QStringLiteral("INSERT INTO devices (username, device, identity_key, signed_prekey_id, signed_prekey,"
                             " signed_prekey_signature, created) VALUES (?, 1, ?, ?, ?, ?, ?)"));
    q.addBindValue(username);
    q.addBindValue(identityKey);
    q.addBindValue(qint64(signedPreKey.id));
    q.addBindValue(signedPreKey.key);
    q.addBindValue(signedPreKey.signature);
    q.addBindValue(now);
    if (!q.exec()) {
        m_db.rollback();
        return false;
    }
    return m_db.commit();
}

QList<quint32> ServerStore::devices(const QString &username) const
{
    QList<quint32> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT device FROM devices WHERE username = ? ORDER BY device"));
    q.addBindValue(username);
    q.exec();
    while (q.next())
        out.append(q.value(0).toUInt());
    return out;
}

QByteArray ServerStore::identityKey(const QString &username, quint32 device) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT identity_key FROM devices WHERE username = ? AND device = ?"));
    q.addBindValue(username);
    q.addBindValue(qint64(device));
    if (q.exec() && q.next())
        return q.value(0).toByteArray();
    return {};
}

bool ServerStore::setSignedPreKey(const QString &username, quint32 device, const Protocol::PublicSignedPreKey &key)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE devices SET signed_prekey_id = ?, signed_prekey = ?, signed_prekey_signature = ?"
                             " WHERE username = ? AND device = ?"));
    q.addBindValue(qint64(key.id));
    q.addBindValue(key.key);
    q.addBindValue(key.signature);
    q.addBindValue(username);
    q.addBindValue(qint64(device));
    return q.exec() && q.numRowsAffected() == 1;
}

bool ServerStore::addOneTimePreKeys(const QString &username, quint32 device,
                                    const QList<Protocol::PublicOneTimePreKey> &keys)
{
    if (!m_db.transaction())
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO one_time_prekeys (username, device, id, key) VALUES (?, ?, ?, ?)"));
    for (const Protocol::PublicOneTimePreKey &key : keys) {
        q.bindValue(0, username);
        q.bindValue(1, qint64(device));
        q.bindValue(2, qint64(key.id));
        q.bindValue(3, key.key);
        if (!q.exec()) {
            m_db.rollback();
            return false;
        }
    }
    // Never more than the limit: the oldest ids go first.
    q.prepare(QStringLiteral("DELETE FROM one_time_prekeys WHERE username = ? AND device = ? AND id NOT IN"
                             " (SELECT id FROM one_time_prekeys WHERE username = ? AND device = ?"
                             " ORDER BY rowid DESC LIMIT ?)"));
    q.addBindValue(username);
    q.addBindValue(qint64(device));
    q.addBindValue(username);
    q.addBindValue(qint64(device));
    q.addBindValue(MaxOneTimePreKeys);
    if (!q.exec()) {
        m_db.rollback();
        return false;
    }
    return m_db.commit();
}

int ServerStore::oneTimePreKeyCount(const QString &username, quint32 device) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM one_time_prekeys WHERE username = ? AND device = ?"));
    q.addBindValue(username);
    q.addBindValue(qint64(device));
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

std::optional<X3dh::PreKeyBundle> ServerStore::takeBundle(const QString &username, quint32 device, bool withOneTime)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT identity_key, signed_prekey_id, signed_prekey, signed_prekey_signature"
                             " FROM devices WHERE username = ? AND device = ?"));
    q.addBindValue(username);
    q.addBindValue(qint64(device));
    if (!q.exec() || !q.next())
        return std::nullopt;
    X3dh::PreKeyBundle bundle;
    bundle.identityKey = q.value(0).toByteArray();
    bundle.signedPreKeyId = q.value(1).toUInt();
    bundle.signedPreKey = q.value(2).toByteArray();
    bundle.signature = q.value(3).toByteArray();

    q.prepare(QStringLiteral("SELECT rowid, id, key FROM one_time_prekeys WHERE username = ? AND device = ?"
                             " ORDER BY rowid LIMIT 1"));
    q.addBindValue(username);
    q.addBindValue(qint64(device));
    if (withOneTime && q.exec() && q.next()) {
        const qint64 rowid = q.value(0).toLongLong();
        bundle.oneTimePreKeyId = q.value(1).toUInt();
        bundle.oneTimePreKey = q.value(2).toByteArray();
        QSqlQuery del(m_db);
        del.prepare(QStringLiteral("DELETE FROM one_time_prekeys WHERE rowid = ?"));
        del.addBindValue(rowid);
        if (!del.exec()) // never hand out the same one twice
            return std::nullopt;
    }
    return bundle;
}

qint64 ServerStore::queue(const QString &to, quint32 toDevice, const QString &from, quint32 fromDevice,
                          const QByteArray &message)
{
    if (queuedCount(to, toDevice) >= MaxQueuedMessages)
        return 0;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO queue (to_user, to_device, from_user, from_device, received, message)"
                             " VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(to);
    q.addBindValue(qint64(toDevice));
    q.addBindValue(from);
    q.addBindValue(qint64(fromDevice));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(message);
    if (!q.exec())
        return 0;
    return q.lastInsertId().toLongLong();
}

QList<Protocol::Envelope> ServerStore::queued(const QString &to, quint32 toDevice, int limit) const
{
    QList<Protocol::Envelope> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, from_user, from_device, received, message FROM queue"
                             " WHERE to_user = ? AND to_device = ? ORDER BY id LIMIT ?"));
    q.addBindValue(to);
    q.addBindValue(qint64(toDevice));
    q.addBindValue(limit);
    q.exec();
    while (q.next())
        out.append(envelopeFrom(q));
    return out;
}

std::optional<Protocol::Envelope> ServerStore::envelope(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, from_user, from_device, received, message FROM queue WHERE id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next())
        return envelopeFrom(q);
    return std::nullopt;
}

int ServerStore::queuedCount(const QString &to, quint32 toDevice) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM queue WHERE to_user = ? AND to_device = ?"));
    q.addBindValue(to);
    q.addBindValue(qint64(toDevice));
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

void ServerStore::remove(const QString &to, quint32 toDevice, const QList<qint64> &ids)
{
    if (!m_db.transaction())
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM queue WHERE id = ? AND to_user = ? AND to_device = ?"));
    for (qint64 id : ids) {
        q.bindValue(0, id);
        q.bindValue(1, to);
        q.bindValue(2, qint64(toDevice));
        q.exec();
    }
    m_db.commit();
}
