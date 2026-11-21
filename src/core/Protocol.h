#pragma once

#include "Attachment.h"
#include "X3dh.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

// What goes over the wire, in three layers (docs/protocol.md):
//
//   1. Content: what a person wrote (or a receipt), as JSON, padded to a
//      multiple of 160 bytes (as Signal does) so its length shows little.
//      Only ever seen encrypted outside this computer.
//   2. CipherMessage: Content encrypted by a Session. Binary. The first
//      messages of a conversation are "prekey" messages: they also carry what
//      Bob needs for X3DH (Alice's identity key, her ephemeral key, which of
//      his prekeys she used).
//   3. The server's JSON: prekey bundles, and envelopes (a CipherMessage with
//      who sent it and when, as the server says).
//
// Every decode function takes data from a stranger: sizes, types and ranges
// are checked, and anything wrong gives nothing.
namespace Protocol {

constexpr int Version = 1;
constexpr int MaxTextBytes = 64 * 1024;        // a text message's body, UTF-8
constexpr int MaxContentBytes = 128 * 1024;    // a padded Content
constexpr int MaxCipherMessageBytes = 192 * 1024;
constexpr int MaxReceiptIds = 1000;
constexpr int MaxAttachments = 10; // per message
constexpr int MaxQuoteChars = 200;  // a reply's excerpt of the message it answers
constexpr int MaxGroupMembers = 50; // me included
// Disappearing messages: at most four weeks (seconds).
constexpr qint64 MaxExpireSeconds = 4LL * 7 * 24 * 3600;
constexpr int PaddingBlock = 160;

// Usernames: 3 to 32 characters, a-z 0-9 _ and ., starting with a letter, no
// ".." and no "." at the end; ASCII only (no look-alike letters from other
// alphabets). Case does not matter: stored and compared in lower case.
QString normalizeUsername(const QString &username); // empty if not valid
bool isValidUsername(const QString &username);      // already normalized

// A new message id (a UUID without braces).
QString newMessageId();
bool isValidMessageId(const QString &id);

// Layer 1.
// A reply's quote of the message it answers, as Signal's: its id, its author
// (a username, mine too) and the start of its text, for showing it.
struct Quote
{
    QString id;
    QString author;
    QString text; // at most MaxQuoteChars, maybe empty (a file's)
    bool operator==(const Quote &other) const { return id == other.id && author == other.author && text == other.text; }
};

struct Content
{
    enum class Type { Text, Receipt, Profile, Group, Leave, Timer, Reaction };
    enum class Receipt { Delivered, Read };

    Type type = Type::Text;
    QString id;         // this message's id
    qint64 sentAt = 0;  // ms since the epoch, UTC, as the sender's clock says
    QString body;       // Text (with attachments: their caption, maybe empty)
    QList<Attachment::Pointer> attachments; // Text
    Receipt receipt = Receipt::Delivered;
    QStringList receiptFor; // Receipt: the ids of the messages it is for
    // Profile: the sender's display name (maybe empty) and picture (maybe none).
    QString name;
    std::optional<Attachment::Pointer> avatar;
    // Text, Group, Leave, Timer, Reaction: the group (32 hex digits; empty: a direct message).
    QString group;
    // Text, to my own other devices only: whom I sent it to (a username), so
    // they show it as mine (a group's has `group` instead). Timer, Reaction too.
    QString sentTo;
    // Text: the message it answers (a reply). Reaction: the message it is on
    // (its id and author; `text` unused).
    std::optional<Quote> quote;
    // Reaction: one emoji (`isValidReaction`); `remove`: taken back.
    QString emoji;
    bool remove = false;
    // Text: disappears this many seconds after it is seen (0: never).
    // Timer: the conversation's new setting (with `group` for a group's).
    qint64 expireSeconds = 0;
    // Group (from its admin): its name, admin, members (the admin among
    // them) and revision (each update one higher; older ones are ignored).
    QString groupName;
    QString groupAdmin;
    QStringList groupMembers;
    int groupRevision = 0;

    static Content text(const QString &body);
    static Content receiptOf(Receipt kind, const QStringList &ids);
    static Content profile(const QString &name, const std::optional<Attachment::Pointer> &avatar);
    static Content groupUpdate(const QString &group, const QString &name, const QString &admin,
                               const QStringList &members, int revision);
    static Content leave(const QString &group);
    static Content timer(qint64 seconds, const QString &group = QString());
    // On the message `targetId` by `targetAuthor`; empty `emoji` with `remove`.
    static Content reaction(const QString &targetId, const QString &targetAuthor, const QString &emoji, bool remove,
                            const QString &group = QString());
};

// A reaction: one emoji as people type it (with its variation selectors,
// skin tones, joiners): 1 to MaxReactionBytes of UTF-8, no letters, digits,
// spaces, controls or direction marks.
constexpr int MaxReactionBytes = 32;
bool isValidReaction(const QString &emoji);

// A group's id: 32 lower-case hex digits, random.
bool isValidGroupId(const QString &id);
QString newGroupId();

// A display name as it may be shown: no control characters or direction
// marks, white space simplified, at most MaxDisplayName characters.
constexpr int MaxDisplayName = 64;
QString cleanDisplayName(const QString &name);

// Padded. Empty if the content is not valid (too long, bad ids).
QByteArray encodeContent(const Content &content);
// Nothing for an unknown type, a later version, or anything malformed.
std::optional<Content> decodeContent(const QByteArray &padded);

// Signal's padding: 0x80, then zeros up to a multiple of `PaddingBlock`.
QByteArray pad(const QByteArray &data);
std::optional<QByteArray> unpad(const QByteArray &padded);

// Layer 2. On the wire: kind (1 byte: 1 message, 2 prekey message), for a
// prekey message identity key (32) | base key (32) | signed prekey id (4) |
// has one-time prekey (1) | one-time prekey id (4), then the ratchet message.
struct CipherMessage
{
    bool preKey = false;
    QByteArray identityKey; // the sender's (prekey messages)
    QByteArray baseKey;     // the sender's X3DH ephemeral key
    quint32 signedPreKeyId = 0;
    std::optional<quint32> oneTimePreKeyId;
    QByteArray ratchetMessage; // RatchetSession::encrypt's output
};

QByteArray encodeCipherMessage(const CipherMessage &message);
std::optional<CipherMessage> decodeCipherMessage(const QByteArray &data);

// Layer 3: binary fields in standard base64.

// Strict JSON readers for data from strangers: nothing if the value is not
// exactly what is asked.
namespace Json {
// Standard base64 of exactly `size` bytes; size < 0: 1 to `maxSize` bytes.
std::optional<QByteArray> bytes(const QJsonValue &value, int size, int maxSize = 0);
// A whole number in [min, max] (JSON numbers are doubles: up to 2^53).
std::optional<qint64> integer(const QJsonValue &value, qint64 min, qint64 max);
std::optional<quint32> uint32(const QJsonValue &value);
QString base64(const QByteArray &bytes);
} // namespace Json

// Prekeys as they are uploaded: the public halves only.
struct PublicSignedPreKey
{
    quint32 id = 0;
    QByteArray key;
    QByteArray signature;
};

struct PublicOneTimePreKey
{
    quint32 id = 0;
    QByteArray key;
};

constexpr int MaxOneTimePreKeysPerUpload = 100;

PublicSignedPreKey publicPart(const X3dh::SignedPreKey &key);
QList<PublicOneTimePreKey> publicParts(const QList<X3dh::OneTimePreKey> &keys);
QJsonObject signedPreKeyToJson(const PublicSignedPreKey &key);
// The signature is not checked here (it needs the identity key).
std::optional<PublicSignedPreKey> signedPreKeyFromJson(const QJsonValue &json);
QJsonArray oneTimePreKeysToJson(const QList<PublicOneTimePreKey> &keys);
// At most MaxOneTimePreKeysPerUpload, ids all different.
std::optional<QList<PublicOneTimePreKey>> oneTimePreKeysFromJson(const QJsonValue &json);

// Signing in: the server sends a random nonce, the device signs this text
// with its identity key. Bound to the purpose, the nonce and the account.
constexpr int AuthNonceSize = 32;
QByteArray authMessage(const QByteArray &nonce, const QString &username, quint32 device);

QJsonObject bundleToJson(const X3dh::PreKeyBundle &bundle);
std::optional<X3dh::PreKeyBundle> bundleFromJson(const QJsonObject &json);

// A CipherMessage as the server hands it over.
struct Envelope
{
    QString id;           // the server's, to acknowledge it (1-64 printable ASCII)
    QString from;         // username
    quint32 fromDevice = 1;
    qint64 serverTime = 0; // ms, when the server got it
    QByteArray message;    // an encoded CipherMessage
};

QJsonObject envelopeToJson(const Envelope &envelope);
std::optional<Envelope> envelopeFromJson(const QJsonObject &json);

} // namespace Protocol
