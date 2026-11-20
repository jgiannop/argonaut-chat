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
// Not encrypted on disk yet: that comes with the encryption (SQLCipher, or
// our own with a key from the keyring), before anything leaves this computer.
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
        bool outgoing() const { return author.isEmpty(); }
    };

    // `file` empty: the default place.
    explicit ChatStore(const QString &file = QString(), QObject *parent = nullptr);
    ~ChatStore() override;
    bool isOpen() const { return m_db.isOpen(); }
    QString error() const { return m_error; }
    static QString defaultFile();

    // Newest activity first.
    QList<Conversation> conversations() const;
    Conversation conversation(qint64 id) const;
    // Note to Self: always there (made the first time).
    qint64 noteToSelf();
    qint64 addConversation(const QString &kind, const QString &title, const QString &peer);

    // A message of mine (author empty) or of someone else's. Returns it, with its id.
    Message addMessage(qint64 conversation, const QString &body, const QString &author = QString(),
                       const QDateTime &sentAt = QDateTime::currentDateTimeUtc());
    // Oldest first: the newest `limit` of them.
    QList<Message> messages(qint64 conversation, int limit = 500) const;
    void setStatus(qint64 message, const QString &status);
    void markRead(qint64 conversation);

signals:
    void conversationsChanged();
    void messagesChanged(qint64 conversation);

private:
    void createTables();

    QSqlDatabase m_db;
    QString m_connection;
    QString m_error;
};
