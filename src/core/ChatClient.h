#pragma once

#include "Protocol.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <functional>
#include <optional>

// A connection to the relay server (docs/protocol.md): requests with their
// answers, and the envelopes the server pushes. It carries only
// CipherMessages and public keys; encrypting is Session's job.
//
// wss:// anywhere, with TLS errors never ignored; plain ws:// only to this
// computer (tests, or a server behind a local proxy).
//
// Keep-alive: a ping every 30 s; a connection silent for 10 s past that is
// taken as dead (routers drop quiet connections without a word) and closed,
// so disconnected() comes and the Messenger connects again.
//
// Everything the server says is data from a stranger: an answer is accepted
// only as JSON with "ok" exactly true, and envelopes are checked by
// Protocol::envelopeFromJson.
class ChatClient : public QObject
{
    Q_OBJECT

public:
    struct Result
    {
        bool ok = false;
        QString error;   // "denied", "taken", "not-found", "devices", "disconnected", ...
        QString message; // for people
        QJsonObject data;
    };
    using Callback = std::function<void(const Result &)>;

    struct DeviceBundle
    {
        quint32 device = 0;
        X3dh::PreKeyBundle bundle;
    };

    explicit ChatClient(QObject *parent = nullptr);
    ~ChatClient() override;

    // False (and why) if the address is not allowed.
    bool open(const QUrl &url, QString *error = nullptr);
    void close();
    bool isConnected() const;

    void registerAccount(const QString &username, const Crypto::IdentityKeyPair &identity,
                         const Protocol::PublicSignedPreKey &signedPreKey,
                         const QList<Protocol::PublicOneTimePreKey> &oneTimePreKeys, const Callback &done);
    void login(const QString &username, quint32 device, const Crypto::IdentityKeyPair &identity,
               const Callback &done);
    // The answer's devices: see bundles().
    void fetchKeys(const QString &username, const Callback &done);
    static std::optional<QList<DeviceBundle>> bundles(const Result &result);
    // One CipherMessage per device of theirs.
    void send(const QString &to, const QHash<quint32, QByteArray> &messages, const Callback &done);
    // Envelopes safely kept: the server forgets them.
    void acknowledge(const QStringList &ids, const Callback &done = {});
    void uploadPreKeys(const std::optional<Protocol::PublicSignedPreKey> &signedPreKey,
                       const QList<Protocol::PublicOneTimePreKey> &oneTimePreKeys, const Callback &done);

    // Attachments (encrypted blobs, Attachment.h), in chunks, one at a time.
    using Progress = std::function<void(qint64 done, qint64 total)>;
    void uploadAttachment(const QByteArray &blob, const Progress &progress,
                          const std::function<void(const QString &id, const Result &result)> &done);
    // `size`: the blob's size the message promised; another size is refused.
    void downloadAttachment(const QString &id, qint64 size, const Progress &progress,
                            const std::function<void(const QByteArray &blob, const Result &result)> &done);

    // Linking a device. The new one: a slot (the answer's "id"), then
    // provisionReceived when a device of the account sends into it, then
    // addDevice. The account's device: sendProvisioning into that slot.
    void openProvisioning(const Callback &done);
    void sendProvisioning(const QString &id, const QByteArray &message, const Callback &done);
    void addDevice(const QString &username, const Crypto::IdentityKeyPair &identity,
                   const Protocol::PublicSignedPreKey &signedPreKey,
                   const QList<Protocol::PublicOneTimePreKey> &oneTimePreKeys, const Callback &done);
    void listDevices(const Callback &done);
    void removeDevice(quint32 device, const Callback &done);

    // Tests: the ping interval and how long past it a silence is tolerated.
    void setKeepAlive(int intervalMs, int timeoutMs);

    // Any request; `done` gets the answer (or "disconnected").
    void request(const QString &op, QJsonObject fields, const Callback &done);

signals:
    void connected();
    void disconnected();
    void envelopeReceived(const Protocol::Envelope &envelope);
    // The sealed message for a device waiting to be linked.
    void provisionReceived(const QByteArray &message);
    // The connection's trouble (TLS errors included), for people.
    void failed(const QString &why);

private:
    void onText(const QString &text);
    void onDisconnected();
    void checkAlive();
    void heard();
    // challenge, then `then` with the nonce.
    void withChallenge(const Callback &done, const std::function<void(const QByteArray &nonce)> &then);

    QWebSocket m_socket;
    qint64 m_nextRid = 1;
    QHash<qint64, Callback> m_pending;
    QTimer m_keepAlive;
    int m_timeoutMs = 10000;
    QElapsedTimer m_clock;
    qint64 m_lastHeard = 0; // m_clock's ms
};
