#pragma once

#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QWebSocketServer>

class QWebSocket;
class ServerStore;

// The relay (docs/protocol.md, "The server's requests"): accounts, public
// keys, and a queue of encrypted messages until each device has them. Plain
// WebSockets and JSON; TLS is the reverse proxy's job, so by default it only
// listens on this computer.
//
// Every connection is a stranger until it signs in by signing a nonce with
// its identity key (no passwords). Every request is checked; a connection
// that sends something that is not a request is closed.
class ChatServer : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 MaxRequestBytes = 4 * 1024 * 1024;
    static constexpr int MaxDevicesPerSend = 16;
    static constexpr int MaxAckIds = 1000;
    static constexpr int MaxFailedSignIns = 5;  // then the connection is closed
    static constexpr int SignInTimeoutMs = 30000;
    static constexpr int DeliverBatch = 500;    // envelopes in flight per device

    explicit ChatServer(ServerStore *store, QObject *parent = nullptr);
    ~ChatServer() override;

    // Port 0: any free port (see port()).
    bool listen(const QHostAddress &address = QHostAddress::LocalHost, quint16 port = 0);
    quint16 port() const;
    QString errorString() const;
    int connectionCount() const { return int(m_clients.size()); }

private:
    struct Client
    {
        QByteArray nonce;  // the challenge given, used once
        QString username;  // empty until signed in
        quint32 device = 0;
        int failedSignIns = 0;
        QSet<qint64> inFlight; // pushed, not yet acknowledged
        bool signedIn() const { return !username.isEmpty(); }
    };

    void onNewConnection();
    void onText(QWebSocket *socket, const QString &text);
    void onDisconnected(QWebSocket *socket);

    QJsonObject handle(QWebSocket *socket, Client &client, const QString &op, const QJsonObject &request);
    QJsonObject challenge(Client &client);
    QJsonObject registerAccount(QWebSocket *socket, Client &client, const QJsonObject &request);
    QJsonObject login(QWebSocket *socket, Client &client, const QJsonObject &request);
    QJsonObject keys(const QJsonObject &request);
    QJsonObject send(const Client &client, const QJsonObject &request);
    QJsonObject ack(QWebSocket *socket, Client &client, const QJsonObject &request);
    QJsonObject prekeys(const Client &client, const QJsonObject &request);

    QJsonObject signInFailed(QWebSocket *socket, Client &client, const QString &error, const QString &message);
    void signIn(QWebSocket *socket, Client &client, const QString &username, quint32 device);
    // Pushes what is queued for this device and not in flight yet.
    void deliverQueued(QWebSocket *socket);
    void deliverTo(const QString &username, quint32 device, qint64 envelopeId);

    static QString deviceKey(const QString &username, quint32 device);

    QWebSocketServer m_server;
    ServerStore *m_store;
    QHash<QWebSocket *, Client> m_clients;
    QHash<QString, QWebSocket *> m_online; // deviceKey -> its connection
};
