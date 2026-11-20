#include "ChatServer.h"

#include "Crypto.h"
#include "Protocol.h"
#include "ServerStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QWebSocket>

#include <limits>

namespace {

QJsonObject ok(QJsonObject fields = {})
{
    fields.insert(QStringLiteral("ok"), true);
    return fields;
}

QJsonObject fail(const QString &error, const QString &message, QJsonObject fields = {})
{
    fields.insert(QStringLiteral("ok"), false);
    fields.insert(QStringLiteral("error"), error);
    fields.insert(QStringLiteral("message"), message);
    return fields;
}

QJsonObject badRequest(const QString &message)
{
    return fail(QStringLiteral("bad-request"), message);
}

void sendJson(QWebSocket *socket, const QJsonObject &json)
{
    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

} // namespace

ChatServer::ChatServer(ServerStore *store, QObject *parent)
    : QObject(parent)
    , m_server(QStringLiteral("argonaut-chat-server"), QWebSocketServer::NonSecureMode)
    , m_store(store)
{
    connect(&m_server, &QWebSocketServer::newConnection, this, &ChatServer::onNewConnection);
    m_prune.setInterval(5 * 60 * 1000);
    connect(&m_prune, &QTimer::timeout, this, [this] { m_limiter.prune(); });
    m_prune.start();
}

ChatServer::~ChatServer()
{
    m_server.close();
    const QList<QWebSocket *> sockets = m_clients.keys();
    m_clients.clear();
    m_online.clear();
    for (QWebSocket *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        delete socket;
    }
}

bool ChatServer::listen(const QHostAddress &address, quint16 port)
{
    return m_server.listen(address, port);
}

quint16 ChatServer::port() const
{
    return m_server.serverPort();
}

QString ChatServer::errorString() const
{
    return m_server.errorString();
}

QString ChatServer::deviceKey(const QString &username, quint32 device)
{
    return username + QLatin1Char('/') + QString::number(device);
}

QString ChatServer::addressOf(QWebSocket *socket) const
{
    const QHostAddress peer = socket->peerAddress();
    if (m_trustProxy && peer.isLoopback()) {
        // The proxy appends the address it saw: the last one is the one to trust.
        const QByteArray header = socket->request().rawHeader("X-Forwarded-For");
        const QHostAddress forwarded(QString::fromLatin1(header.split(',').last().trimmed()));
        if (!forwarded.isNull())
            return forwarded.toString();
    }
    return peer.toString();
}

std::optional<QJsonObject> ChatServer::limit(const QString &key, const RateLimiter::Rule &rule)
{
    qint64 retryAfter = 0;
    if (m_limiter.take(key, rule, &retryAfter))
        return std::nullopt;
    return fail(QStringLiteral("rate-limited"), QStringLiteral("Too many requests; try again later."),
                {{QStringLiteral("retryAfter"), double(qMin<qint64>(retryAfter, qint64(1) << 40))}});
}

void ChatServer::onNewConnection()
{
    while (QWebSocket *socket = m_server.nextPendingConnection()) {
        socket->setParent(nullptr);
        const QString address = addressOf(socket);
        if (m_clients.size() >= m_limits.maxConnections
            || m_perAddress.value(address) >= m_limits.connectionsPerAddress) {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("Too many connections."));
            socket->deleteLater();
            continue;
        }
        socket->setMaxAllowedIncomingMessageSize(quint64(MaxRequestBytes));
        socket->setMaxAllowedIncomingFrameSize(quint64(MaxRequestBytes));
        Client client;
        client.address = address;
        m_clients.insert(socket, client);
        ++m_perAddress[address];
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString &text) { onText(socket, text); });
        connect(socket, &QWebSocket::binaryMessageReceived, socket,
                [socket] { socket->close(QWebSocketProtocol::CloseCodeDatatypeNotSupported); });
        connect(socket, &QWebSocket::disconnected, this, [this, socket] { onDisconnected(socket); });
        // Sign in soon, or go.
        QTimer::singleShot(SignInTimeoutMs, socket, [this, socket] {
            if (m_clients.contains(socket) && !m_clients[socket].signedIn())
                socket->close(QWebSocketProtocol::CloseCodePolicyViolated);
        });
    }
}

void ChatServer::onDisconnected(QWebSocket *socket)
{
    const auto it = m_clients.constFind(socket);
    if (it != m_clients.constEnd()) {
        const QString key = deviceKey(it->username, it->device);
        if (it->signedIn() && m_online.value(key) == socket)
            m_online.remove(key);
        if (--m_perAddress[it->address] <= 0)
            m_perAddress.remove(it->address);
        m_clients.erase(it);
    }
    socket->deleteLater();
}

void ChatServer::onText(QWebSocket *socket, const QString &text)
{
    if (!m_clients.contains(socket))
        return;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
    const QJsonObject request = document.object();
    const auto rid = Protocol::Json::integer(request.value(QStringLiteral("rid")), 0, (qint64(1) << 53));
    const QJsonValue op = request.value(QStringLiteral("op"));
    if (error.error != QJsonParseError::NoError || !document.isObject() || !rid || !op.isString()) {
        socket->close(QWebSocketProtocol::CloseCodeProtocolError);
        return;
    }
    QJsonObject response = handle(socket, m_clients[socket], op.toString(), request);
    response.insert(QStringLiteral("rid"), double(*rid));
    if (socket->state() != QAbstractSocket::ConnectedState)
        return;
    sendJson(socket, response);
    // After signing in or acknowledging: what is waiting.
    if (m_clients.contains(socket) && m_clients[socket].signedIn() && response.value(QStringLiteral("ok")).toBool()
        && (op == QLatin1String("login") || op == QLatin1String("register") || op == QLatin1String("ack")))
        deliverQueued(socket);
}

QJsonObject ChatServer::handle(QWebSocket *socket, Client &client, const QString &op, const QJsonObject &request)
{
    if (auto limited = limit(QStringLiteral("connection:%1").arg(quintptr(socket)), m_limits.requestsPerConnection))
        return *limited;
    if (op == QLatin1String("challenge")) {
        if (auto limited = limit(QStringLiteral("challenge:") + client.address, m_limits.challengesPerAddress))
            return *limited;
        return challenge(client);
    }
    if (op == QLatin1String("register")) {
        if (!client.signedIn()) {
            if (auto limited = limit(QStringLiteral("signup:") + client.address, m_limits.signUpsPerAddress))
                return *limited;
        }
        return registerAccount(socket, client, request);
    }
    if (op == QLatin1String("login"))
        return login(socket, client, request);
    if (!client.signedIn()) {
        if (op == QLatin1String("keys") || op == QLatin1String("send") || op == QLatin1String("ack")
            || op == QLatin1String("prekeys"))
            return fail(QStringLiteral("unauthenticated"), QStringLiteral("Sign in first."));
        return fail(QStringLiteral("unknown-op"), QStringLiteral("No such request."));
    }
    if (op == QLatin1String("keys")) {
        if (auto limited = limit(QStringLiteral("keys:") + client.username, m_limits.keyFetchesPerAccount))
            return *limited;
        return keys(request);
    }
    if (op == QLatin1String("send")) {
        if (auto limited = limit(QStringLiteral("send:") + client.username, m_limits.sendsPerAccount))
            return *limited;
        return send(client, request);
    }
    if (op == QLatin1String("ack"))
        return ack(socket, client, request);
    if (op == QLatin1String("prekeys")) {
        if (auto limited = limit(QStringLiteral("prekeys:") + client.username, m_limits.preKeyUploadsPerAccount))
            return *limited;
        return prekeys(client, request);
    }
    return fail(QStringLiteral("unknown-op"), QStringLiteral("No such request."));
}

QJsonObject ChatServer::challenge(Client &client)
{
    client.nonce = Crypto::randomBytes(Protocol::AuthNonceSize);
    return ok({{QStringLiteral("nonce"), Protocol::Json::base64(client.nonce)}});
}

QJsonObject ChatServer::signInFailed(QWebSocket *socket, Client &client, const QString &error,
                                     const QString &message)
{
    if (++client.failedSignIns >= MaxFailedSignIns)
        QTimer::singleShot(0, socket, [socket] { socket->close(QWebSocketProtocol::CloseCodePolicyViolated); });
    return fail(error, message);
}

QJsonObject ChatServer::registerAccount(QWebSocket *socket, Client &client, const QJsonObject &request)
{
    if (client.signedIn())
        return fail(QStringLiteral("signed-in"), QStringLiteral("Already signed in."));
    const QByteArray nonce = client.nonce;
    client.nonce.clear(); // one try per challenge
    if (nonce.isEmpty())
        return signInFailed(socket, client, QStringLiteral("no-challenge"), QStringLiteral("Ask for a challenge first."));

    const QString username = request.value(QStringLiteral("username")).toString();
    const auto identityKey = Protocol::Json::bytes(request.value(QStringLiteral("identityKey")), Crypto::KeySize);
    const auto signedPreKey = Protocol::signedPreKeyFromJson(request.value(QStringLiteral("signedPreKey")));
    const auto oneTimePreKeys = Protocol::oneTimePreKeysFromJson(
        request.value(QStringLiteral("oneTimePreKeys")).isUndefined() ? QJsonValue(QJsonArray())
                                                                      : request.value(QStringLiteral("oneTimePreKeys")));
    const auto signature = Protocol::Json::bytes(request.value(QStringLiteral("signature")), Crypto::SignatureSize);
    if (!Protocol::isValidUsername(username))
        return signInFailed(socket, client, QStringLiteral("bad-username"), QStringLiteral("Not a valid username."));
    if (!identityKey || Crypto::dhPublicKeyOf(*identityKey).isEmpty() || !signedPreKey || !oneTimePreKeys
        || !signature)
        return signInFailed(socket, client, QStringLiteral("bad-request"), QStringLiteral("Malformed keys."));
    if (!Crypto::verify(*identityKey, Protocol::authMessage(nonce, username, 1), *signature)
        || !Crypto::verify(*identityKey, signedPreKey->key, signedPreKey->signature))
        return signInFailed(socket, client, QStringLiteral("denied"), QStringLiteral("A signature does not match."));
    if (!m_store->createAccount(username, *identityKey, *signedPreKey))
        return fail(QStringLiteral("taken"), QStringLiteral("That username is taken."));
    m_store->addOneTimePreKeys(username, 1, *oneTimePreKeys);
    signIn(socket, client, username, 1);
    return ok({{QStringLiteral("device"), 1}});
}

QJsonObject ChatServer::login(QWebSocket *socket, Client &client, const QJsonObject &request)
{
    if (client.signedIn())
        return fail(QStringLiteral("signed-in"), QStringLiteral("Already signed in."));
    const QByteArray nonce = client.nonce;
    client.nonce.clear();
    if (nonce.isEmpty())
        return signInFailed(socket, client, QStringLiteral("no-challenge"), QStringLiteral("Ask for a challenge first."));

    const QString username = request.value(QStringLiteral("username")).toString();
    const auto device = Protocol::Json::integer(request.value(QStringLiteral("device")), 1,
                                                std::numeric_limits<quint32>::max());
    const auto signature = Protocol::Json::bytes(request.value(QStringLiteral("signature")), Crypto::SignatureSize);
    if (!Protocol::isValidUsername(username) || !device || !signature)
        return signInFailed(socket, client, QStringLiteral("bad-request"), QStringLiteral("Malformed sign-in."));
    // The same answer for an unknown account and a wrong key.
    const QByteArray identityKey = m_store->identityKey(username, quint32(*device));
    if (identityKey.isEmpty()
        || !Crypto::verify(identityKey, Protocol::authMessage(nonce, username, quint32(*device)), *signature))
        return signInFailed(socket, client, QStringLiteral("denied"), QStringLiteral("Sign-in refused."));
    signIn(socket, client, username, quint32(*device));
    return ok({{QStringLiteral("prekeys"), m_store->oneTimePreKeyCount(username, quint32(*device))}});
}

void ChatServer::signIn(QWebSocket *socket, Client &client, const QString &username, quint32 device)
{
    client.username = username;
    client.device = device;
    client.failedSignIns = 0;
    // The same device signed in elsewhere: that connection goes.
    const QString key = deviceKey(username, device);
    QWebSocket *previous = m_online.value(key);
    m_online.insert(key, socket);
    if (previous && previous != socket) {
        if (m_clients.contains(previous))
            m_clients[previous].username.clear();
        previous->close(QWebSocketProtocol::CloseCodeNormal, QStringLiteral("Signed in elsewhere."));
    }
}

QJsonObject ChatServer::keys(const QJsonObject &request)
{
    const QString username = request.value(QStringLiteral("username")).toString();
    if (!Protocol::isValidUsername(username))
        return badRequest(QStringLiteral("Not a valid username."));
    const QList<quint32> devices = m_store->devices(username);
    if (devices.isEmpty())
        return fail(QStringLiteral("not-found"), QStringLiteral("No such user."));
    QJsonArray out;
    for (quint32 device : devices) {
        // A one-time prekey only so often per device; then without.
        const bool oneTime = m_limiter.take(QStringLiteral("onetime:") + deviceKey(username, device),
                                            m_limits.oneTimePreKeysPerDevice);
        const auto bundle = m_store->takeBundle(username, device, oneTime);
        if (bundle)
            out.append(QJsonObject{{QStringLiteral("device"), double(device)},
                                   {QStringLiteral("bundle"), Protocol::bundleToJson(*bundle)}});
    }
    return ok({{QStringLiteral("devices"), out}});
}

QJsonObject ChatServer::send(const Client &client, const QJsonObject &request)
{
    const QString to = request.value(QStringLiteral("to")).toString();
    const QJsonValue messages = request.value(QStringLiteral("messages"));
    if (!Protocol::isValidUsername(to))
        return badRequest(QStringLiteral("Not a valid username."));
    if (!messages.isArray() || messages.toArray().isEmpty() || messages.toArray().size() > MaxDevicesPerSend)
        return badRequest(QStringLiteral("No messages."));

    QHash<quint32, QByteArray> byDevice;
    for (const QJsonValue &item : messages.toArray()) {
        const auto device = Protocol::Json::integer(item.toObject().value(QStringLiteral("device")), 1,
                                                    std::numeric_limits<quint32>::max());
        const auto message = Protocol::Json::bytes(item.toObject().value(QStringLiteral("message")), -1,
                                                   Protocol::MaxCipherMessageBytes);
        if (!device || !message || byDevice.contains(quint32(*device))
            || !Protocol::decodeCipherMessage(*message))
            return badRequest(QStringLiteral("Malformed message."));
        byDevice.insert(quint32(*device), *message);
    }

    // One message for every device of theirs (but not for the sending device itself).
    QList<quint32> devices = m_store->devices(to);
    if (devices.isEmpty())
        return fail(QStringLiteral("not-found"), QStringLiteral("No such user."));
    if (to == client.username)
        devices.removeAll(client.device);
    QList<quint32> given = byDevice.keys();
    std::sort(given.begin(), given.end());
    if (given != devices) {
        QJsonArray expected;
        for (quint32 device : devices)
            expected.append(double(device));
        return fail(QStringLiteral("devices"), QStringLiteral("Their devices have changed."),
                    {{QStringLiteral("devices"), expected}});
    }
    for (quint32 device : devices) {
        if (m_store->queuedCount(to, device) >= ServerStore::MaxQueuedMessages)
            return fail(QStringLiteral("full"), QStringLiteral("Their queue is full."));
    }
    for (quint32 device : devices) {
        const qint64 id = m_store->queue(to, device, client.username, client.device, byDevice.value(device));
        if (id == 0)
            return fail(QStringLiteral("server"), QStringLiteral("Could not keep the message."));
        deliverTo(to, device, id);
    }
    return ok();
}

QJsonObject ChatServer::ack(QWebSocket *socket, Client &client, const QJsonObject &request)
{
    Q_UNUSED(socket)
    const QJsonValue ids = request.value(QStringLiteral("ids"));
    if (!ids.isArray() || ids.toArray().size() > MaxAckIds)
        return badRequest(QStringLiteral("No ids."));
    QList<qint64> list;
    for (const QJsonValue &id : ids.toArray()) {
        bool isNumber = false;
        const qint64 n = id.toString().toLongLong(&isNumber);
        if (!id.isString() || !isNumber || n <= 0)
            return badRequest(QStringLiteral("Malformed id."));
        list.append(n);
    }
    m_store->remove(client.username, client.device, list);
    for (qint64 id : list)
        client.inFlight.remove(id);
    return ok();
}

QJsonObject ChatServer::prekeys(const Client &client, const QJsonObject &request)
{
    const QJsonValue signedValue = request.value(QStringLiteral("signedPreKey"));
    const QJsonValue oneTimeValue = request.value(QStringLiteral("oneTimePreKeys"));
    std::optional<Protocol::PublicSignedPreKey> signedPreKey;
    if (!signedValue.isUndefined()) {
        signedPreKey = Protocol::signedPreKeyFromJson(signedValue);
        if (!signedPreKey)
            return badRequest(QStringLiteral("Malformed signed prekey."));
        const QByteArray identityKey = m_store->identityKey(client.username, client.device);
        if (!Crypto::verify(identityKey, signedPreKey->key, signedPreKey->signature))
            return fail(QStringLiteral("denied"), QStringLiteral("The signature does not match."));
    }
    const auto oneTime =
        Protocol::oneTimePreKeysFromJson(oneTimeValue.isUndefined() ? QJsonValue(QJsonArray()) : oneTimeValue);
    if (!oneTime)
        return badRequest(QStringLiteral("Malformed one-time prekeys."));
    if (signedPreKey)
        m_store->setSignedPreKey(client.username, client.device, *signedPreKey);
    if (!oneTime->isEmpty())
        m_store->addOneTimePreKeys(client.username, client.device, *oneTime);
    return ok({{QStringLiteral("prekeys"), m_store->oneTimePreKeyCount(client.username, client.device)}});
}

void ChatServer::deliverQueued(QWebSocket *socket)
{
    Client &client = m_clients[socket];
    if (client.inFlight.size() >= DeliverBatch)
        return;
    const QList<Protocol::Envelope> envelopes =
        m_store->queued(client.username, client.device, DeliverBatch + int(client.inFlight.size()));
    for (const Protocol::Envelope &envelope : envelopes) {
        if (client.inFlight.size() >= DeliverBatch)
            break;
        const qint64 id = envelope.id.toLongLong();
        if (client.inFlight.contains(id))
            continue;
        client.inFlight.insert(id);
        sendJson(socket, {{QStringLiteral("op"), QStringLiteral("deliver")},
                          {QStringLiteral("envelope"), Protocol::envelopeToJson(envelope)}});
    }
}

void ChatServer::deliverTo(const QString &username, quint32 device, qint64 envelopeId)
{
    QWebSocket *socket = m_online.value(deviceKey(username, device));
    if (!socket || !m_clients.contains(socket))
        return;
    Client &client = m_clients[socket];
    if (client.inFlight.size() >= DeliverBatch) // the rest after the acknowledgements
        return;
    const auto envelope = m_store->envelope(envelopeId);
    if (!envelope)
        return;
    client.inFlight.insert(envelopeId);
    sendJson(socket, {{QStringLiteral("op"), QStringLiteral("deliver")},
                      {QStringLiteral("envelope"), Protocol::envelopeToJson(*envelope)}});
}
