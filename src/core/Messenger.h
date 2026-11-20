#pragma once

#include "ChatClient.h"
#include "ChatStore.h"
#include "KeyStore.h"

#include <QJsonObject>
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

    // Groups, kept by their members: the server sees only one-to-one
    // messages (each group message is encrypted for each member). Their
    // creator is the admin: only the admin's updates count (and only newer
    // ones); only members' messages are taken; a group made by someone I
    // have not accepted comes as a request. At most Protocol::MaxGroupMembers.
    using GroupDone = std::function<void(qint64 conversation, const QString &error)>;
    void createGroup(const QString &name, const QStringList &members, const GroupDone &done);
    // The admin only: a new name or members (added ones are checked first).
    void changeGroup(qint64 conversation, const QString &name, const QStringList &members, const GroupDone &done);
    void leaveGroup(qint64 conversation);
    ChatStore::Group group(qint64 conversation) const { return m_store->group(conversation); }

    // Linked devices: one account (one identity key) on up to 5 computers.
    // This one, new: a code to show (its one-time public key and a slot on
    // the server), then the account comes sealed to that key from a device
    // of it, and this one joins. Starts empty: no old history.
    using LinkCode = std::function<void(const QString &code)>;
    void linkThisDevice(const QUrl &server, const LinkCode &showCode, const Done &done);
    void cancelLinking();
    // A device of the account: the account's keys to the device showing `code`.
    void linkDevice(const QString &code, const Done &done);
    using DevicesDone = std::function<void(const QList<quint32> &devices, const QString &error)>;
    void listDevices(const DevicesDone &done);
    void removeDevice(quint32 device, const Done &done);
    quint32 device() const { return m_account ? m_account->device : 0; }
    // The account's devices as last heard from the server (this one too).
    QList<quint32> myDevices() const { return m_myDevices; }
    // The code: its parts as base64url, no padding (64 characters).
    static QString encodeLinkCode(const QByteArray &publicKey, const QString &slot);
    static bool decodeLinkCode(const QString &code, QByteArray *publicKey, QString *slot);

    // Profiles: a display name and a picture, sent only inside encrypted
    // messages, to the people I write to or accept (and again to them all
    // when it changes); the server never has one. The picture: PNG or JPEG,
    // at most MaxAvatarBytes (the window makes it 256×256).
    static constexpr qint64 MaxAvatarBytes = 256 * 1024;
    bool setMyProfile(const QString &name, const QByteArray &avatar, QString *error = nullptr);
    ChatStore::Profile myProfile() const { return m_store->myProfile(); }
    // Their display name if they sent one, else their username.
    QString displayName(const QString &username) const;
    // Their picture (image bytes), once fetched; none for a request until accepted.
    QByteArray avatar(const QString &username) const { return m_store->profile(username).avatar; }

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
    // `content` to each of `to`, each encrypted for them; done(true) once all
    // have it (or cannot ever), done(false, "disconnected"/"rate-limited") if
    // some must be tried again (the others will drop the copy by its id).
    void fanOut(const QStringList &to, const Protocol::Content &content, const Done &done);
    // Sessions with all of `people` (they must all exist), then `done`.
    void checkPeople(const QStringList &people, const Done &done);
    void sendGroupUpdate(const ChatStore::Group &group, const QStringList &to);
    void handleGroupUpdate(const QString &from, const Protocol::Content &content);
    void handleLeave(const QString &from, const Protocol::Content &content);
    QStringList othersIn(qint64 conversation) const;
    // Everyone I have accepted: direct peers, and members of my accepted groups.
    QStringList knownPeople() const;
    // To my own other devices, if there are any (what I sent, what I read,
    // group changes, my profile).
    void syncToMyDevices(const Protocol::Content &content);
    bool haveOtherDevices() const;
    void refreshMyDevices();
    // From another device of mine.
    void handleSync(const Protocol::Content &content);
    void applyMyProfile(const QString &name, const std::optional<Attachment::Pointer> &avatar);
    void onProvision(const QByteArray &sealed);
    // From the linking message: my contacts, groups and blocked people.
    void takeLinkedState(const QJsonObject &json);
    void finishLinking(bool ok, const QString &error);

    // My profile to `peer` if they lack this version; to everyone accepted.
    void shareProfile(const QString &peer);
    void shareProfileWithAll();
    // Their pictures still to fetch, accepted conversations only.
    void fetchAvatars();
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
    bool m_avatarUploading = false;
    bool m_avatarFetching = false;
    QSet<QString> m_profileInFlight;
    // Linking this device: while waiting for the account.
    Done m_linkDone;
    LinkCode m_linkCode;
    Crypto::DhKeyPair m_linkKey;
    QList<quint32> m_myDevices; // all of the account's, this one too
};
