#pragma once

#include "ChatClient.h"
#include "ChatStore.h"
#include "KeyStore.h"

#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>

// The messenger itself, without widgets: the account, the connection to the
// server, and every message in and out, encrypted end to end.
//
//   - Sign-up: a new identity key, a signed prekey and one-time prekeys,
//     kept here (KeyStore), the public halves sent to the server.
//   - Sending: the message is kept at once ("sending", the outbox), then
//     encrypted for every device of the other person (Session) and sent;
//     "sent" when the server has it, "delivered" and "read" by their receipts.
//     What could not go out goes when the connection is back.
//   - Receiving: decrypted, kept, then acknowledged (so the server forgets
//     it), and a delivered receipt goes back. Twice the same message is kept
//     once. A new identity key for someone is noticed (safetyNumberChanged).
//   - The connection comes back by itself, sooner first, then slower.
//
// Note to Self never leaves this computer.
class Messenger : public QObject
{
    Q_OBJECT

public:
    enum class State { NoAccount, Offline, Connecting, Online };
    Q_ENUM(State)

    static constexpr int PreKeyBatch = 100;
    static constexpr int PreKeyLowWater = 20;
    static constexpr int MaxReconnectDelayMs = 60000;

    using Done = std::function<void(bool ok, const QString &error)>;

    Messenger(ChatStore *store, KeyStore *keys, QObject *parent = nullptr);
    ~Messenger() override;

    State state() const { return m_state; }
    bool hasAccount() const { return m_account && m_account->registered; }
    QString username() const { return hasAccount() ? m_account->username : QString(); }

    // A new account on `server`; connected when `done` says yes.
    void signUp(const QUrl &server, const QString &username, const Done &done);
    // Connected while there is an account, until stop().
    void start();
    void stop();
    // Tests: the first wait before connecting again.
    void setReconnectDelay(int ms) { m_firstDelay = ms; }

    // Finds the person on the server and opens the conversation with them.
    void startConversation(const QString &username, const std::function<void(qint64 conversation, const QString &error)> &done);
    // Mine: kept at once, sent when it can be. Note to Self stays here.
    qint64 send(qint64 conversation, const QString &text);
    // Opened: their messages are seen, and they get read receipts.
    void markRead(qint64 conversation);

    // The 60 digits to compare with them; empty while their key is unknown.
    QString safetyNumber(const QString &username) const;
    bool safetyNumberHasChanged(const QString &username) const { return m_keys->identityChanged(username); }
    void setVerified(const QString &username, bool verified);
    bool isVerified(const QString &username) const { return m_keys->isVerified(username); }

signals:
    void stateChanged(Messenger::State state);
    void safetyNumberChanged(const QString &username);
    // For people: something went wrong that they should know of.
    void problem(const QString &text);

private:
    void setState(State state);
    void connectNow();
    void onConnected();
    void onConnectionLost(const QString &why);
    void scheduleReconnect();
    void finishSignUp(bool ok, const QString &error);

    void receive(const Protocol::Envelope &envelope);
    std::optional<QByteArray> decrypt(const Protocol::Envelope &envelope, const Protocol::CipherMessage &message);
    void handleContent(const QString &from, const Protocol::Envelope &envelope, const Protocol::Content &content);

    void flushOutbox();
    void deliver(const ChatStore::Message &message);
    // Encrypts `content` for every device of `to` and sends it.
    void sendContent(const QString &to, const Protocol::Content &content, const Done &done, bool retried = false);
    // Sessions with the devices of `to` that have none (or all, if `devices` lists them).
    void fetchSessions(const QString &to, const Done &done);
    void sendReceipt(const QString &to, Protocol::Content::Receipt kind, const QStringList &ids);
    void replenishPreKeys();

    ChatStore *m_store;
    KeyStore *m_keys;
    ChatClient m_client;
    std::optional<KeyStore::Account> m_account;
    State m_state = State::NoAccount;
    bool m_running = false;
    Done m_signUpDone; // while signing up
    QList<X3dh::OneTimePreKey> m_signUpPreKeys;
    QTimer m_reconnect;
    QTimer m_retry; // the outbox again, after the server said "too many"
    qint64 m_retryAfterMs = 0;
    int m_firstDelay = 1000;
    int m_delay = 1000;
    int m_serverPreKeys = 0;
    bool m_replenishing = false;
    QSet<qint64> m_inFlight; // outbox messages being sent
};
