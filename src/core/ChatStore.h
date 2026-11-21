#pragma once

#include "Attachment.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include <optional>

// The conversations and their messages, kept on this computer only, as
// Signal keeps them (see docs/how-signal-works.md): one SQLite file,
// $XDG_DATA_HOME/Argonaut/argonaut-chat/chat.sqlite, that only the user can
// read (0600, its folder 0700). A server, when there is one, only passes
// messages on; the history is here.
//
// Encrypted with SQLCipher (CipherSql) when given a key: the app's key lives
// in the system keyring (DatabaseKey). A plain file found where an encrypted
// one should be (from before the encryption) is encrypted in place. Without a
// key (tests) the file is plain SQLite.
class ChatStore : public QObject
{
    Q_OBJECT

public:
    struct Conversation
    {
        qint64 id = 0;
        QString kind;  // "self" (Note to Self), "direct", "group"
        QString title;
        QString peer;  // who it is with (empty for Note to Self)
        QDateTime lastActivity;
        QString lastText; // the newest message, for the list
        bool lastDeleted = false; // it was deleted for everyone
        int unread = 0;
        bool request = false; // a message request: someone I have not accepted yet
        bool left = false;    // a group I am no longer in (read only)
        qint64 expireSeconds = 0; // disappearing messages: 0 off
    };

    struct Message
    {
        qint64 id = 0;
        qint64 conversation = 0;
        QString author; // empty: me
        QString body;
        QDateTime sentAt;
        QString status; // mine: "sending", "sent", "delivered", "read", "failed"; theirs: "received"
        QString uid;    // the message's id on the wire (Protocol::Content::id); empty for notes to self
        qint64 expireSeconds = 0; // disappears this long after it is seen (mine: sent); 0 never
        qint64 expiresAt = 0;     // ms; 0: not counting yet
        bool system = false;      // a note in the conversation ("… set disappearing messages …")
        // A reply: the message it answers (its wire id, its author's username, an excerpt).
        QString quoteUid;
        QString quoteAuthor;
        QString quoteText;
        bool isReply() const { return !quoteAuthor.isEmpty(); }
        bool deleted = false; // deleted for everyone: no text, files or reactions left
        qint64 editedAt = 0;  // ms; 0: never edited
        bool outgoing() const { return author.isEmpty(); }
    };

    // `file` empty: the default place. `key`: 32 bytes, or empty for a plain file.
    explicit ChatStore(const QString &file = QString(), const QByteArray &key = QByteArray(),
                       QObject *parent = nullptr);
    ~ChatStore() override;
    bool isOpen() const { return m_db.isOpen(); }
    bool isEncrypted() const { return m_encrypted; }
    QString error() const { return m_error; }
    static QString defaultFile();

    // Newest activity first.
    QList<Conversation> conversations() const;
    Conversation conversation(qint64 id) const;
    // Note to Self: always there (made the first time).
    qint64 noteToSelf();
    // `request`: started by them, not accepted by me yet.
    qint64 addConversation(const QString &kind, const QString &title, const QString &peer, bool request = false);
    void setAccepted(qint64 conversation);
    // The conversation and all its messages.
    void deleteConversation(qint64 conversation);
    // The direct conversation with this username; 0 if there is none.
    qint64 directConversation(const QString &peer) const;

    // A message of mine (author empty) or of someone else's. Returns it, with its id.
    // `status` empty: "sent" for mine, "received" for theirs.
    Message addMessage(qint64 conversation, const QString &body, const QString &author = QString(),
                       const QDateTime &sentAt = QDateTime::currentDateTimeUtc(), const QString &uid = QString(),
                       const QString &status = QString());
    Message message(qint64 id) const;
    // Search: the messages of `conversation` whose text contains `text`,
    // whatever its case (Greek too), oldest first; notes and deleted ones never.
    QList<qint64> findMessages(qint64 conversation, const QString &text) const;
    int messageCount(qint64 conversation) const;
    // A message already there with this wire id (to drop one delivered twice).
    bool hasMessage(qint64 conversation, const QString &uid) const;
    // Its local id, or 0.
    qint64 messageByUid(qint64 conversation, const QString &uid) const;
    // A reply: what it answers.
    void setQuote(qint64 message, const QString &uid, const QString &author, const QString &text);
    // Mine in "sending", oldest first: the outbox.
    QList<Message> outbox() const;
    // Oldest first: the newest `limit` of them.
    QList<Message> messages(qint64 conversation, int limit = 500) const;
    void setStatus(qint64 message, const QString &status);
    // A receipt: mine with these wire ids, in this conversation, move on to
    // "delivered" or "read" (never back).
    void advanceStatus(qint64 conversation, const QStringList &uids, const QString &status);
    // The same for a receipt from `from`: mine with these ids, in any
    // conversation with them (direct, or a group they are in).
    void advanceStatusFrom(const QString &from, const QStringList &uids, const QString &status);
    // Theirs not seen yet, by author (group read receipts go to each).
    QHash<QString, QStringList> unseenUidsByAuthor(qint64 conversation) const;
    // The wire ids of theirs not seen yet (for read receipts).
    QStringList unseenUids(qint64 conversation) const;
    void markRead(qint64 conversation);
    // Read on another of my devices: theirs with these wire ids, seen.
    void markSeenUids(const QStringList &uids);

    // Disappearing messages.
    void setExpireSeconds(qint64 conversation, qint64 seconds);
    // A message's timer; mine count from `startAt` (ms; 0: once seen).
    void setMessageExpiry(qint64 message, qint64 seconds, qint64 startAt);
    // Theirs, seen and on a timer: counting from `now` (ms).
    void startExpiry(qint64 conversation, qint64 now);
    void startExpiryForUids(const QStringList &uids, qint64 now);
    // Gone, with their attachments' files: those whose time came. How many.
    int deleteExpired(qint64 now);
    // The next moment one is due (ms), or 0.
    qint64 nextExpiry() const;
    // A note in the conversation, not a message from anyone.
    qint64 addSystemNote(qint64 conversation, const QString &text, const QDateTime &at);

    // Editing: its text now `body`, edited at `at` (ms). `pending`: mine, to be sent.
    void editMessage(qint64 message, const QString &body, qint64 at, bool pending = false);
    QList<qint64> pendingEdits() const;
    // Sent: the edit made at `at` (a newer one stays pending).
    void setEditSent(qint64 message, qint64 at);

    // Deleting. For me: gone from here, with its files and reactions.
    void deleteMessage(qint64 message);
    // For everyone: what it said, its files and reactions go, and the quotes of
    // it in replies; a mark stays ("This message was deleted"). `pending`:
    // mine, to be sent to the others.
    void markDeleted(qint64 message, bool pending = false);
    QList<qint64> pendingDeletes() const;
    void setDeleteSent(qint64 message);

    // Reactions: one emoji per person per message (author empty: mine).
    struct Reaction
    {
        qint64 message = 0;
        QString author;
        QString emoji;
        qint64 at = 0; // ms
    };
    // Empty `emoji`: taken back. Mine are "pending" until sent (`pending`).
    void setReaction(qint64 message, const QString &author, const QString &emoji, qint64 at, bool pending = false);
    // Oldest first.
    QList<Reaction> reactions(qint64 message) const;
    QString myReaction(qint64 message) const;
    // Mine not sent yet (an emoji, or empty: a removal), oldest first.
    QList<Reaction> pendingReactions() const;
    // Sent: the one made at `at` (a newer one stays pending).
    void setReactionSent(qint64 message, qint64 at);

    // People whose messages are dropped.
    void block(const QString &username);
    void unblock(const QString &username);
    bool isBlocked(const QString &username) const;
    QStringList blocked() const;

    // Attachments of messages. The files are kept as the encrypted blobs
    // (Attachment.h) in the folder "attachments" next to the database (0700);
    // their keys are here, in the encrypted database.
    struct StoredAttachment
    {
        qint64 id = 0;
        qint64 message = 0;
        QString name;
        QString type;
        qint64 size = 0; // the file's
        QByteArray key;
        QByteArray digest;
        QString remoteId; // the server's
        QString file;     // the blob's file name in attachmentFolder(); empty until here
        QString state;    // "upload", "sent", "download", "ready", "failed"
        bool voice = false; // a voice message
    };
    qint64 addAttachment(const StoredAttachment &attachment);
    StoredAttachment attachment(qint64 id) const;
    QList<StoredAttachment> attachments(qint64 message) const;
    // Theirs still to fetch, in accepted conversations only.
    QList<StoredAttachment> pendingDownloads() const;
    void setAttachmentState(qint64 id, const QString &state);
    void setAttachmentUploaded(qint64 id, const QString &remoteId);
    void setAttachmentFile(qint64 id, const QString &file);
    QString attachmentFolder() const { return m_attachments; }
    // A blob into the folder: its new file name (empty if it could not be written).
    QString writeBlob(const QByteArray &blob);
    QByteArray readBlob(const QString &file) const;

    // Groups (kept by their members; the server knows nothing of them): a
    // conversation of kind "group", its peer the group's id, its title the
    // name; the members, the admin, and the revision of the last update.
    struct Group
    {
        qint64 conversation = 0;
        QString id;
        QString name;
        QString admin;
        QStringList members;
        int revision = 0;
    };
    qint64 groupConversation(const QString &groupId) const;
    Group group(qint64 conversation) const;
    qint64 addGroup(const Group &group, bool request);
    void updateGroup(const Group &group);
    void setLeft(qint64 conversation);
    bool isMember(qint64 conversation, const QString &username) const;

    // Profiles: a display name and a picture (image bytes, small), mine and
    // others'. Mine has a version, raised by every change, and the pointer of
    // its picture once uploaded (reused for everyone until near its expiry).
    struct Profile
    {
        QString name;
        QByteArray avatar;
    };
    Profile myProfile() const;
    int myProfileVersion() const;
    void setMyProfile(const QString &name, const QByteArray &avatar);
    std::optional<Attachment::Pointer> myAvatarPointer(qint64 notBefore) const; // uploaded at notBefore or later
    void setMyAvatarPointer(const Attachment::Pointer &pointer, qint64 uploadedAt);
    Profile profile(const QString &username) const;
    // Theirs; `avatarPointer` is the picture still to fetch (none: no picture).
    void setProfile(const QString &username, const QString &name, const std::optional<Attachment::Pointer> &avatarPointer);
    std::optional<Attachment::Pointer> pendingAvatar(const QString &username) const;
    void setAvatar(const QString &username, const QByteArray &avatar);
    // The version of my profile each person has.
    int profileSentVersion(const QString &username) const;
    void setProfileSent(const QString &username, int version);

    // The open database, for KeyStore (the same file).
    QSqlDatabase database() const { return m_db; }

signals:
    void conversationsChanged();
    void messagesChanged(qint64 conversation);
    void profileChanged(const QString &username); // empty: mine

private:
    void createTables();

    QSqlDatabase m_db;
    QString m_connection;
    QString m_error;
    QString m_attachments; // the folder
    bool m_encrypted = false;
};
