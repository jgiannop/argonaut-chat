#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

class ChatStore;

// Moving history to a newly linked computer (as Signal Desktop's "transfer
// message history"): the old computer writes the last year's conversations,
// messages, reactions and files as one stream, cut into pieces that go up as
// encrypted attachments (Attachment.h: a new key each, which the server never
// sees); their pointers reach the new computer only end-to-end encrypted
// (Content::Type::History, from a device of the same account). The new one
// reads the stream as a stranger's: every frame and field checked, nothing
// it already has taken twice.
//
// The stream: "ACHIST1\n", then frames of a kind byte, a big-endian 32-bit
// length and that many bytes:
//   'C' a conversation (JSON: n, kind, peer), numbered from 0
//   'M' a message (JSON: c, uid, author, body, at, status, quote, mentions,
//       edited, reactions, files), its files' bytes following as 'F' frames
//   'F' a file's bytes
//   'E' the end (JSON: messages, files written), and nothing after it
namespace History {

constexpr qint64 PieceBytes = 16LL * 1024 * 1024;  // each attachment's plain size (at most)
constexpr int MaxPieces = 64;                       // what a reader takes (1 GiB; Protocol::MaxHistoryPieces)
constexpr qint64 FileBudget = 768LL * 1024 * 1024;  // files stop being written past this much
constexpr qint64 MaxAgeMs = 366LL * 24 * 3600 * 1000; // a year (and a day)
constexpr qint64 WaitMs = 7LL * 24 * 3600 * 1000;   // how long a new computer awaits it

// The old computer's side: the stream, a piece at a time (a gigabyte is never
// held at once).
class Writer
{
public:
    Writer(ChatStore *store, qint64 nowMs);
    ~Writer();

    // The next piece, at most PieceBytes; empty once all is written.
    QByteArray next();
    // How far (0 to 1), by messages.
    double progress() const;
    int messagesWritten() const { return m_messages; }
    int filesWritten() const { return m_files; }

private:
    void writeFrame(char kind, const QByteArray &payload);
    void writeMessage(qint64 id);

    ChatStore *m_store;
    QHash<qint64, int> m_conversations; // local id -> number in the stream
    QList<qint64> m_ids;                // the messages, newest first
    int m_next = 0;
    bool m_ended = false;
    QByteArray m_buffer;
    qint64 m_streamed = 0; // handed out so far
    qint64 m_fileBytes = 0;
    int m_messages = 0;
    int m_files = 0;
};

// The new computer's side.
class Reader
{
public:
    // `me`: my username (whose @mentions are mine).
    Reader(ChatStore *store, const QString &me, qint64 nowMs);
    ~Reader();

    // A piece, in order. False: the stream is broken (nothing more is taken;
    // what came before stays).
    bool feed(const QByteArray &piece);
    // True if the end came, matched, and nothing followed.
    bool finish() const;
    int messagesTaken() const { return m_messages; }
    int filesTaken() const { return m_files; }

private:
    struct PendingFile
    {
        QString name;
        QString type;
        qint64 size = 0;
        bool voice = false;
    };
    bool frame(char kind, const QByteArray &payload);
    bool conversation(const QByteArray &payload);
    bool message(const QByteArray &payload);
    bool file(const QByteArray &payload);
    bool end(const QByteArray &payload);

    ChatStore *m_store;
    QString m_me;
    qint64 m_now;
    QByteArray m_buffer;
    bool m_headerSeen = false;
    bool m_broken = false;
    bool m_ended = false;
    QList<qint64> m_conversations; // number in the stream -> local id (0: skipped)
    QList<QString> m_kinds;
    QList<QString> m_peers;
    qint64 m_fileMessage = 0;      // the message the next 'F' frames belong to (0: dropped)
    QList<PendingFile> m_pendingFiles;
    QSet<qint64> m_touched;        // conversations changed
    QStringList m_seen;            // theirs taken, to mark seen
    qint64 m_received = 0;
    int m_messagesRead = 0;        // frames, taken or not
    int m_filesRead = 0;
    int m_messages = 0;
    int m_files = 0;
};

} // namespace History
