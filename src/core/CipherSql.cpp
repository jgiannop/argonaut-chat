#include "CipherSql.h"

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlField>
#include <QSqlRecord>
#include <QVariant>

#include <sqlite3.h>

namespace {

QString sqliteError(sqlite3 *db)
{
    return db ? QString::fromUtf8(sqlite3_errmsg(db)) : QStringLiteral("no database");
}

// One statement, forward only.
class CipherResult : public QSqlResult
{
public:
    explicit CipherResult(const CipherSqlDriver *driver)
        : QSqlResult(driver)
        , m_driver(driver)
    {
        setForwardOnly(true);
    }
    ~CipherResult() override { finalize(); }

protected:
    QVariant data(int i) override
    {
        if (!m_stmt || i < 0 || i >= sqlite3_column_count(m_stmt))
            return {};
        switch (sqlite3_column_type(m_stmt, i)) {
        case SQLITE_INTEGER:
            return qint64(sqlite3_column_int64(m_stmt, i));
        case SQLITE_FLOAT:
            return sqlite3_column_double(m_stmt, i);
        case SQLITE_TEXT:
            return QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(m_stmt, i)),
                                     sqlite3_column_bytes(m_stmt, i));
        case SQLITE_BLOB: {
            const void *blob = sqlite3_column_blob(m_stmt, i);
            return QByteArray(static_cast<const char *>(blob), sqlite3_column_bytes(m_stmt, i));
        }
        default:
            return {};
        }
    }

    bool isNull(int i) override
    {
        return !m_stmt || sqlite3_column_type(m_stmt, i) == SQLITE_NULL;
    }

    bool reset(const QString &query) override
    {
        return prepare(query) && exec();
    }

    bool prepare(const QString &query) override
    {
        finalize();
        setActive(false);
        setAt(QSql::BeforeFirstRow);
        sqlite3 *db = m_driver->connection();
        if (!db) {
            setLastError(QSqlError(QStringLiteral("Not open"), QString(), QSqlError::ConnectionError));
            return false;
        }
        const QByteArray sql = query.toUtf8();
        if (sqlite3_prepare_v2(db, sql.constData(), int(sql.size()), &m_stmt, nullptr) != SQLITE_OK) {
            setLastError(QSqlError(QStringLiteral("Unable to prepare the statement"), sqliteError(db),
                                   QSqlError::StatementError));
            finalize();
            return false;
        }
        return true;
    }

    bool exec() override
    {
        sqlite3 *db = m_driver->connection();
        if (!m_stmt || !db)
            return false;
        sqlite3_reset(m_stmt);
        sqlite3_clear_bindings(m_stmt);
        m_hasRow = false;
        m_done = false;
        setAt(QSql::BeforeFirstRow);
        const QVariantList values = boundValues();
        if (values.size() != sqlite3_bind_parameter_count(m_stmt)) {
            setLastError(QSqlError(QStringLiteral("Wrong number of bound values"), QString(),
                                   QSqlError::StatementError));
            return false;
        }
        for (int i = 0; i < values.size(); ++i) {
            if (!bind(i + 1, values.at(i))) {
                setLastError(QSqlError(QStringLiteral("Unable to bind a value"), sqliteError(db),
                                       QSqlError::StatementError));
                return false;
            }
        }
        const int rc = sqlite3_step(m_stmt);
        if (rc == SQLITE_ROW) {
            m_hasRow = true;
        } else if (rc == SQLITE_DONE) {
            m_done = true;
        } else {
            setLastError(QSqlError(QStringLiteral("Unable to run the statement"), sqliteError(db),
                                   QSqlError::StatementError));
            sqlite3_reset(m_stmt);
            setActive(false);
            return false;
        }
        const bool select = sqlite3_column_count(m_stmt) > 0;
        setSelect(select);
        m_changes = select ? 0 : sqlite3_changes(db);
        m_lastInsertId = sqlite3_last_insert_rowid(db);
        setActive(true);
        return true;
    }

    bool fetch(int i) override
    {
        return i == at() + 1 && fetchNext();
    }

    bool fetchFirst() override
    {
        return at() == QSql::BeforeFirstRow && fetchNext();
    }

    bool fetchLast() override
    {
        return false; // forward only
    }

    bool fetchNext() override
    {
        if (!m_stmt)
            return false;
        if (m_hasRow) { // the row exec() stepped onto
            m_hasRow = false;
            setAt(at() == QSql::BeforeFirstRow ? 0 : at() + 1);
            return true;
        }
        if (m_done) {
            setAt(QSql::AfterLastRow);
            return false;
        }
        const int rc = sqlite3_step(m_stmt);
        if (rc == SQLITE_ROW) {
            setAt(at() + 1);
            return true;
        }
        m_done = true;
        if (rc != SQLITE_DONE)
            setLastError(QSqlError(QStringLiteral("Unable to fetch a row"), sqliteError(m_driver->connection()),
                                   QSqlError::StatementError));
        setAt(QSql::AfterLastRow);
        return false;
    }

    int size() override { return -1; }
    int numRowsAffected() override { return m_changes; }
    QVariant lastInsertId() const override { return m_lastInsertId; }

    QSqlRecord record() const override
    {
        QSqlRecord out;
        if (!m_stmt)
            return out;
        for (int i = 0; i < sqlite3_column_count(m_stmt); ++i)
            out.append(QSqlField(QString::fromUtf8(sqlite3_column_name(m_stmt, i))));
        return out;
    }

    QVariant handle() const override
    {
        return QVariant::fromValue(static_cast<void *>(m_stmt));
    }

    void detachFromResultSet() override
    {
        if (m_stmt)
            sqlite3_reset(m_stmt);
    }

private:
    bool bind(int index, const QVariant &value)
    {
        if (value.isNull())
            return sqlite3_bind_null(m_stmt, index) == SQLITE_OK;
        switch (value.typeId()) {
        case QMetaType::QByteArray: {
            const QByteArray bytes = value.toByteArray();
            return sqlite3_bind_blob(m_stmt, index, bytes.constData(), int(bytes.size()), SQLITE_TRANSIENT)
                   == SQLITE_OK;
        }
        case QMetaType::Bool:
        case QMetaType::Char:
        case QMetaType::SChar:
        case QMetaType::UChar:
        case QMetaType::Short:
        case QMetaType::UShort:
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::Long:
        case QMetaType::ULong:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
            return sqlite3_bind_int64(m_stmt, index, value.toLongLong()) == SQLITE_OK;
        case QMetaType::Double:
        case QMetaType::Float:
            return sqlite3_bind_double(m_stmt, index, value.toDouble()) == SQLITE_OK;
        default: {
            const QByteArray text = value.toString().toUtf8();
            return sqlite3_bind_text(m_stmt, index, text.constData(), int(text.size()), SQLITE_TRANSIENT)
                   == SQLITE_OK;
        }
        }
    }

    void finalize()
    {
        if (m_stmt)
            sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
        m_hasRow = false;
        m_done = false;
    }

    const CipherSqlDriver *m_driver;
    sqlite3_stmt *m_stmt = nullptr;
    bool m_hasRow = false; // exec() stepped onto a row not fetched yet
    bool m_done = false;   // no more rows: never step again (it would start over)
    int m_changes = 0;
    qint64 m_lastInsertId = 0;
};

bool isHexKey(const QString &password)
{
    // x'<64 hex digits>'
    if (password.size() != 3 + 2 * CipherSql::KeySize || !password.startsWith(QLatin1String("x'"))
        || !password.endsWith(QLatin1Char('\'')))
        return false;
    for (int i = 2; i < password.size() - 1; ++i) {
        const QChar c = password.at(i);
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')))
            return false;
    }
    return true;
}

} // namespace

namespace CipherSql {

void registerDriver()
{
    static const bool registered = [] {
        QSqlDatabase::registerSqlDriver(DriverName, new QSqlDriverCreator<CipherSqlDriver>);
        return true;
    }();
    Q_UNUSED(registered)
}

QString password(const QByteArray &key)
{
    if (key.size() != KeySize)
        return {};
    return QStringLiteral("x'") + QString::fromLatin1(key.toHex()) + QLatin1Char('\'');
}

bool isPlainSqlite(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return f.read(16) == QByteArray("SQLite format 3\0", 16);
}

bool encryptFile(const QString &file, const QByteArray &key, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const QString keyText = password(key);
    if (keyText.isEmpty())
        return fail(QStringLiteral("not a key"));
    if (!isPlainSqlite(file))
        return fail(QStringLiteral("not a plain SQLite file"));
    const QString encrypted = file + QStringLiteral(".encrypting");
    QFile::remove(encrypted);

    sqlite3 *db = nullptr;
    // CREATE: the attached, encrypted file is made under the same flags.
    if (sqlite3_open_v2(QFile::encodeName(file).constData(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
        != SQLITE_OK) {
        const QString why = sqliteError(db);
        sqlite3_close_v2(db);
        return fail(why);
    }
    QString quoted = encrypted;
    quoted.replace(QLatin1Char('\''), QStringLiteral("''"));
    const QByteArray steps[] = {
        QByteArrayLiteral("PRAGMA wal_checkpoint(TRUNCATE);"),
        QStringLiteral("ATTACH DATABASE '%1' AS encrypted KEY \"%2\";").arg(quoted, keyText).toUtf8(),
        QByteArrayLiteral("SELECT sqlcipher_export('encrypted');"),
        QByteArrayLiteral("PRAGMA encrypted.user_version = 0;"),
        QByteArrayLiteral("DETACH DATABASE encrypted;"),
    };
    for (const QByteArray &step : steps) {
        char *message = nullptr;
        if (sqlite3_exec(db, step.constData(), nullptr, nullptr, &message) != SQLITE_OK) {
            const QString why = QString::fromUtf8(message ? message : "failed");
            sqlite3_free(message);
            sqlite3_close_v2(db);
            QFile::remove(encrypted);
            return fail(why);
        }
    }
    sqlite3_close_v2(db);

    // The plain file goes, the encrypted one takes its place.
    QFile::setPermissions(encrypted, QFile::ReadOwner | QFile::WriteOwner);
    QFile::remove(file + QStringLiteral("-wal"));
    QFile::remove(file + QStringLiteral("-shm"));
    if (!QFile::remove(file) || !QFile::rename(encrypted, file))
        return fail(QStringLiteral("could not replace the file"));
    return true;
}

} // namespace CipherSql

CipherSqlDriver::CipherSqlDriver(QObject *parent)
    : QSqlDriver(parent)
{
}

CipherSqlDriver::~CipherSqlDriver()
{
    close();
}

bool CipherSqlDriver::hasFeature(DriverFeature feature) const
{
    switch (feature) {
    case Transactions:
    case PreparedQueries:
    case PositionalPlaceholders:
    case LastInsertId:
    case BLOB:
    case Unicode:
    case SimpleLocking:
        return true;
    default:
        return false;
    }
}

bool CipherSqlDriver::open(const QString &db, const QString &, const QString &password, const QString &, int,
                           const QString &)
{
    close();
    if (!password.isEmpty() && !isHexKey(password)) {
        setLastError(QSqlError(QStringLiteral("Not a key"), QString(), QSqlError::ConnectionError));
        setOpenError(true);
        return false;
    }
    const int rc = sqlite3_open_v2(QFile::encodeName(db).constData(), &m_db,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        setLastError(QSqlError(QStringLiteral("Unable to open the database"), sqliteError(m_db),
                               QSqlError::ConnectionError));
        sqlite3_close_v2(m_db);
        m_db = nullptr;
        setOpenError(true);
        return false;
    }
    sqlite3_busy_timeout(m_db, 5000);
    const QByteArray keyPragma = QStringLiteral("PRAGMA key = \"%1\";").arg(password).toUtf8();
    // With the key (or none), the file must read: a wrong key reads as rubbish.
    if ((!password.isEmpty() && !run(keyPragma.constData(), QStringLiteral("Unable to set the key")))
        || !run("SELECT count(*) FROM sqlite_master;", QStringLiteral("Wrong key, or not a database"))) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
        setOpenError(true);
        return false;
    }
    setOpen(true);
    setOpenError(false);
    return true;
}

bool CipherSqlDriver::run(const char *sql, const QString &what)
{
    char *message = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &message) == SQLITE_OK)
        return true;
    setLastError(QSqlError(what, QString::fromUtf8(message ? message : ""), QSqlError::ConnectionError));
    sqlite3_free(message);
    return false;
}

void CipherSqlDriver::close()
{
    if (!m_db)
        return;
    // Statements still alive are finalized by their results; close_v2 waits for them.
    sqlite3_close_v2(m_db);
    m_db = nullptr;
    setOpen(false);
    setOpenError(false);
}

QSqlResult *CipherSqlDriver::createResult() const
{
    return new CipherResult(this);
}

bool CipherSqlDriver::beginTransaction()
{
    return isOpen() && run("BEGIN;", QStringLiteral("Unable to begin a transaction"));
}

bool CipherSqlDriver::commitTransaction()
{
    return isOpen() && run("COMMIT;", QStringLiteral("Unable to commit"));
}

bool CipherSqlDriver::rollbackTransaction()
{
    return isOpen() && run("ROLLBACK;", QStringLiteral("Unable to roll back"));
}

QVariant CipherSqlDriver::handle() const
{
    return QVariant::fromValue(static_cast<void *>(m_db));
}
