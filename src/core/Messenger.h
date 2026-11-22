#pragma once

#include "ChatClient.h"
#include "ChatStore.h"
#include "AppConfig.h"
#include "History.h"
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

    // The server to use (AppConfig::serverUrl() unless told otherwise: tests).
    // An account kept with another address is moved to this one.
    void useServer(const QUrl &server);
    QUrl server() const { return m_server; }
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
    // `replyTo`: a message of this conversation it answers (quoted, as Signal does).
    // `mentions`: @mentions in `text`, in a group only, of its other members (others dropped).
    qint64 send(qint64 conversation, const QString &text, qint64 replyTo = 0, const QList<Protocol::Mention> &mentions = {});
    // A file (at most Attachment::MaxFileBytes), with an optional caption:
    // encrypted here, kept here as the encrypted blob, uploaded, then the
    // message with its key goes as any other. 0 and `error` if it cannot be.
    qint64 sendFile(qint64 conversation, const QString &path, const QString &caption, QString *error = nullptr,
                    qint64 replyTo = 0, const QList<Protocol::Mention> &mentions = {});
    // A voice message: 16-bit mono samples at Voice::SampleRate, sent as a
    // WAV attachment marked "voice".
    // The same for bytes in memory (a pasted picture: never written to disk
    // unencrypted); its type is told from its name and its start.
    qint64 sendFileData(qint64 conversation, const QString &fileName, const QByteArray &data, const QString &caption,
                        QString *error = nullptr, qint64 replyTo = 0, const QList<Protocol::Mention> &mentions = {});
    // A voice message from mono 16-bit samples (encoded here: tests), or
    // from an Ogg Opus file made while recording (Voice::OpusWriter).
    qint64 sendVoice(qint64 conversation, const QByteArray &samples, QString *error = nullptr, qint64 replyTo = 0);
    qint64 sendVoiceNote(qint64 conversation, const QByteArray &ogg, QString *error = nullptr, qint64 replyTo = 0);
    // What a reply to `message` quotes: its id, author and the start of its text.
    std::optional<Protocol::Quote> quoteOf(qint64 message) const;
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
    // `withHistory`: the last year's messages and files follow, encrypted
    // (History.h), once the new computer has joined.
    void linkDevice(const QString &code, const Done &done, bool withHistory = false);
    // History going to a new computer, or coming from my other one.
    bool isSendingHistory() const { return m_historyWriter != nullptr || m_historyDelivering; }
    bool isReceivingHistory() const { return m_historyReader != nullptr; }
    using DevicesDone = std::function<void(const QList<quint32> &devices, const QString &error)>;
    void listDevices(const DevicesDone &done);
    void removeDevice(quint32 device, const Done &done);
    quint32 device() const { return m_account ? m_account->device : 0; }
    // The account's devices as last heard from the server (this one too).
    QList<quint32> myDevices() const { return m_myDevices; }
    // The code: its parts as base64url, no padding (64 characters).
    static QString encodeLinkCode(const QByteArray &publicKey, const QString &slot);
    static bool decodeLinkCode(const QString &code, QByteArray *publicKey, QString *slot);

    // Disappearing messages, as Signal's: either side of a chat (any member
    // of a group) sets the conversation's timer; each message then carries
    // it and disappears that long after it is seen (mine: after it is sent),
    // on every side, attachments and all.
    void setDisappearing(qint64 conversation, qint64 seconds);
    // "1 hour", "off"…: for the notes and the menu.
    static QString describeTimer(qint64 seconds);
    // Tests (and the timer): deletes what is due now, then waits for the next.
    void expireNow();

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

    // Typing indicators (as Signal's): "started" again every TypingRepeatMs
    // while typing, "stopped" when sent or cleared; theirs shown until
    // TypingShownMs without news, or their message. Online only: never
    // queued, and one older than 30 s by the server's clock is ignored. Off:
    // neither sent nor shown. Not in requests.
    static constexpr qint64 TypingRepeatMs = 10000;
    static constexpr qint64 TypingShownMs = 15000;
    void setTypingIndicators(bool on);
    bool typingIndicators() const { return m_typingIndicators; }
    // Read receipts (as Signal's, both ways): off, others are not told when I
    // read theirs, and theirs show me only "delivered". Delivered receipts
    // and my own devices' read state go on as before.
    void setReadReceipts(bool on) { m_readReceipts = on; }
    bool readReceipts() const { return m_readReceipts; }
    void setTyping(qint64 conversation, bool typing);
    // Who is typing there now (usernames).
    QStringList typingIn(qint64 conversation) const;
    // Tests (and the timer): forgets those whose time is up.
    void expireTyping();

    // Editing (as Signal's): my text, sent, at most EditMs ago; everyone
    // (my other devices too) sees the new text, marked edited. Files stay.
    static constexpr qint64 EditMs = 24LL * 3600 * 1000;
    bool canEdit(qint64 message) const;
    bool edit(qint64 message, const QString &text, const QList<Protocol::Mention> &mentions = {});
    // The @mentions that may stay in a message of `conversation`: in a group,
    // of its members (`me` too, or not), in order; none if they don't fit `text`.
    QList<Protocol::Mention> mentionsIn(qint64 conversation, const QString &text, const QList<Protocol::Mention> &mentions,
                                        bool me) const;

    // Deleting (as Signal's). For me: from this computer only, any message.
    // For everyone: mine, sent, at most DeleteForEveryoneMs ago; the others
    // and my other devices keep only "This message was deleted".
    static constexpr qint64 DeleteForEveryoneMs = 24LL * 3600 * 1000;
    void deleteForMe(qint64 message);
    bool canDeleteForEveryone(qint64 message) const;
    bool deleteForEveryone(qint64 message);

    // Reactions (as Signal's): one emoji per person per message; mine to
    // everyone in the conversation and my other devices, kept until sent.
    // Empty `emoji`: take mine back. Not on notes, requests or left groups.
    static QStringList quickReactions(); // 👍 ❤️ 😂 😮 😢 🙏
    bool react(qint64 message, const QString &emoji);

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
    // Calls (the signalling only; the call itself is Calls' in chat_call):
    // only in an accepted direct conversation, never with the blocked.
    bool canCall(qint64 conversation) const;
    QString callPeer(qint64 conversation) const; // whom; empty if canCall is false
    // Sends `call` to every device of `peer`; Hangup with AnsweredElsewhere or
    // DeclinedElsewhere goes to my own other devices instead.
    void sendCall(const QString &peer, const Protocol::Call &call, const Done &done);
    // The relay (TURN) for calls, as the server hands it out for a while:
    // no urls when it has none (then calls go only directly).
    struct Relay
    {
        QStringList urls;
        QString username;
        QString credential;
    };
    void fetchRelay(const std::function<void(const Relay &relay, const QString &error)> &done);
    // A note in a conversation's history ("Missed call"), dated now.
    void addCallNote(const QString &peer, const QString &text);
    // The engine's clock (ms; a fake one in tests).
    qint64 now() const;

    QString safetyNumber(const QString &username) const;
    bool safetyNumberHasChanged(const QString &username) const { return m_keys->identityChanged(username); }
    void setVerified(const QString &username, bool verified);
    bool isVerified(const QString &username) const { return m_keys->isVerified(username); }

signals:
    void stateChanged(Messenger::State state);
    void safetyNumberChanged(const QString &username);
    // A new text from someone (not a receipt, not one seen before, not blocked).
    // `mentionsMe`: it @mentions me (notified even when muted, as Signal does).
    void messageReceived(qint64 conversation, const QString &from, const QString &text, bool request, bool mentionsMe);
    // For people: something went wrong that they should know of.
    void problem(const QString &text);
    // Who is typing in `conversation` changed (typingIn).
    void typingChanged(qint64 conversation);
    // Someone reacted to a message of mine (not a removal, not in a request).
    void reactionReceived(qint64 conversation, const QString &from, const QString &emoji, const QString &onText);
    // An attachment going up or coming down.
    void attachmentProgress(qint64 attachment, qint64 done, qint64 total);
    // History to a new computer (`sending`) or from my other one: how far,
    // and how it ended (`message` for people).
    void historyProgress(bool sending, int percent);
    void historyFinished(bool sending, bool ok, const QString &message);
    // A call's signalling from `peer` (`device`: theirs), or, with `mine`,
    // from another device of mine about a call with `peer`. `age`: how long
    // the server held it (ms).
    void callSignal(const QString &peer, quint32 device, const Protocol::Call &call, qint64 age, bool mine);

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
    qint64 sendData(qint64 conversation, const QString &fileName, const QString &type, const QByteArray &data,
                    const QString &caption, QString *error, qint64 replyTo, bool voice,
                    const QList<Protocol::Mention> &mentions = {});
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
    // `mine`: from another device of mine (`from` is me).
    void handleReaction(const QString &from, const Protocol::Content &content, bool mine);
    void sendPendingReactions();
    void handleDelete(const QString &from, const Protocol::Envelope &envelope, const Protocol::Content &content, bool mine);
    void sendPendingDeletes();
    void handleEdit(const QString &from, const Protocol::Envelope &envelope, const Protocol::Content &content, bool mine);
    void sendPendingEdits();
    void handleTyping(const QString &from, const Protocol::Envelope &envelope, const Protocol::Content &content);
    void stopShowingTyping(qint64 conversation, const QString &who);
    QStringList othersIn(qint64 conversation) const;
    // Everyone I have accepted: direct peers, and members of my accepted groups.
    QStringList knownPeople() const;
    // To my own other devices, if there are any (what I sent, what I read,
    // group changes, my profile).
    void syncToMyDevices(const Protocol::Content &content);
    bool haveOtherDevices() const;
    void refreshMyDevices();
    // From another device of mine.
    void handleSync(const Protocol::Envelope &envelope, const Protocol::Content &content);
    void applyMyProfile(const QString &name, const std::optional<Attachment::Pointer> &avatar);
    void onProvision(const QByteArray &sealed);
    void scheduleExpiry();
    void handleTimer(const QString &from, const Protocol::Content &content);
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
    // now(), but later than the last one it gave: two edits or reactions in
    // the same millisecond still have their order (and the newer one stays pending).
    qint64 nextLocalTime();

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
    QSet<qint64> m_reactionsInFlight; // messages whose reaction of mine is being sent
    QSet<qint64> m_deletesInFlight;
    QSet<qint64> m_editsInFlight;
    qint64 m_lastLocalTime = 0;
    bool m_typingIndicators = true;
    bool m_readReceipts = true;
    QHash<qint64, qint64> m_typingSentAt;            // where I said I am typing: when
    QHash<qint64, QHash<QString, qint64>> m_typing;  // who types where: until when
    QTimer m_typingExpiry;
    bool m_downloading = false;
    QUrl m_server = AppConfig::serverUrl();
    // History (History.h): going up, then its pointers to the new computer;
    // or coming down, piece by piece.
    void sendNextHistoryPiece();
    void deliverHistory();
    void failHistory(bool sending, const QString &why);
    void handleHistory(const Protocol::Content &content);
    void fetchNextHistoryPiece();
    std::unique_ptr<History::Writer> m_historyWriter;
    QList<Attachment::Pointer> m_historyPieces;
    QList<quint32> m_devicesBeforeLink;
    bool m_historyDelivering = false;
    int m_historyWaits = 0;
    std::unique_ptr<History::Reader> m_historyReader;
    QList<Attachment::Pointer> m_historyToFetch;
    int m_historyIndex = 0;
    bool m_historyFetching = false;
    bool m_avatarUploading = false;
    bool m_avatarFetching = false;
    QSet<QString> m_profileInFlight;
    QTimer m_expiry; // the next disappearing message
    // Linking this device: while waiting for the account.
    Done m_linkDone;
    LinkCode m_linkCode;
    Crypto::DhKeyPair m_linkKey;
    QList<quint32> m_myDevices; // all of the account's, this one too
};
