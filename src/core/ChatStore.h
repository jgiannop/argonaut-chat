#pragma once

#include <QDateTime>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

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
    qint64 addConversation(const QString &kind, const QString &title, const QString &peer);
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
    bool m_encrypted = false;
};
