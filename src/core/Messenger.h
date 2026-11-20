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
//   - Message requests: the first message from someone I have not written to
//     opens a conversation as a request. Until I accept it they get no
//     receipts (not even delivered), so they cannot tell I exist or read it.
//     Blocked people's messages are dropped without a word.
//   - The connection comes back by itself, sooner first, then slower.
//   - The signed prekey is replaced every 7 days (on signing in, and while
//     connected); an old one is kept 30 more days for conversations started
//     with it, then deleted. A new one counts only once the server has it.
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
    static constexpr qint64 SignedPreKeyLifetimeMs = 7LL * 24 * 3600 * 1000;
    static constexpr qint64 SignedPreKeyGraceMs = 30LL * 24 * 3600 * 1000;

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
    // Tests: the first wait before connecting again, and the clock (ms).
    void setReconnectDelay(int ms) { m_firstDelay = ms; }
    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }

    // Finds the person on the server and opens the conversation with them.
    void startConversation(const QString &username, const std::function<void(qint64 conversation, const QString &error)> &done);
    // Mine: kept at once, sent when it can be. Note to Self stays here.
    qint64 send(qint64 conversation, const QString &text);
    // A file (at most Attachment::MaxFileBytes), with an optional caption:
    // encrypted here, kept here as the encrypted blob, uploaded, then the
    // message with its key goes as any other. 0 and `error` if it cannot be.
    qint64 sendFile(qint64 conversation, const QString &path, const QString &caption, QString *error = nullptr);
    // An attachment here, decrypted in memory; nothing if it is not here (yet).
    std::optional<QByteArray> attachmentData(qint64 attachment) const;
    // Decrypted into `path` (the user's choice).
    bool saveAttachment(qint64 attachment, const QString &path, QString *error = nullptr) const;

    // Opened: their messages are seen, and they get read receipts (not for a request).
    void markRead(qint64 conversation);
    // A message request: accepted (they get the receipts now), deleted, or
    // deleted with its sender blocked.
    void acceptRequest(qint64 conversation);
    void deleteConversation(qint64 conversation);
    void block(const QString &username);
    void unblock(const QString &username);
    bool isBlocked(const QString &username) const { return m_store->isBlocked(username); }
    QStringList blocked() const { return m_store->blocked(); }

    // The 60 digits to compare with them; empty while their key is unknown.
    QString safetyNumber(const QString &username) const;
    bool safetyNumberHasChanged(const QString &username) const { return m_keys->identityChanged(username); }
    void setVerified(const QString &username, bool verified);
    bool isVerified(const QString &username) const { return m_keys->isVerified(username); }

signals:
    void stateChanged(Messenger::State state);
    void safetyNumberChanged(const QString &username);
    // A new text from someone (not a receipt, not one seen before, not blocked).
    void messageReceived(qint64 conversation, const QString &from, const QString &text, bool request);
    // For people: something went wrong that they should know of.
    void problem(const QString &text);
    // An attachment going up or coming down.
    void attachmentProgress(qint64 attachment, qint64 done, qint64 total);

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
    // A message of mine did not go: again later, or failed for good.
    void deliveryFailed(qint64 message, const QString &error, const QString &why);
    // Fetches their attachments, one at a time, in accepted conversations.
    void downloadPending();
    // Encrypts `content` for every device of `to` and sends it.
    void sendContent(const QString &to, const Protocol::Content &content, const Done &done, bool retried = false);
    // Sessions with the devices of `to` that have none (or all, if `devices` lists them).
    void fetchSessions(const QString &to, const Done &done);
    void sendReceipt(const QString &to, Protocol::Content::Receipt kind, const QStringList &ids);
    void replenishPreKeys();
    void rotateSignedPreKey();
    qint64 now() const;

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
    bool m_rotating = false;
    QTimer m_rotation; // checks now and then while connected
    std::function<qint64()> m_clock;
    QSet<qint64> m_inFlight; // outbox messages being sent
    bool m_downloading = false;
};
