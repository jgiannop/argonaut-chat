#include "ChatServer.h"

#include "Attachment.h"
#include "Crypto.h"
#include "Protocol.h"
#include "ServerStore.h"

#include <QDateTime>
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
    m_clock.start();
    m_keepAlive.setInterval(60 * 1000);
    connect(&m_keepAlive, &QTimer::timeout, this, &ChatServer::pingAll);
    m_keepAlive.start();
    m_cleanup.setInterval(3600 * 1000);
    connect(&m_cleanup, &QTimer::timeout, this, [this] {
        m_store->removeOldAttachments(QDateTime::currentMSecsSinceEpoch(), m_limits.attachmentKeepMs,
                                      m_limits.unfinishedUploadKeepMs);
    });
    m_cleanup.start();
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

void ChatServer::setKeepAlive(int intervalMs)
{
    m_keepAlive.setInterval(intervalMs);
}

void ChatServer::pingAll()
{
    const qint64 now = m_clock.elapsed();
    const QList<QWebSocket *> sockets = m_clients.keys();
    for (QWebSocket *socket : sockets) {
        if (now - m_clients.value(socket).lastHeard > 2 * qint64(m_keepAlive.interval()))
            socket->abort(); // disconnected() cleans up
        else
            socket->ping();
    }
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

std::optional<QJsonObject> ChatServer::limit(const QString &key, const RateLimiter::Rule &rule, double cost)
{
    qint64 retryAfter = 0;
    if (m_limiter.take(key, rule, &retryAfter, cost))
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
        client.lastHeard = m_clock.elapsed();
        m_clients.insert(socket, client);
        connect(socket, &QWebSocket::pong, this, [this, socket] {
            if (m_clients.contains(socket))
                m_clients[socket].lastHeard = m_clock.elapsed();
        });
        ++m_perAddress[address];
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString &text) { onText(socket, text); });
        connect(socket, &QWebSocket::binaryMessageReceived, socket,
                [socket] { socket->close(QWebSocketProtocol::CloseCodeDatatypeNotSupported); });
        connect(socket, &QWebSocket::disconnected, this, [this, socket] { onDisconnected(socket); });
        // Sign in soon, or go.
        QTimer::singleShot(SignInTimeoutMs, socket, [this, socket] {
            // (A new device waiting to be linked has longer: provisionOpen.)
            if (m_clients.contains(socket) && !m_clients[socket].signedIn() && m_clients[socket].provisioning.isEmpty())
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
        if (!it->provisioning.isEmpty() && m_provisioning.value(it->provisioning) == socket)
            m_provisioning.remove(it->provisioning);
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
    m_clients[socket].lastHeard = m_clock.elapsed();
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
        && (op == QLatin1String("login") || op == QLatin1String("register") || op == QLatin1String("ack")
            || op == QLatin1String("add-device")))
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
    if (op == QLatin1String("provision-open")) {
        if (auto limited = limit(QStringLiteral("challenge:") + client.address, m_limits.challengesPerAddress))
            return *limited;
        return provisionOpen(socket, client);
    }
    if (op == QLatin1String("add-device")) {
        if (!client.signedIn()) {
            if (auto limited = limit(QStringLiteral("signup:") + client.address, m_limits.signUpsPerAddress))
                return *limited;
        }
        return addDevice(socket, client, request);
    }
    if (!client.signedIn()) {
        if (op == QLatin1String("keys") || op == QLatin1String("send") || op == QLatin1String("ack")
            || op == QLatin1String("prekeys") || op == QLatin1String("upload") || op == QLatin1String("upload-chunk")
            || op == QLatin1String("download") || op == QLatin1String("provision-send")
            || op == QLatin1String("devices") || op == QLatin1String("remove-device"))
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
    if (op == QLatin1String("upload")) {
        if (auto limited = limit(QStringLiteral("uploads:") + client.username, m_limits.attachmentUploadsPerAccount))
            return *limited;
        return upload(client, request);
    }
    if (op == QLatin1String("upload-chunk"))
        return uploadChunk(client, request);
    if (op == QLatin1String("download"))
        return download(client, request);
    if (op == QLatin1String("provision-send"))
        return provisionSend(request);
    if (op == QLatin1String("devices"))
        return listDevices(client);
    if (op == QLatin1String("remove-device"))
        return removeDevice(client, request);
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
        || !Protocol::verifyPreKeys(*identityKey, signedPreKey, *oneTimePreKeys))
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
    // "online": only to the devices connected now, kept no longer than until
    // they acknowledge it (typing indicators: worthless later). Others get nothing.
    if (request.value(QStringLiteral("online")) == QJsonValue(true)) {
        devices.erase(std::remove_if(devices.begin(), devices.end(),
                                     [&](quint32 device) { return !m_online.contains(deviceKey(to, device)); }),
                      devices.end());
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
    }
    const auto oneTime =
        Protocol::oneTimePreKeysFromJson(oneTimeValue.isUndefined() ? QJsonValue(QJsonArray()) : oneTimeValue);
    if (!oneTime)
        return badRequest(QStringLiteral("Malformed one-time prekeys."));
    // Every signature (the KEM prekeys' too) by this device's identity key.
    if (!Protocol::verifyPreKeys(m_store->identityKey(client.username, client.device), signedPreKey, *oneTime))
        return fail(QStringLiteral("denied"), QStringLiteral("The signature does not match."));
    if (signedPreKey)
        m_store->setSignedPreKey(client.username, client.device, *signedPreKey);
    if (!oneTime->isEmpty())
        m_store->addOneTimePreKeys(client.username, client.device, *oneTime);
    return ok({{QStringLiteral("prekeys"), m_store->oneTimePreKeyCount(client.username, client.device)}});
}

QJsonObject ChatServer::upload(const Client &client, const QJsonObject &request)
{
    const auto size = Protocol::Json::integer(request.value(QStringLiteral("size")), 1,
                                              Attachment::blobSize(Attachment::MaxFileBytes));
    if (!size)
        return badRequest(QStringLiteral("No size, or too large."));
    if (m_store->storedAttachmentBytes() + *size > m_limits.maxStoredAttachmentBytes)
        return fail(QStringLiteral("full"), QStringLiteral("The server has no room for attachments now."));
    if (auto limited = limit(QStringLiteral("upbytes:") + client.username, m_limits.uploadBytesPerAccount, double(*size)))
        return *limited;
    const QString id = m_store->createAttachment(client.username, *size);
    if (id.isEmpty())
        return fail(QStringLiteral("server"), QStringLiteral("Could not keep the attachment."));
    return ok({{QStringLiteral("id"), id}, {QStringLiteral("chunk"), MaxChunkBytes}});
}

QJsonObject ChatServer::uploadChunk(const Client &client, const QJsonObject &request)
{
    const QString id = request.value(QStringLiteral("id")).toString();
    const auto offset = Protocol::Json::integer(request.value(QStringLiteral("offset")), 0,
                                                Attachment::blobSize(Attachment::MaxFileBytes));
    const auto data = Protocol::Json::bytes(request.value(QStringLiteral("data")), -1, MaxChunkBytes);
    if (!Attachment::isValidId(id) || !offset || !data)
        return badRequest(QStringLiteral("Malformed chunk."));
    const auto info = m_store->attachment(id);
    if (!info || info->owner != client.username)
        return fail(QStringLiteral("not-found"), QStringLiteral("No such upload."));
    if (!m_store->appendAttachment(id, client.username, *offset, *data))
        return fail(QStringLiteral("bad-offset"), QStringLiteral("Not where the upload is."),
                    {{QStringLiteral("received"), double(info->received)}});
    return ok({{QStringLiteral("received"), double(*offset + data->size())}});
}

QJsonObject ChatServer::download(const Client &client, const QJsonObject &request)
{
    const QString id = request.value(QStringLiteral("id")).toString();
    const auto offset = Protocol::Json::integer(request.value(QStringLiteral("offset")), 0,
                                                Attachment::blobSize(Attachment::MaxFileBytes));
    const auto length = Protocol::Json::integer(request.value(QStringLiteral("length")), 1, MaxChunkBytes);
    if (!Attachment::isValidId(id) || !offset || !length)
        return badRequest(QStringLiteral("Malformed download."));
    const auto info = m_store->attachment(id);
    if (!info || !info->complete())
        return fail(QStringLiteral("not-found"), QStringLiteral("No such attachment (it may have expired)."));
    if (*offset >= info->size)
        return badRequest(QStringLiteral("Past the end."));
    const qint64 count = qMin<qint64>(*length, info->size - *offset);
    if (auto limited = limit(QStringLiteral("downbytes:") + client.username, m_limits.downloadBytesPerAccount,
                             double(count)))
        return *limited;
    const QByteArray data = m_store->readAttachment(id, *offset, count);
    if (data.isEmpty())
        return fail(QStringLiteral("server"), QStringLiteral("Could not read the attachment."));
    return ok({{QStringLiteral("data"), Protocol::Json::base64(data)}, {QStringLiteral("size"), double(info->size)}});
}

QJsonObject ChatServer::provisionOpen(QWebSocket *socket, Client &client)
{
    if (client.signedIn())
        return fail(QStringLiteral("signed-in"), QStringLiteral("Already signed in."));
    if (client.provisioning.isEmpty()) {
        client.provisioning = QString::fromLatin1(Crypto::randomBytes(16).toHex());
        m_provisioning.insert(client.provisioning, socket);
        // Linked within ten minutes, or gone.
        QTimer::singleShot(ProvisioningTimeoutMs, socket, [this, socket] {
            if (m_clients.contains(socket) && !m_clients[socket].signedIn())
                socket->close(QWebSocketProtocol::CloseCodePolicyViolated);
        });
    }
    return ok({{QStringLiteral("id"), client.provisioning}});
}

QJsonObject ChatServer::provisionSend(const QJsonObject &request)
{
    const QString id = request.value(QStringLiteral("id")).toString();
    const auto message = Protocol::Json::bytes(request.value(QStringLiteral("message")), -1, MaxProvisionBytes);
    if (!Attachment::isValidId(id) || !message)
        return badRequest(QStringLiteral("Malformed provisioning message."));
    // One message per slot.
    QWebSocket *target = m_provisioning.take(id);
    if (!target || !m_clients.contains(target))
        return fail(QStringLiteral("not-found"), QStringLiteral("No device is waiting with that code."));
    sendJson(target, {{QStringLiteral("op"), QStringLiteral("provision")},
                      {QStringLiteral("message"), Protocol::Json::base64(*message)}});
    return ok();
}

QJsonObject ChatServer::addDevice(QWebSocket *socket, Client &client, const QJsonObject &request)
{
    if (client.signedIn())
        return fail(QStringLiteral("signed-in"), QStringLiteral("Already signed in."));
    const QByteArray nonce = client.nonce;
    client.nonce.clear();
    if (nonce.isEmpty())
        return signInFailed(socket, client, QStringLiteral("no-challenge"), QStringLiteral("Ask for a challenge first."));
    const QString username = request.value(QStringLiteral("username")).toString();
    const auto identityKey = Protocol::Json::bytes(request.value(QStringLiteral("identityKey")), Crypto::KeySize);
    const auto signedPreKey = Protocol::signedPreKeyFromJson(request.value(QStringLiteral("signedPreKey")));
    const auto oneTimePreKeys = Protocol::oneTimePreKeysFromJson(
        request.value(QStringLiteral("oneTimePreKeys")).isUndefined() ? QJsonValue(QJsonArray())
                                                                      : request.value(QStringLiteral("oneTimePreKeys")));
    const auto signature = Protocol::Json::bytes(request.value(QStringLiteral("signature")), Crypto::SignatureSize);
    if (!Protocol::isValidUsername(username) || !identityKey || !signedPreKey || !oneTimePreKeys || !signature)
        return signInFailed(socket, client, QStringLiteral("bad-request"), QStringLiteral("Malformed request."));
    // The account's identity key, and proof of holding it (device 0: a device to be).
    const QList<quint32> devices = m_store->devices(username);
    if (devices.isEmpty() || m_store->identityKey(username, devices.first()) != *identityKey
        || !Crypto::verify(*identityKey, Protocol::authMessage(nonce, username, 0), *signature)
        || !Protocol::verifyPreKeys(*identityKey, signedPreKey, *oneTimePreKeys))
        return signInFailed(socket, client, QStringLiteral("denied"), QStringLiteral("Linking refused."));
    if (devices.size() >= MaxDevices)
        return fail(QStringLiteral("too-many-devices"),
                    QStringLiteral("This account has %1 devices already.").arg(MaxDevices));
    const quint32 device = m_store->addDevice(username, *identityKey, *signedPreKey);
    if (device == 0)
        return fail(QStringLiteral("server"), QStringLiteral("Could not add the device."));
    m_store->addOneTimePreKeys(username, device, *oneTimePreKeys);
    if (!client.provisioning.isEmpty()) {
        m_provisioning.remove(client.provisioning);
        client.provisioning.clear();
    }
    signIn(socket, client, username, device);
    return ok({{QStringLiteral("device"), double(device)}});
}

QJsonObject ChatServer::listDevices(const Client &client)
{
    QJsonArray list;
    for (quint32 device : m_store->devices(client.username))
        list.append(QJsonObject{{QStringLiteral("device"), double(device)},
                                {QStringLiteral("created"), double(m_store->deviceCreated(client.username, device))}});
    return ok({{QStringLiteral("devices"), list}});
}

QJsonObject ChatServer::removeDevice(const Client &client, const QJsonObject &request)
{
    const auto device = Protocol::Json::integer(request.value(QStringLiteral("device")), 1,
                                                std::numeric_limits<quint32>::max());
    if (!device)
        return badRequest(QStringLiteral("No device."));
    // The first device removes the others; any device removes itself; the first stays.
    if (*device == 1 || (client.device != 1 && quint32(*device) != client.device))
        return fail(QStringLiteral("denied"), QStringLiteral("Only your first device can remove the others."));
    if (!m_store->removeDevice(client.username, quint32(*device)))
        return fail(QStringLiteral("not-found"), QStringLiteral("No such device."));
    // Its connection, if any: no longer signed in, and gone (taken out of
    // m_online here, as onDisconnected will not know whose it was).
    if (QWebSocket *online = m_online.take(deviceKey(client.username, quint32(*device)))) {
        if (m_clients.contains(online))
            m_clients[online].username.clear();
        QTimer::singleShot(0, online, [online] { online->close(QWebSocketProtocol::CloseCodeNormal); });
    }
    return ok();
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
