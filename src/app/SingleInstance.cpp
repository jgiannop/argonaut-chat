#include "SingleInstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>

SingleInstance::SingleInstance(const QString &dataFile, QObject *parent)
    : QObject(parent)
    , m_lockPath(dataFile + QStringLiteral(".lock"))
{
    const QString absolute = QFileInfo(dataFile).absoluteFilePath();
    const QString name = QStringLiteral("argonaut-chat-")
        + QString::fromLatin1(QCryptographicHash::hash(absolute.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
#ifdef Q_OS_WIN
    m_name = name; // a named pipe; UserAccessOption limits it to this user
#else
    // A path in the user's own runtime folder (0700), not the shared /tmp.
    QString folder = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (folder.isEmpty())
        folder = QFileInfo(dataFile).absolutePath();
    m_name = folder + QLatin1Char('/') + name;
#endif
}

SingleInstance::~SingleInstance()
{
    m_server.close();
}

bool SingleInstance::start()
{
    QDir().mkpath(QFileInfo(m_lockPath).absolutePath());
    m_lock = std::make_unique<QLockFile>(m_lockPath);
    // Never stale by age: only when the copy holding it is gone.
    m_lock->setStaleLockTime(0);
    if (m_lock->tryLock(0)) {
        // A socket left by a crashed copy is in the way.
        QLocalServer::removeServer(m_name);
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!m_server.listen(m_name))
            qWarning("Another start could not reach this one: %s", qPrintable(m_server.errorString()));
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket *socket = m_server.nextPendingConnection()) {
                socket->disconnectFromServer();
                socket->deleteLater();
            }
            emit activated();
        });
        return true;
    }

    // Another copy has the lock. It may be starting and not listening yet:
    // a few tries, then end anyway (two copies must not share the database).
    for (int attempt = 0; attempt < 20; ++attempt) {
        QLocalSocket socket;
        socket.connectToServer(m_name);
        if (socket.waitForConnected(250)) {
            socket.write("show");
            socket.waitForBytesWritten(250);
            socket.disconnectFromServer();
            return false;
        }
        QThread::msleep(100);
    }
    qWarning("Argonaut Chat is already running, and did not answer.");
    return false;
}
