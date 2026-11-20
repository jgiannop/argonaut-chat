#include "ChatClient.h"

#include "Attachment.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSslError>

#include <limits>
#include <memory>
#include <utility>

namespace {

constexpr qint64 kMaxIncomingBytes = 4 * 1024 * 1024;

ChatClient::Result failure(const QString &error, const QString &message)
{
    ChatClient::Result result;
    result.error = error;
    result.message = message;
    return result;
}

bool isThisComputer(const QString &host)
{
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
        return true;
    const QHostAddress address(host);
    return !address.isNull() && address.isLoopback();
}

} // namespace

ChatClient::ChatClient(QObject *parent)
    : QObject(parent)
{
    m_socket.setMaxAllowedIncomingMessageSize(quint64(kMaxIncomingBytes));
    m_socket.setMaxAllowedIncomingFrameSize(quint64(kMaxIncomingBytes));
    m_clock.start();
    m_keepAlive.setInterval(30000);
    connect(&m_keepAlive, &QTimer::timeout, this, &ChatClient::checkAlive);
    connect(&m_socket, &QWebSocket::pong, this, &ChatClient::heard);
    connect(&m_socket, &QWebSocket::connected, this, [this] {
        heard();
        m_keepAlive.start();
    });
    connect(&m_socket, &QWebSocket::connected, this, &ChatClient::connected);
    connect(&m_socket, &QWebSocket::disconnected, this, &ChatClient::onDisconnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &ChatClient::onText);
    connect(&m_socket, &QWebSocket::binaryMessageReceived, this, [this] {
        m_socket.close(QWebSocketProtocol::CloseCodeDatatypeNotSupported);
    });
    // Never ignored: the socket gives up, we only say why.
    connect(&m_socket, &QWebSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
        QStringList why;
        for (const QSslError &error : errors)
            why << error.errorString();
        emit failed(tr("The server's certificate is not trusted: %1").arg(why.join(QStringLiteral("; "))));
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(&m_socket, &QWebSocket::errorOccurred, this, [this] { emit failed(m_socket.errorString()); });
#else
    connect(&m_socket, qOverload<QAbstractSocket::SocketError>(&QWebSocket::error), this, [this] { emit failed(m_socket.errorString()); });
#endif
}

ChatClient::~ChatClient()
{
    m_socket.disconnect(this);
    m_socket.abort();
}

bool ChatClient::open(const QUrl &url, QString *error)
{
    const QString scheme = url.scheme();
    const bool allowed = url.isValid() && !url.host().isEmpty()
                         && (scheme == QLatin1String("wss") || (scheme == QLatin1String("ws") && isThisComputer(url.host())));
    if (!allowed) {
        if (error)
            *error = tr("Only wss:// addresses (ws:// only to this computer).");
        return false;
    }
    m_socket.open(url);
    return true;
}

void ChatClient::setKeepAlive(int intervalMs, int timeoutMs)
{
    m_keepAlive.setInterval(intervalMs);
    m_timeoutMs = timeoutMs;
}

void ChatClient::heard()
{
    m_lastHeard = m_clock.elapsed();
}

void ChatClient::checkAlive()
{
    if (!isConnected()) {
        m_keepAlive.stop();
        return;
    }
    // Nothing since the last ping and the time it had to answer: gone.
    if (m_clock.elapsed() - m_lastHeard > m_keepAlive.interval() + m_timeoutMs) {
        m_keepAlive.stop();
        m_socket.abort();
        return;
    }
    m_socket.ping();
}

void ChatClient::close()
{
    m_socket.close();
}

bool ChatClient::isConnected() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

void ChatClient::request(const QString &op, QJsonObject fields, const Callback &done)
{
    if (!isConnected()) {
        if (done)
            done(failure(QStringLiteral("disconnected"), tr("Not connected.")));
        return;
    }
    const qint64 rid = m_nextRid++;
    fields.insert(QStringLiteral("op"), op);
    fields.insert(QStringLiteral("rid"), double(rid));
    m_pending.insert(rid, done);
    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact)));
}

void ChatClient::onText(const QString &text)
{
    heard();
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
    if (!document.isObject())
        return;
    const QJsonObject json = document.object();
    if (json.value(QStringLiteral("op")) == QLatin1String("provision")) {
        const auto message = Protocol::Json::bytes(json.value(QStringLiteral("message")), -1, 768 * 1024);
        if (message)
            emit provisionReceived(*message);
        return;
    }
    if (json.value(QStringLiteral("op")) == QLatin1String("deliver")) {
        const auto envelope = Protocol::envelopeFromJson(json.value(QStringLiteral("envelope")).toObject());
        if (envelope)
            emit envelopeReceived(*envelope);
        return;
    }
    const auto rid = Protocol::Json::integer(json.value(QStringLiteral("rid")), 1, m_nextRid);
    if (!rid || !m_pending.contains(*rid))
        return;
    const Callback done = m_pending.take(*rid);
    Result result;
    result.ok = json.value(QStringLiteral("ok")).isBool() && json.value(QStringLiteral("ok")).toBool();
    result.error = json.value(QStringLiteral("error")).toString().left(64);
    result.message = json.value(QStringLiteral("message")).toString().left(500);
    result.data = json;
    if (!result.ok && result.error.isEmpty())
        result.error = QStringLiteral("server");
    if (done)
        done(result);
}

void ChatClient::onDisconnected()
{
    m_keepAlive.stop();
    const QHash<qint64, Callback> pending = std::exchange(m_pending, {});
    for (const Callback &done : pending) {
        if (done)
            done(failure(QStringLiteral("disconnected"), tr("The connection was lost.")));
    }
    emit disconnected();
}

void ChatClient::withChallenge(const Callback &done, const std::function<void(const QByteArray &)> &then)
{
    request(QStringLiteral("challenge"), {}, [done, then](const Result &result) {
        const auto nonce = Protocol::Json::bytes(result.data.value(QStringLiteral("nonce")), Protocol::AuthNonceSize);
        if (!result.ok || !nonce) {
            if (done)
                done(result.ok ? failure(QStringLiteral("server"), tr("A bad challenge.")) : result);
            return;
        }
        then(*nonce);
    });
}

void ChatClient::registerAccount(const QString &username, const Crypto::IdentityKeyPair &identity,
                                 const Protocol::PublicSignedPreKey &signedPreKey,
                                 const QList<Protocol::PublicOneTimePreKey> &oneTimePreKeys, const Callback &done)
{
    withChallenge(done, [=](const QByteArray &nonce) {
        request(QStringLiteral("register"),
                {{QStringLiteral("username"), username},
                 {QStringLiteral("identityKey"), Protocol::Json::base64(identity.publicKey())},
                 {QStringLiteral("signedPreKey"), Protocol::signedPreKeyToJson(signedPreKey)},
                 {QStringLiteral("oneTimePreKeys"), Protocol::oneTimePreKeysToJson(oneTimePreKeys)},
                 {QStringLiteral("signature"),
                  Protocol::Json::base64(identity.sign(Protocol::authMessage(nonce, username, 1)))}},
                done);
    });
}

void ChatClient::login(const QString &username, quint32 device, const Crypto::IdentityKeyPair &identity,
                       const Callback &done)
{
    withChallenge(done, [=](const QByteArray &nonce) {
        request(QStringLiteral("login"),
                {{QStringLiteral("username"), username},
                 {QStringLiteral("device"), double(device)},
                 {QStringLiteral("signature"),
                  Protocol::Json::base64(identity.sign(Protocol::authMessage(nonce, username, device)))}},
                done);
    });
}

void ChatClient::fetchKeys(const QString &username, const Callback &done)
{
    request(QStringLiteral("keys"), {{QStringLiteral("username"), username}}, done);
}

std::optional<QList<ChatClient::DeviceBundle>> ChatClient::bundles(const Result &result)
{
    const QJsonValue devices = result.data.value(QStringLiteral("devices"));
    if (!result.ok || !devices.isArray() || devices.toArray().size() > 64)
        return std::nullopt;
    QList<DeviceBundle> out;
    for (const QJsonValue &item : devices.toArray()) {
        const auto device = Protocol::Json::integer(item.toObject().value(QStringLiteral("device")), 1,
                                                    std::numeric_limits<quint32>::max());
        const auto bundle = Protocol::bundleFromJson(item.toObject().value(QStringLiteral("bundle")).toObject());
        if (!device || !bundle)
            return std::nullopt;
        out.append({quint32(*device), *bundle});
    }
    return out;
}

void ChatClient::send(const QString &to, const QHash<quint32, QByteArray> &messages, const Callback &done)
{
    QJsonArray list;
    for (auto it = messages.cbegin(); it != messages.cend(); ++it)
        list.append(QJsonObject{{QStringLiteral("device"), double(it.key())},
                                {QStringLiteral("message"), Protocol::Json::base64(it.value())}});
    request(QStringLiteral("send"), {{QStringLiteral("to"), to}, {QStringLiteral("messages"), list}}, done);
}

void ChatClient::acknowledge(const QStringList &ids, const Callback &done)
{
    request(QStringLiteral("ack"), {{QStringLiteral("ids"), QJsonArray::fromStringList(ids)}}, done);
}

void ChatClient::uploadPreKeys(const std::optional<Protocol::PublicSignedPreKey> &signedPreKey,
                               const QList<Protocol::PublicOneTimePreKey> &oneTimePreKeys, const Callback &done)
{
    QJsonObject fields{{QStringLiteral("oneTimePreKeys"), Protocol::oneTimePreKeysToJson(oneTimePreKeys)}};
    if (signedPreKey)
        fields.insert(QStringLiteral("signedPreKey"), Protocol::signedPreKeyToJson(*signedPreKey));
    request(QStringLiteral("prekeys"), fields, done);
}

void ChatClient::uploadAttachment(const QByteArray &blob, const Progress &progress,
                                  const std::function<void(const QString &, const Result &)> &done)
{
    request(QStringLiteral("upload"), {{QStringLiteral("size"), double(blob.size())}},
            [this, blob, progress, done](const Result &started) {
                const QString id = started.data.value(QStringLiteral("id")).toString();
                if (!started.ok || !Attachment::isValidId(id)) {
                    done(QString(), started.ok ? failure(QStringLiteral("server"), tr("A bad answer.")) : started);
                    return;
                }
                const int chunk = int(qBound(qint64(16 * 1024), qint64(started.data.value(QStringLiteral("chunk")).toDouble()),
                                             qint64(512 * 1024)));
                // One chunk, then the next when the server has it.
                auto next = std::make_shared<std::function<void(qint64)>>();
                *next = [this, blob, id, chunk, progress, done, next](qint64 offset) {
                    if (offset >= blob.size()) {
                        done(id, Result{true, {}, {}, {}});
                        *next = nullptr; // no cycle left behind
                        return;
                    }
                    const QByteArray part = blob.mid(offset, chunk);
                    request(QStringLiteral("upload-chunk"),
                            {{QStringLiteral("id"), id},
                             {QStringLiteral("offset"), double(offset)},
                             {QStringLiteral("data"), Protocol::Json::base64(part)}},
                            [blob, offset, part, progress, done, next](const Result &result) {
                                if (!result.ok) {
                                    done(QString(), result);
                                    *next = nullptr;
                                    return;
                                }
                                if (progress)
                                    progress(offset + part.size(), blob.size());
                                (*next)(offset + part.size());
                            });
                };
                (*next)(0);
            });
}

void ChatClient::downloadAttachment(const QString &id, qint64 size, const Progress &progress,
                                    const std::function<void(const QByteArray &, const Result &)> &done)
{
    if (!Attachment::isValidId(id) || size <= 0 || size > Attachment::blobSize(Attachment::MaxFileBytes)) {
        done({}, failure(QStringLiteral("bad-request"), tr("Not an attachment.")));
        return;
    }
    auto blob = std::make_shared<QByteArray>();
    blob->reserve(int(size));
    auto next = std::make_shared<std::function<void()>>();
    *next = [this, id, size, progress, done, blob, next] {
        if (blob->size() >= size) {
            done(*blob, Result{true, {}, {}, {}});
            *next = nullptr;
            return;
        }
        request(QStringLiteral("download"),
                {{QStringLiteral("id"), id},
                 {QStringLiteral("offset"), double(blob->size())},
                 {QStringLiteral("length"), double(qMin(qint64(512 * 1024), size - blob->size()))}},
                [size, progress, done, blob, next](const Result &result) {
                    const auto data = Protocol::Json::bytes(result.data.value(QStringLiteral("data")), -1, 512 * 1024);
                    const auto total = Protocol::Json::integer(result.data.value(QStringLiteral("size")), 1,
                                                               std::numeric_limits<qint32>::max());
                    if (!result.ok || !data || !total || *total != size || blob->size() + data->size() > size) {
                        done({}, result.ok ? failure(QStringLiteral("server"), tr("The attachment is not what was sent."))
                                           : result);
                        *next = nullptr;
                        return;
                    }
                    blob->append(*data);
                    if (progress)
                        progress(blob->size(), size);
                    (*next)();
                });
    };
    (*next)();
}

void ChatClient::openProvisioning(const Callback &done)
{
    request(QStringLiteral("provision-open"), {}, done);
}

void ChatClient::sendProvisioning(const QString &id, const QByteArray &message, const Callback &done)
{
    request(QStringLiteral("provision-send"),
            {{QStringLiteral("id"), id}, {QStringLiteral("message"), Protocol::Json::base64(message)}}, done);
}

void ChatClient::addDevice(const QString &username, const Crypto::IdentityKeyPair &identity,
                           const Protocol::PublicSignedPreKey &signedPreKey,
                           const QList<Protocol::PublicOneTimePreKey> &oneTimePreKeys, const Callback &done)
{
    withChallenge(done, [=](const QByteArray &nonce) {
        request(QStringLiteral("add-device"),
                {{QStringLiteral("username"), username},
                 {QStringLiteral("identityKey"), Protocol::Json::base64(identity.publicKey())},
                 {QStringLiteral("signedPreKey"), Protocol::signedPreKeyToJson(signedPreKey)},
                 {QStringLiteral("oneTimePreKeys"), Protocol::oneTimePreKeysToJson(oneTimePreKeys)},
                 // Device 0: a device to be (a sign-in is for 1 and up).
                 {QStringLiteral("signature"),
                  Protocol::Json::base64(identity.sign(Protocol::authMessage(nonce, username, 0)))}},
                done);
    });
}

void ChatClient::listDevices(const Callback &done)
{
    request(QStringLiteral("devices"), {}, done);
}

void ChatClient::removeDevice(quint32 device, const Callback &done)
{
    request(QStringLiteral("remove-device"), {{QStringLiteral("device"), double(device)}}, done);
}
