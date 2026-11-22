#include "KeyStore.h"

#include "ChatStore.h"

#include <QDateTime>
#include <QSqlQuery>
#include <QVariant>

KeyStore::KeyStore(ChatStore *store)
    : m_db(store->database())
{
    createTables();
}

void KeyStore::createTables()
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS account ("
                          " id INTEGER PRIMARY KEY CHECK (id = 1),"
                          " username TEXT NOT NULL,"
                          " device INTEGER NOT NULL,"
                          " server TEXT NOT NULL,"
                          " identity_secret BLOB NOT NULL,"
                          " registered INTEGER NOT NULL DEFAULT 0,"
                          " next_prekey_id INTEGER NOT NULL DEFAULT 1)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS signed_prekeys ("
                          " id INTEGER PRIMARY KEY,"
                          " public_key BLOB NOT NULL,"
                          " secret_key BLOB NOT NULL,"
                          " signature BLOB NOT NULL,"
                          " created INTEGER NOT NULL)"));
    // Added with the rotation of signed prekeys.
    bool hasUploaded = false;
    q.exec(QStringLiteral("PRAGMA table_info(signed_prekeys)"));
    while (q.next())
        hasUploaded = hasUploaded || q.value(1).toString() == QLatin1String("uploaded");
    if (!hasUploaded)
        q.exec(QStringLiteral("ALTER TABLE signed_prekeys ADD COLUMN uploaded INTEGER NOT NULL DEFAULT 1"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS one_time_prekeys ("
                          " id INTEGER PRIMARY KEY,"
                          " public_key BLOB NOT NULL,"
                          " secret_key BLOB NOT NULL)"));
    // Added with PQXDH: each prekey's KEM prekey (NULL in keys made before).
    for (const char *table : {"signed_prekeys", "one_time_prekeys"}) {
        QStringList columns;
        q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(QLatin1String(table)));
        while (q.next())
            columns << q.value(1).toString();
        for (const char *column : {"kem_public", "kem_secret", "kem_signature"}) {
            if (!columns.contains(QLatin1String(column)))
                q.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 BLOB").arg(QLatin1String(table), QLatin1String(column)));
        }
    }
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS sessions ("
                          " username TEXT NOT NULL,"
                          " device INTEGER NOT NULL,"
                          " data BLOB NOT NULL,"
                          " updated INTEGER NOT NULL,"
                          " PRIMARY KEY (username, device))"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS identities ("
                          " username TEXT PRIMARY KEY,"
                          " identity_key BLOB NOT NULL,"
                          " first_seen INTEGER NOT NULL,"
                          " changed INTEGER NOT NULL DEFAULT 0,"
                          " verified INTEGER NOT NULL DEFAULT 0)"));
}

std::optional<KeyStore::Account> KeyStore::account() const
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT username, device, server, identity_secret, registered FROM account WHERE id = 1"));
    if (!q.next())
        return std::nullopt;
    Account account;
    account.username = q.value(0).toString();
    account.device = q.value(1).toUInt();
    account.server = QUrl(q.value(2).toString());
    QByteArray secret = q.value(3).toByteArray();
    account.identity = Crypto::IdentityKeyPair::fromSecretKey(secret);
    Crypto::wipe(secret); // the identity keeps its own, locked copy
    account.registered = q.value(4).toBool();
    if (!account.identity.isValid())
        return std::nullopt;
    return account;
}

bool KeyStore::saveAccount(const Account &account)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO account (id, username, device, server, identity_secret, registered,"
                             " next_prekey_id) VALUES (1, ?, ?, ?, ?, ?,"
                             " COALESCE((SELECT next_prekey_id FROM account WHERE id = 1), 1))"));
    q.addBindValue(account.username);
    q.addBindValue(qint64(account.device));
    q.addBindValue(account.server.toString());
    QByteArray secret = account.identity.secretKey();
    q.addBindValue(secret);
    q.addBindValue(account.registered ? 1 : 0);
    const bool saved = q.exec();
    q = QSqlQuery(); // lets go of the bound copy, so the wipe reaches the bytes
    Crypto::wipe(secret);
    return saved;
}

bool KeyStore::setRegistered()
{
    QSqlQuery q(m_db);
    return q.exec(QStringLiteral("UPDATE account SET registered = 1 WHERE id = 1")) && q.numRowsAffected() == 1;
}

void KeyStore::forgetAccount()
{
    QSqlQuery q(m_db);
    for (const char *table : {"account", "signed_prekeys", "one_time_prekeys", "sessions", "identities"})
        q.exec(QStringLiteral("DELETE FROM %1").arg(QLatin1String(table)));
}

bool KeyStore::addSignedPreKey(const X3dh::SignedPreKey &key, qint64 created, bool uploaded)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO signed_prekeys (id, public_key, secret_key, signature, created,"
                             " uploaded, kem_public, kem_secret, kem_signature) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(qint64(key.id));
    q.addBindValue(key.pair.publicKey);
    q.addBindValue(key.pair.secretKey);
    q.addBindValue(key.signature);
    q.addBindValue(created >= 0 ? created : QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(uploaded ? 1 : 0);
    q.addBindValue(key.kemPair.publicKey);
    q.addBindValue(key.kemPair.secretKey);
    q.addBindValue(key.kemSignature);
    return q.exec();
}

QList<KeyStore::SignedPreKeyInfo> KeyStore::signedPreKeys() const
{
    QList<SignedPreKeyInfo> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT id, created, uploaded FROM signed_prekeys ORDER BY created, id"));
    while (q.next())
        out.append({q.value(0).toUInt(), q.value(1).toLongLong(), q.value(2).toBool()});
    return out;
}

void KeyStore::setSignedPreKeyUploaded(quint32 id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE signed_prekeys SET uploaded = 1 WHERE id = ?"));
    q.addBindValue(qint64(id));
    q.exec();
}

void KeyStore::removeSignedPreKey(quint32 id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM signed_prekeys WHERE id = ?"));
    q.addBindValue(qint64(id));
    q.exec();
}

namespace {

std::optional<X3dh::SignedPreKey> signedFrom(QSqlQuery &q)
{
    if (!q.exec() || !q.next())
        return std::nullopt;
    X3dh::SignedPreKey key;
    key.id = q.value(0).toUInt();
    key.pair.publicKey = q.value(1).toByteArray();
    key.pair.secretKey = q.value(2).toByteArray();
    key.signature = q.value(3).toByteArray();
    key.kemPair.publicKey = q.value(4).toByteArray();
    key.kemPair.secretKey = q.value(5).toByteArray();
    key.kemSignature = q.value(6).toByteArray();
    if (!key.pair.isValid())
        return std::nullopt;
    if (!key.kemPair.isValid())
        key.kemPair = {}; // one from before PQXDH
    return key;
}

} // namespace

std::optional<X3dh::SignedPreKey> KeyStore::signedPreKey(quint32 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, public_key, secret_key, signature, kem_public, kem_secret, kem_signature FROM signed_prekeys WHERE id = ?"));
    q.addBindValue(qint64(id));
    return signedFrom(q);
}

std::optional<X3dh::SignedPreKey> KeyStore::latestSignedPreKey() const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, public_key, secret_key, signature, kem_public, kem_secret, kem_signature FROM signed_prekeys"
                             " ORDER BY created DESC, id DESC LIMIT 1"));
    return signedFrom(q);
}

bool KeyStore::addOneTimePreKeys(const QList<X3dh::OneTimePreKey> &keys)
{
    if (!m_db.transaction())
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO one_time_prekeys (id, public_key, secret_key, kem_public, kem_secret,"
                             " kem_signature) VALUES (?, ?, ?, ?, ?, ?)"));
    for (const X3dh::OneTimePreKey &key : keys) {
        q.bindValue(0, qint64(key.id));
        q.bindValue(1, key.pair.publicKey);
        q.bindValue(2, key.pair.secretKey);
        q.bindValue(3, key.kemPair.publicKey);
        q.bindValue(4, key.kemPair.secretKey);
        q.bindValue(5, key.kemSignature);
        if (!q.exec()) {
            m_db.rollback();
            return false;
        }
    }
    return m_db.commit();
}

std::optional<X3dh::OneTimePreKey> KeyStore::oneTimePreKey(quint32 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT public_key, secret_key, kem_public, kem_secret, kem_signature FROM one_time_prekeys"
                             " WHERE id = ?"));
    q.addBindValue(qint64(id));
    if (!q.exec() || !q.next())
        return std::nullopt;
    X3dh::OneTimePreKey key;
    key.id = id;
    key.pair.publicKey = q.value(0).toByteArray();
    key.pair.secretKey = q.value(1).toByteArray();
    key.kemPair.publicKey = q.value(2).toByteArray();
    key.kemPair.secretKey = q.value(3).toByteArray();
    key.kemSignature = q.value(4).toByteArray();
    if (!key.pair.isValid())
        return std::nullopt;
    if (!key.kemPair.isValid())
        key.kemPair = {}; // one from before PQXDH
    return key;
}

void KeyStore::removeOneTimePreKey(quint32 id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM one_time_prekeys WHERE id = ?"));
    q.addBindValue(qint64(id));
    q.exec();
}

int KeyStore::oneTimePreKeyCount() const
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT COUNT(*) FROM one_time_prekeys"));
    return q.next() ? q.value(0).toInt() : 0;
}

quint32 KeyStore::takePreKeyIds(int count)
{
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT next_prekey_id FROM account WHERE id = 1"));
    const quint32 first = q.next() ? q.value(0).toUInt() : 1;
    QSqlQuery update(m_db);
    update.prepare(QStringLiteral("UPDATE account SET next_prekey_id = ? WHERE id = 1"));
    update.addBindValue(qint64(first) + count);
    update.exec();
    return first;
}

std::optional<Session> KeyStore::session(const QString &username, quint32 device) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT data FROM sessions WHERE username = ? AND device = ?"));
    q.addBindValue(username);
    q.addBindValue(qint64(device));
    if (!q.exec() || !q.next())
        return std::nullopt;
    Session session = Session::deserialize(q.value(0).toByteArray());
    if (!session.isValid())
        return std::nullopt;
    return session;
}

bool KeyStore::saveSession(const QString &username, quint32 device, const Session &session)
{
    const QByteArray data = session.serialize();
    if (data.isEmpty())
        return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO sessions (username, device, data, updated) VALUES (?, ?, ?, ?)"));
    q.addBindValue(username);
    q.addBindValue(qint64(device));
    q.addBindValue(data);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    return q.exec();
}

QList<quint32> KeyStore::sessionDevices(const QString &username) const
{
    QList<quint32> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT device FROM sessions WHERE username = ? ORDER BY device"));
    q.addBindValue(username);
    q.exec();
    while (q.next())
        out.append(q.value(0).toUInt());
    return out;
}

void KeyStore::removeSession(const QString &username, quint32 device)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM sessions WHERE username = ? AND device = ?"));
    q.addBindValue(username);
    q.addBindValue(qint64(device));
    q.exec();
}

KeyStore::Trust KeyStore::checkIdentity(const QString &username, const QByteArray &identityKey)
{
    const QByteArray known = this->identityKey(username);
    if (known == identityKey)
        return Trust::Same;
    QSqlQuery q(m_db);
    if (known.isEmpty()) {
        q.prepare(QStringLiteral("INSERT INTO identities (username, identity_key, first_seen) VALUES (?, ?, ?)"));
        q.addBindValue(username);
        q.addBindValue(identityKey);
        q.addBindValue(QDateTime::currentMSecsSinceEpoch());
        q.exec();
        return Trust::New;
    }
    q.prepare(QStringLiteral("UPDATE identities SET identity_key = ?, first_seen = ?, changed = 1, verified = 0"
                             " WHERE username = ?"));
    q.addBindValue(identityKey);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(username);
    q.exec();
    return Trust::Changed;
}

QByteArray KeyStore::identityKey(const QString &username) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT identity_key FROM identities WHERE username = ?"));
    q.addBindValue(username);
    return q.exec() && q.next() ? q.value(0).toByteArray() : QByteArray();
}

bool KeyStore::identityChanged(const QString &username) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT changed FROM identities WHERE username = ?"));
    q.addBindValue(username);
    return q.exec() && q.next() && q.value(0).toBool();
}

void KeyStore::setVerified(const QString &username, bool verified)
{
    QSqlQuery q(m_db);
    // Looking at the new number and accepting it ends the warning.
    q.prepare(QStringLiteral("UPDATE identities SET verified = ?, changed = 0 WHERE username = ?"));
    q.addBindValue(verified ? 1 : 0);
    q.addBindValue(username);
    q.exec();
}

bool KeyStore::isVerified(const QString &username) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT verified FROM identities WHERE username = ?"));
    q.addBindValue(username);
    return q.exec() && q.next() && q.value(0).toBool();
}
