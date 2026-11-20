#pragma once

#include "Protocol.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>

#include <optional>

// What the server keeps, and nothing more (docs/how-signal-works.md, "What
// the server keeps"): accounts, each device's public keys, and the queue of
// encrypted messages until each is delivered and acknowledged. It never sees
// what anyone wrote: messages are CipherMessages it cannot open.
//
// One SQLite file, readable by the server's user only (0600, folder 0700).
class ServerStore
{
public:
    static constexpr int MaxOneTimePreKeys = 200;   // per device
    static constexpr int MaxQueuedMessages = 10000; // per device

    explicit ServerStore(const QString &file);
    ~ServerStore();
    ServerStore(const ServerStore &) = delete;
    ServerStore &operator=(const ServerStore &) = delete;

    bool isOpen() const { return m_db.isOpen(); }
    QString error() const { return m_error; }
    static QString defaultFile();

    bool accountExists(const QString &username) const;
    // A new account with its first device (1). False if the name is taken.
    bool createAccount(const QString &username, const QByteArray &identityKey,
                       const Protocol::PublicSignedPreKey &signedPreKey);
    QList<quint32> devices(const QString &username) const;
    // Empty if there is no such device.
    QByteArray identityKey(const QString &username, quint32 device) const;

    bool setSignedPreKey(const QString &username, quint32 device, const Protocol::PublicSignedPreKey &key);
    // Kept up to MaxOneTimePreKeys; a key with an id already there replaces it.
    bool addOneTimePreKeys(const QString &username, quint32 device,
                           const QList<Protocol::PublicOneTimePreKey> &keys);
    int oneTimePreKeyCount(const QString &username, quint32 device) const;
    // A device's bundle; its one-time prekey (if any is left, and `withOneTime`) is taken out.
    std::optional<X3dh::PreKeyBundle> takeBundle(const QString &username, quint32 device, bool withOneTime = true);

    // Queued for one device; the envelope's id. 0 if the queue is full.
    qint64 queue(const QString &to, quint32 toDevice, const QString &from, quint32 fromDevice,
                 const QByteArray &message);
    // Oldest first.
    QList<Protocol::Envelope> queued(const QString &to, quint32 toDevice, int limit = 1000) const;
    std::optional<Protocol::Envelope> envelope(qint64 id) const;
    int queuedCount(const QString &to, quint32 toDevice) const;
    // Only the device's own messages are removed.
    void remove(const QString &to, quint32 toDevice, const QList<qint64> &ids);

    // Attachments: encrypted blobs it cannot read, as files in the folder
    // "attachments" next to the database; their ids are random (32 hex).
    struct AttachmentInfo
    {
        QString owner;
        qint64 size = 0;     // the blob's, as announced
        qint64 received = 0; // so far
        qint64 created = 0;  // ms
        bool complete() const { return received == size; }
    };
    QString createAttachment(const QString &owner, qint64 size);
    std::optional<AttachmentInfo> attachment(const QString &id) const;
    // Only at its end (offset == received), only by its owner, never past its size.
    bool appendAttachment(const QString &id, const QString &owner, qint64 offset, const QByteArray &data);
    QByteArray readAttachment(const QString &id, qint64 offset, qint64 length) const;
    // All announced sizes (what the folder may come to).
    qint64 storedAttachmentBytes() const;
    // Complete ones older than `keepMs`, unfinished ones older than `unfinishedMs`.
    int removeOldAttachments(qint64 now, qint64 keepMs, qint64 unfinishedMs);
    QString attachmentFolder() const { return m_attachments; }

private:
    void createTables();
    QString attachmentFile(const QString &id) const;

    QSqlDatabase m_db;
    QString m_connection;
    QString m_error;
    QString m_attachments; // the folder
};
