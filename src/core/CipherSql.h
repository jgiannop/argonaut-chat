#pragma once

#include <QByteArray>
#include <QSqlDriver>
#include <QSqlResult>
#include <QString>

struct sqlite3;
struct sqlite3_stmt;

// SQLCipher (an encrypted SQLite: AES-256 and HMAC-SHA512 on every page) as
// a Qt SQL driver, "QSQLCIPHER": Qt's own QSQLITE is built on plain SQLite
// and cannot open these files. The stores keep using QSqlDatabase and
// QSqlQuery; only the driver's name changes.
//
// The key is the database's password: CipherSql::password(key) makes it from
// 32 random bytes (a raw key: no passphrase stretching needed). Without a
// password the file is plain SQLite (the server's, and tests).
//
// No process should load QSQLITE as well: both libraries define the same
// symbols.
namespace CipherSql {

inline const QString DriverName = QStringLiteral("QSQLCIPHER");
constexpr int KeySize = 32;

// Once per process, before the first QSqlDatabase::addDatabase.
void registerDriver();
// The password for a key: x'…' in hex. Empty if the key is not 32 bytes.
QString password(const QByteArray &key);
// The file starts as plain SQLite does ("SQLite format 3").
bool isPlainSqlite(const QString &file);
// Encrypts a plain SQLite file in place with `key` (sqlcipher_export).
bool encryptFile(const QString &file, const QByteArray &key, QString *error = nullptr);

} // namespace CipherSql

class CipherSqlDriver : public QSqlDriver
{
    Q_OBJECT

public:
    explicit CipherSqlDriver(QObject *parent = nullptr);
    ~CipherSqlDriver() override;

    bool hasFeature(DriverFeature feature) const override;
    bool open(const QString &db, const QString &user, const QString &password, const QString &host, int port,
              const QString &options) override;
    void close() override;
    QSqlResult *createResult() const override;
    bool beginTransaction() override;
    bool commitTransaction() override;
    bool rollbackTransaction() override;
    QVariant handle() const override;

    sqlite3 *connection() const { return m_db; }

private:
    bool run(const char *sql, const QString &what);

    sqlite3 *m_db = nullptr;
};
