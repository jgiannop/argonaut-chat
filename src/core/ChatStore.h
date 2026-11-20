#pragma once

#include <QDateTime>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

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
        int unread = 0;
        bool request = false; // a message request: someone I have not accepted yet
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
    // A message already there with this wire id (to drop one delivered twice).
    bool hasMessage(qint64 conversation, const QString &uid) const;
    // Mine in "sending", oldest first: the outbox.
    QList<Message> outbox() const;
    // Oldest first: the newest `limit` of them.
    QList<Message> messages(qint64 conversation, int limit = 500) const;
    void setStatus(qint64 message, const QString &status);
    // A receipt: mine with these wire ids, in this conversation, move on to
    // "delivered" or "read" (never back).
    void advanceStatus(qint64 conversation, const QStringList &uids, const QString &status);
    // The wire ids of theirs not seen yet (for read receipts).
    QStringList unseenUids(qint64 conversation) const;
    void markRead(qint64 conversation);

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

    // The open database, for KeyStore (the same file).
    QSqlDatabase database() const { return m_db; }

signals:
    void conversationsChanged();
    void messagesChanged(qint64 conversation);

private:
    void createTables();

    QSqlDatabase m_db;
    QString m_connection;
    QString m_error;
    QString m_attachments; // the folder
    bool m_encrypted = false;
};
