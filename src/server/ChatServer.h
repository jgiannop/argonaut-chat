#pragma once

#include "RateLimiter.h"

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QWebSocketServer>

#include <optional>

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
//
// Rate limits (Limits): connections, sign-ups and sign-in tries per address;
// sends, key fetches and prekey uploads per account; requests per
// connection. Over a limit the answer is "rate-limited" with "retryAfter"
// (ms). One-time prekeys are handed out at most so often per device, then
// bundles come without one: nobody can use up someone's stock.
//
// Keep-alive: every connection is pinged each minute (Qt answers pings by
// itself); one not heard from in two minutes is dropped, so dead connections
// (a laptop closed, a router that forgot) do not linger.
//
// Attachments: encrypted blobs (Attachment.h) uploaded in chunks over the
// same connection ("upload", "upload-chunk") and downloaded by anyone signed
// in who knows the random id ("download"). Kept 30 days, unfinished ones a
// day; 5 GB in all at most.
//
// Linked devices: a new device opens a slot ("provision-open") and shows
// the user a code with the slot and a one-time public key; a signed-in
// device of the account passes a message, sealed to that key, into the slot
// ("provision-send"); the new device then joins the account ("add-device"),
// proving it holds the account's identity key. At most MaxDevices.
//
// Behind a reverse proxy every connection comes from this computer; with
// setTrustProxy(true) the address is the last one in X-Forwarded-For, and
// only for connections from loopback (the proxy).
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

    struct Limits
    {
        int maxConnections = 10000;
        int connectionsPerAddress = 50;
        RateLimiter::Rule requestsPerConnection{200, 20};
        RateLimiter::Rule signUpsPerAddress{5, 5.0 / 3600};
        RateLimiter::Rule challengesPerAddress{30, 0.5};
        RateLimiter::Rule sendsPerAccount{120, 2};
        RateLimiter::Rule keyFetchesPerAccount{100, 100.0 / 3600};
        RateLimiter::Rule preKeyUploadsPerAccount{10, 10.0 / 3600};
        RateLimiter::Rule oneTimePreKeysPerDevice{50, 50.0 / 3600};
        // Attachments: uploads per account, and bytes up and down per account a day.
        RateLimiter::Rule attachmentUploadsPerAccount{100, 100.0 / 3600};
        RateLimiter::Rule uploadBytesPerAccount{1e9, 1e9 / 86400};
        RateLimiter::Rule downloadBytesPerAccount{5e9, 5e9 / 86400};
        qint64 maxStoredAttachmentBytes = 5LL * 1000 * 1000 * 1000;
        qint64 attachmentKeepMs = 30LL * 24 * 3600 * 1000;
        qint64 unfinishedUploadKeepMs = 24LL * 3600 * 1000;
    };
    static constexpr int MaxChunkBytes = 512 * 1024;
    // Linked devices: at most this many per account; a new device waits
    // (not signed in) at most ProvisioningTimeoutMs for its message.
    static constexpr int MaxDevices = 5;
    static constexpr int ProvisioningTimeoutMs = 10 * 60 * 1000;
    static constexpr int MaxProvisionBytes = 768 * 1024;

    explicit ChatServer(ServerStore *store, QObject *parent = nullptr);
    ~ChatServer() override;

    void setLimits(const Limits &limits) { m_limits = limits; }
    const Limits &limits() const { return m_limits; }
    // Tests: the limiter's clock (ms).
    void setClock(RateLimiter::Clock clock) { m_limiter.setClock(std::move(clock)); }
    void setTrustProxy(bool trust) { m_trustProxy = trust; }
    // Tests: the ping interval (a connection silent for two is dropped).
    void setKeepAlive(int intervalMs);

    // Port 0: any free port (see port()).
    bool listen(const QHostAddress &address = QHostAddress::LocalHost, quint16 port = 0);
    quint16 port() const;
    // A device signed in on a connection now (tests).
    bool isOnline(const QString &username, quint32 device) const { return m_online.contains(deviceKey(username, device)); }
    QString errorString() const;
    int connectionCount() const { return int(m_clients.size()); }

private:
    struct Client
    {
        QByteArray nonce;  // the challenge given, used once
        QString address;   // where it comes from (the proxy's X-Forwarded-For, if trusted)
        QString username;  // empty until signed in
        quint32 device = 0;
        int failedSignIns = 0;
        QSet<qint64> inFlight; // pushed, not yet acknowledged
        QString provisioning;  // a new device waiting to be linked: its slot
        qint64 lastHeard = 0;  // m_clock's ms
        bool signedIn() const { return !username.isEmpty(); }
    };

    void onNewConnection();
    void onText(QWebSocket *socket, const QString &text);
    void onDisconnected(QWebSocket *socket);
    void pingAll();

    QJsonObject handle(QWebSocket *socket, Client &client, const QString &op, const QJsonObject &request);
    QJsonObject challenge(Client &client);
    QJsonObject registerAccount(QWebSocket *socket, Client &client, const QJsonObject &request);
    QJsonObject login(QWebSocket *socket, Client &client, const QJsonObject &request);
    QJsonObject keys(const QJsonObject &request);
    QJsonObject send(const Client &client, const QJsonObject &request);
    QJsonObject ack(QWebSocket *socket, Client &client, const QJsonObject &request);
    QJsonObject prekeys(const Client &client, const QJsonObject &request);
    QJsonObject upload(const Client &client, const QJsonObject &request);
    QJsonObject uploadChunk(const Client &client, const QJsonObject &request);
    QJsonObject download(const Client &client, const QJsonObject &request);
    QJsonObject provisionOpen(QWebSocket *socket, Client &client);
    QJsonObject provisionSend(const QJsonObject &request);
    QJsonObject addDevice(QWebSocket *socket, Client &client, const QJsonObject &request);
    QJsonObject listDevices(const Client &client);
    QJsonObject removeDevice(const Client &client, const QJsonObject &request);

    QJsonObject signInFailed(QWebSocket *socket, Client &client, const QString &error, const QString &message);
    void signIn(QWebSocket *socket, Client &client, const QString &username, quint32 device);
    // Pushes what is queued for this device and not in flight yet.
    void deliverQueued(QWebSocket *socket);
    void deliverTo(const QString &username, quint32 device, qint64 envelopeId);

    static QString deviceKey(const QString &username, quint32 device);
    QString addressOf(QWebSocket *socket) const;
    // Takes a token; on failure, the answer to send.
    std::optional<QJsonObject> limit(const QString &key, const RateLimiter::Rule &rule, double cost = 1);

    QWebSocketServer m_server;
    ServerStore *m_store;
    QHash<QWebSocket *, Client> m_clients;
    QHash<QString, QWebSocket *> m_online; // deviceKey -> its connection
    QHash<QString, int> m_perAddress;      // open connections
    QHash<QString, QWebSocket *> m_provisioning; // slot -> the new device waiting on it
    Limits m_limits;
    RateLimiter m_limiter;
    QTimer m_prune;
    QTimer m_cleanup; // old attachments, hourly
    QTimer m_keepAlive;
    QElapsedTimer m_clock;
    bool m_trustProxy = false;
};
