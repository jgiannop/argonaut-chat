#pragma once

#include "Protocol.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
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

    // Any request; `done` gets the answer (or "disconnected").
    void request(const QString &op, QJsonObject fields, const Callback &done);

signals:
    void connected();
    void disconnected();
    void envelopeReceived(const Protocol::Envelope &envelope);
    // The connection's trouble (TLS errors included), for people.
    void failed(const QString &why);

private:
    void onText(const QString &text);
    void onDisconnected();
    // challenge, then `then` with the nonce.
    void withChallenge(const Callback &done, const std::function<void(const QByteArray &nonce)> &then);

    QWebSocket m_socket;
    qint64 m_nextRid = 1;
    QHash<qint64, Callback> m_pending;
};
