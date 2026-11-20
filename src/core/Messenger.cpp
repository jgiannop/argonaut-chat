#include "Messenger.h"

#include "Attachment.h"
#include "SafetyNumber.h"

#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QTimeZone>

Messenger::Messenger(ChatStore *store, KeyStore *keys, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_keys(keys)
{
    Crypto::init();
    m_account = m_keys->account();
    if (m_account && !m_account->registered) {
        // A sign-up that never finished: start again from nothing.
        m_keys->forgetAccount();
        m_account.reset();
    }
    m_state = hasAccount() ? State::Offline : State::NoAccount;

    m_rotation.setInterval(6 * 3600 * 1000);
    connect(&m_rotation, &QTimer::timeout, this, &Messenger::rotateSignedPreKey);
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        if (m_state == State::Online)
            flushOutbox();
    });
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &Messenger::connectNow);
    connect(&m_client, &ChatClient::connected, this, &Messenger::onConnected);
    connect(&m_client, &ChatClient::disconnected, this, [this] { onConnectionLost(tr("The connection was lost.")); });
    connect(&m_client, &ChatClient::failed, this, &Messenger::onConnectionLost);
    connect(&m_client, &ChatClient::envelopeReceived, this, &Messenger::receive);
}

Messenger::~Messenger()
{
    m_running = false;
    m_client.disconnect(this);
}

void Messenger::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

// --- The account and the connection ---

void Messenger::signUp(const QUrl &server, const QString &username, const Done &done)
{
    const QString name = Protocol::normalizeUsername(username);
    if (hasAccount() || m_signUpDone) {
        done(false, tr("There is an account already."));
        return;
    }
    if (name.isEmpty()) {
        done(false, tr("A username is 3 to 32 letters (a-z), digits, _ or ., and starts with a letter."));
        return;
    }
    m_client.close();
    QString error;
    // Kept before the server hears of it, so a key it accepts is never lost.
    KeyStore::Account account{name, 1, server, Crypto::IdentityKeyPair::generate(), false};
    m_keys->forgetAccount();
    if (!m_keys->saveAccount(account)) {
        done(false, tr("The keys could not be kept on this computer."));
        return;
    }
    const X3dh::SignedPreKey signedPreKey = X3dh::makeSignedPreKey(account.identity, m_keys->takePreKeyIds(1));
    m_signUpPreKeys = X3dh::makeOneTimePreKeys(m_keys->takePreKeyIds(PreKeyBatch), PreKeyBatch);
    m_keys->addSignedPreKey(signedPreKey, now(), true); // sent with the sign-up
    m_keys->addOneTimePreKeys(m_signUpPreKeys);
    m_account = account;
    m_signUpDone = done;
    if (!m_client.open(server, &error)) {
        finishSignUp(false, error);
        return;
    }
    setState(State::Connecting);
}

void Messenger::finishSignUp(bool ok, const QString &error)
{
    const Done done = std::exchange(m_signUpDone, {});
    m_signUpPreKeys.clear();
    if (!ok) {
        m_keys->forgetAccount();
        m_account.reset();
        // Before closing, so the closing is not taken for a lost connection.
        setState(State::NoAccount);
        m_client.close();
    } else {
        m_keys->setRegistered();
        m_account = m_keys->account();
        m_running = true;
        m_delay = m_firstDelay;
        m_serverPreKeys = PreKeyBatch;
        setState(State::Online);
    }
    if (done)
        done(ok, error);
}

void Messenger::start()
{
    if (!hasAccount())
        return;
    m_running = true;
    m_delay = m_firstDelay;
    if (m_state == State::Offline)
        connectNow();
}

void Messenger::stop()
{
    m_running = false;
    m_reconnect.stop();
    m_client.close();
    if (hasAccount())
        setState(State::Offline);
}

void Messenger::connectNow()
{
    if (!m_running || !hasAccount() || m_state == State::Connecting || m_state == State::Online)
        return;
    QString error;
    if (!m_client.open(m_account->server, &error)) {
        emit problem(error);
        return;
    }
    setState(State::Connecting);
}

void Messenger::onConnected()
{
    if (m_signUpDone) {
        m_client.registerAccount(m_account->username, m_account->identity,
                                 Protocol::publicPart(*m_keys->latestSignedPreKey()),
                                 Protocol::publicParts(m_signUpPreKeys), [this](const ChatClient::Result &result) {
                                     if (!m_signUpDone)
                                         return;
                                     QString why = result.message;
                                     if (result.error == QLatin1String("taken"))
                                         why = tr("That username is taken.");
                                     else if (result.error == QLatin1String("rate-limited"))
                                         why = tr("Too many sign-ups from here; try again later.");
                                     finishSignUp(result.ok, result.ok ? QString() : why);
                                 });
        return;
    }
    if (!hasAccount())
        return;
    m_client.login(m_account->username, m_account->device, m_account->identity,
                   [this](const ChatClient::Result &result) {
                       if (!result.ok) {
                           if (result.error == QLatin1String("denied")) {
                               m_running = false; // trying again would not help
                               emit problem(tr("The server refused this device's key."));
                           }
                           m_client.close();
                           return;
                       }
                       m_delay = m_firstDelay;
                       m_serverPreKeys = result.data.value(QStringLiteral("prekeys")).toInt();
                       setState(State::Online);
                       replenishPreKeys();
                       rotateSignedPreKey();
                       m_rotation.start();
                       flushOutbox();
                       downloadPending();
                   });
}

void Messenger::onConnectionLost(const QString &why)
{
    if (m_signUpDone) {
        finishSignUp(false, why);
        return;
    }
    if (m_state != State::Connecting && m_state != State::Online)
        return;
    m_inFlight.clear();
    m_downloading = false;
    m_replenishing = false;
    m_rotating = false;
    m_rotation.stop();
    setState(State::Offline);
    scheduleReconnect();
}

void Messenger::scheduleReconnect()
{
    if (!m_running || m_reconnect.isActive())
        return;
    m_reconnect.start(m_delay);
    m_delay = qMin(m_delay * 2, MaxReconnectDelayMs);
}

void Messenger::replenishPreKeys()
{
    if (m_state != State::Online || m_replenishing || m_serverPreKeys >= PreKeyLowWater)
        return;
    const QList<X3dh::OneTimePreKey> keys = X3dh::makeOneTimePreKeys(m_keys->takePreKeyIds(PreKeyBatch), PreKeyBatch);
    m_keys->addOneTimePreKeys(keys);
    m_replenishing = true;
    m_client.uploadPreKeys({}, Protocol::publicParts(keys), [this](const ChatClient::Result &result) {
        m_replenishing = false;
        if (result.ok)
            m_serverPreKeys = result.data.value(QStringLiteral("prekeys")).toInt();
    });
}

qint64 Messenger::now() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

void Messenger::rotateSignedPreKey()
{
    if (m_state != State::Online || m_rotating || !hasAccount())
        return;
    const qint64 time = now();
    QList<KeyStore::SignedPreKeyInfo> keys = m_keys->signedPreKeys();

    // Old ones: kept 30 days after the next one took over, then gone.
    for (int i = 0; i + 1 < keys.size(); ++i) {
        if (keys.at(i + 1).uploaded && time - keys.at(i + 1).created > SignedPreKeyGraceMs)
            m_keys->removeSignedPreKey(keys.at(i).id);
    }

    // The newest: made if it is time, uploaded if the server does not have it yet.
    std::optional<X3dh::SignedPreKey> latest = m_keys->latestSignedPreKey();
    const bool due = keys.isEmpty() || time - keys.last().created >= SignedPreKeyLifetimeMs;
    if (latest && !due && keys.last().uploaded)
        return;
    if (!latest || due) {
        latest = X3dh::makeSignedPreKey(m_account->identity, m_keys->takePreKeyIds(1));
        if (!m_keys->addSignedPreKey(*latest, time, false))
            return;
    }
    m_rotating = true;
    const quint32 id = latest->id;
    m_client.uploadPreKeys(Protocol::publicPart(*latest), {}, [this, id](const ChatClient::Result &result) {
        m_rotating = false;
        if (result.ok)
            m_keys->setSignedPreKeyUploaded(id);
        // Otherwise it goes again on the next check.
    });
}

// --- Conversations ---

void Messenger::startConversation(const QString &username,
                                  const std::function<void(qint64, const QString &)> &done)
{
    const QString name = Protocol::normalizeUsername(username);
    if (name.isEmpty()) {
        done(0, tr("That is not a username."));
        return;
    }
    if (name == this->username()) {
        done(m_store->noteToSelf(), QString());
        return;
    }
    if (m_store->isBlocked(name)) {
        done(0, tr("You have blocked %1. Unblock them first (File › Blocked People).").arg(name));
        return;
    }
    if (const qint64 existing = m_store->directConversation(name)) {
        acceptRequest(existing); // writing to them is accepting them
        done(existing, QString());
        return;
    }
    if (m_state != State::Online) {
        done(0, tr("Not connected to the server."));
        return;
    }
    fetchSessions(name, [this, name, done](bool ok, const QString &error) {
        if (!ok) {
            done(0, error == QLatin1String("rate-limited") ? tr("Too many new chats at once; try again later.")
                                                           : error);
            return;
        }
        const qint64 existing = m_store->directConversation(name);
        done(existing ? existing : m_store->addConversation(QStringLiteral("direct"), name, name), QString());
    });
}

void Messenger::fetchSessions(const QString &to, const Done &done)
{
    m_client.fetchKeys(to, [this, to, done](const ChatClient::Result &result) {
        if (result.error == QLatin1String("not-found")) {
            done(false, tr("There is nobody called %1.").arg(to));
            return;
        }
        if (result.error == QLatin1String("rate-limited")) {
            m_retryAfterMs = qint64(result.data.value(QStringLiteral("retryAfter")).toDouble());
            done(false, QStringLiteral("rate-limited"));
            return;
        }
        const auto bundles = ChatClient::bundles(result);
        if (!bundles || bundles->isEmpty()) {
            done(false, result.ok ? tr("The server's answer was not understood.") : result.message);
            return;
        }
        QList<quint32> devices;
        for (const ChatClient::DeviceBundle &bundle : *bundles) {
            devices.append(bundle.device);
            if (m_keys->session(to, bundle.device))
                continue;
            if (m_keys->checkIdentity(to, bundle.bundle.identityKey) == KeyStore::Trust::Changed)
                emit safetyNumberChanged(to);
            QString error;
            const auto session = Session::initiate(m_account->identity, bundle.bundle, &error);
            if (!session) {
                done(false, tr("Their keys are not right (%1).").arg(error));
                return;
            }
            m_keys->saveSession(to, bundle.device, *session);
        }
        // Devices they no longer have.
        for (quint32 device : m_keys->sessionDevices(to)) {
            if (!devices.contains(device))
                m_keys->removeSession(to, device);
        }
        done(true, QString());
    });
}

qint64 Messenger::send(qint64 conversation, const QString &text)
{
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.id == 0 || text.isEmpty())
        return 0;
    if (c.kind != QLatin1String("direct"))
        return m_store->addMessage(conversation, text).id;
    if (c.request)
        acceptRequest(conversation);
    const Protocol::Content content = Protocol::Content::text(text);
    const ChatStore::Message message =
        m_store->addMessage(conversation, text, QString(), QDateTime::fromMSecsSinceEpoch(content.sentAt, QTimeZone::utc()),
                            content.id, QStringLiteral("sending"));
    if (message.id != 0 && m_state == State::Online)
        deliver(message);
    return message.id;
}

void Messenger::flushOutbox()
{
    for (const ChatStore::Message &message : m_store->outbox())
        deliver(message);
}

void Messenger::deliver(const ChatStore::Message &message)
{
    if (m_inFlight.contains(message.id) || m_state != State::Online)
        return;
    const ChatStore::Conversation c = m_store->conversation(message.conversation);
    if (c.kind != QLatin1String("direct")) {
        m_store->setStatus(message.id, QStringLiteral("failed"));
        return;
    }
    const qint64 id = message.id;

    // Its attachments first, one at a time; then the message with their keys.
    const QList<ChatStore::StoredAttachment> attachments = m_store->attachments(id);
    for (const ChatStore::StoredAttachment &a : attachments) {
        if (a.state != QLatin1String("upload"))
            continue;
        const QByteArray blob = m_store->readBlob(a.file);
        if (blob.isEmpty()) {
            m_store->setAttachmentState(a.id, QStringLiteral("failed"));
            deliveryFailed(id, QStringLiteral("missing"), tr("The file to send is no longer here."));
            return;
        }
        m_inFlight.insert(id);
        const qint64 attachmentId = a.id;
        m_client.uploadAttachment(
            blob, [this, attachmentId](qint64 done, qint64 total) { emit attachmentProgress(attachmentId, done, total); },
            [this, id, attachmentId](const QString &remoteId, const ChatClient::Result &result) {
                m_inFlight.remove(id);
                if (!result.ok) {
                    if (result.error == QLatin1String("rate-limited"))
                        m_retryAfterMs = qint64(result.data.value(QStringLiteral("retryAfter")).toDouble());
                    deliveryFailed(id, result.error,
                                   result.error == QLatin1String("full") ? tr("The server has no room for files now.")
                                                                         : result.message);
                    return;
                }
                m_store->setAttachmentUploaded(attachmentId, remoteId);
                deliver(m_store->message(id)); // the next one, or the message
            });
        return;
    }

    Protocol::Content content;
    content.type = Protocol::Content::Type::Text;
    content.id = message.uid;
    content.sentAt = message.sentAt.toMSecsSinceEpoch();
    content.body = message.body;
    for (const ChatStore::StoredAttachment &a : attachments)
        content.attachments.append({a.remoteId, a.key, a.digest, a.size, a.name, a.type});
    if (Protocol::encodeContent(content).isEmpty()) {
        m_store->setStatus(id, QStringLiteral("failed"));
        return;
    }
    m_inFlight.insert(id);
    sendContent(c.peer, content, [this, id](bool ok, const QString &error) {
        m_inFlight.remove(id);
        if (ok) {
            if (m_store->message(id).status == QLatin1String("sending")) // a receipt may have come first
                m_store->setStatus(id, QStringLiteral("sent"));
        } else {
            deliveryFailed(id, error, error);
        }
    });
}

void Messenger::deliveryFailed(qint64 message, const QString &error, const QString &why)
{
    if (error == QLatin1String("disconnected"))
        return; // it stays in the outbox for next time
    if (error == QLatin1String("rate-limited")) {
        // Too many at once: the outbox goes again when the server says.
        if (!m_retry.isActive())
            m_retry.start(int(qBound(qint64(1000), m_retryAfterMs, qint64(10 * 60 * 1000))));
        return;
    }
    m_store->setStatus(message, QStringLiteral("failed"));
    emit problem(why.isEmpty() ? error : why);
}

qint64 Messenger::sendFile(qint64 conversation, const QString &path, const QString &caption, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return qint64(0);
    };
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.id == 0)
        return fail(tr("No such conversation."));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(tr("The file could not be read."));
    if (file.size() > Attachment::MaxFileBytes)
        return fail(tr("The file is larger than %1 MB.").arg(Attachment::MaxFileBytes / (1024 * 1024)));
    const QByteArray data = file.readAll();
    const auto sealed = Attachment::encrypt(data);
    if (!sealed)
        return fail(tr("The file could not be encrypted."));
    const QString blobFile = m_store->writeBlob(sealed->blob);
    if (blobFile.isEmpty())
        return fail(tr("The file could not be kept here."));
    QString type = QMimeDatabase().mimeTypeForFileNameAndData(path, data.left(4096)).name();
    if (!Attachment::isValidType(type))
        type = QStringLiteral("application/octet-stream");

    const bool direct = c.kind == QLatin1String("direct");
    if (direct && c.request)
        acceptRequest(conversation);
    const Protocol::Content content = Protocol::Content::text(caption);
    const ChatStore::Message message =
        m_store->addMessage(conversation, caption, QString(), QDateTime::fromMSecsSinceEpoch(content.sentAt, QTimeZone::utc()),
                            content.id, direct ? QStringLiteral("sending") : QStringLiteral("sent"));
    if (message.id == 0)
        return fail(tr("The message could not be kept."));
    ChatStore::StoredAttachment a;
    a.message = message.id;
    a.name = Attachment::safeName(QFileInfo(path).fileName());
    a.type = type;
    a.size = data.size();
    a.key = sealed->key;
    a.digest = sealed->digest;
    a.file = blobFile;
    a.state = direct ? QStringLiteral("upload") : QStringLiteral("ready"); // Note to Self: here only
    m_store->addAttachment(a);
    if (direct && m_state == State::Online)
        deliver(m_store->message(message.id));
    return message.id;
}

std::optional<QByteArray> Messenger::attachmentData(qint64 attachment) const
{
    const ChatStore::StoredAttachment a = m_store->attachment(attachment);
    if (a.id == 0 || a.file.isEmpty())
        return std::nullopt;
    return Attachment::decrypt(m_store->readBlob(a.file), a.key, a.digest, a.size);
}

bool Messenger::saveAttachment(qint64 attachment, const QString &path, QString *error) const
{
    const auto data = attachmentData(attachment);
    if (!data) {
        if (error)
            *error = tr("The file is not here.");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(*data) != data->size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

void Messenger::downloadPending()
{
    if (m_state != State::Online || m_downloading)
        return;
    const QList<ChatStore::StoredAttachment> pending = m_store->pendingDownloads();
    if (pending.isEmpty())
        return;
    const ChatStore::StoredAttachment a = pending.first();
    m_downloading = true;
    m_client.downloadAttachment(
        a.remoteId, Attachment::blobSize(a.size),
        [this, a](qint64 done, qint64 total) { emit attachmentProgress(a.id, done, total); },
        [this, a](const QByteArray &blob, const ChatClient::Result &result) {
            m_downloading = false;
            if (result.ok) {
                // Kept only if it is what the message promised.
                const QString file = Attachment::decrypt(blob, a.key, a.digest, a.size) ? m_store->writeBlob(blob)
                                                                                        : QString();
                if (!file.isEmpty())
                    m_store->setAttachmentFile(a.id, file);
                m_store->setAttachmentState(a.id, file.isEmpty() ? QStringLiteral("failed") : QStringLiteral("ready"));
            } else if (result.error == QLatin1String("disconnected")) {
                return; // on the next sign-in
            } else if (result.error == QLatin1String("rate-limited")) {
                QTimer::singleShot(int(qBound(qint64(1000), qint64(result.data.value(QStringLiteral("retryAfter")).toDouble()),
                                              qint64(10 * 60 * 1000))),
                                   this, &Messenger::downloadPending);
                return;
            } else {
                m_store->setAttachmentState(a.id, QStringLiteral("failed")); // expired, or never there
            }
            downloadPending(); // the next one
        });
}

void Messenger::sendContent(const QString &to, const Protocol::Content &content, const Done &done, bool retried)
{
    if (m_state != State::Online) {
        done(false, QStringLiteral("disconnected"));
        return;
    }
    const QList<quint32> devices = m_keys->sessionDevices(to);
    if (devices.isEmpty()) {
        if (retried) {
            done(false, tr("No way to reach %1.").arg(to));
            return;
        }
        fetchSessions(to, [this, to, content, done](bool ok, const QString &error) {
            if (ok)
                sendContent(to, content, done, true);
            else
                done(false, error);
        });
        return;
    }
    const QByteArray plaintext = Protocol::encodeContent(content);
    QHash<quint32, QByteArray> messages;
    for (quint32 device : devices) {
        std::optional<Session> session = m_keys->session(to, device);
        const QByteArray wire = session ? session->encrypt(plaintext) : QByteArray();
        if (wire.isEmpty()) {
            done(false, tr("The conversation's keys are broken."));
            return;
        }
        m_keys->saveSession(to, device, *session);
        messages.insert(device, wire);
    }
    m_client.send(to, messages, [this, to, content, done, retried](const ChatClient::Result &result) {
        if (result.ok) {
            done(true, QString());
        } else if (result.error == QLatin1String("devices") && !retried) {
            // Their devices changed: sessions for the new ones, then once more.
            fetchSessions(to, [this, to, content, done](bool ok, const QString &error) {
                if (ok)
                    sendContent(to, content, done, true);
                else
                    done(false, error);
            });
        } else if (result.error == QLatin1String("disconnected")) {
            done(false, result.error);
        } else if (result.error == QLatin1String("rate-limited")) {
            m_retryAfterMs = qint64(result.data.value(QStringLiteral("retryAfter")).toDouble());
            done(false, result.error);
        } else if (result.error == QLatin1String("not-found")) {
            done(false, tr("There is nobody called %1 any more.").arg(to));
        } else {
            done(false, result.message.isEmpty() ? result.error : result.message);
        }
    });
}

void Messenger::sendReceipt(const QString &to, Protocol::Content::Receipt kind, const QStringList &ids)
{
    for (int i = 0; i < ids.size(); i += Protocol::MaxReceiptIds) {
        const Protocol::Content receipt =
            Protocol::Content::receiptOf(kind, ids.mid(i, Protocol::MaxReceiptIds));
        sendContent(to, receipt, [](bool, const QString &) {});
    }
}

void Messenger::markRead(qint64 conversation)
{
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.request)
        return; // seen only once accepted: until then they learn nothing
    const QStringList ids = m_store->unseenUids(conversation);
    m_store->markRead(conversation);
    if (!ids.isEmpty() && c.kind == QLatin1String("direct") && m_state == State::Online)
        sendReceipt(c.peer, Protocol::Content::Receipt::Read, ids);
}

void Messenger::acceptRequest(qint64 conversation)
{
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (!c.request)
        return;
    m_store->setAccepted(conversation);
    markRead(conversation); // read covers delivered
    downloadPending();      // their files, now
}

void Messenger::deleteConversation(qint64 conversation)
{
    m_store->deleteConversation(conversation);
}

void Messenger::block(const QString &username)
{
    m_store->block(username);
    if (const qint64 conversation = m_store->directConversation(username)) {
        if (m_store->conversation(conversation).request)
            m_store->deleteConversation(conversation);
    }
}

void Messenger::unblock(const QString &username)
{
    m_store->unblock(username);
}

// --- Receiving ---

void Messenger::receive(const Protocol::Envelope &envelope)
{
    const auto message = Protocol::decodeCipherMessage(envelope.message);
    std::optional<QByteArray> plaintext;
    if (message && hasAccount() && envelope.from != m_account->username)
        plaintext = decrypt(envelope, *message);
    std::optional<Protocol::Content> content;
    if (plaintext)
        content = Protocol::decodeContent(*plaintext);
    if (content)
        handleContent(envelope.from, envelope, *content);
    // Kept (or not worth keeping): the server can forget it.
    m_client.acknowledge({envelope.id});
}

std::optional<QByteArray> Messenger::decrypt(const Protocol::Envelope &envelope,
                                             const Protocol::CipherMessage &message)
{
    std::optional<Session> session = m_keys->session(envelope.from, envelope.fromDevice);
    if (session) {
        auto plaintext = session->decrypt(message);
        if (plaintext) {
            m_keys->saveSession(envelope.from, envelope.fromDevice, *session);
            return plaintext;
        }
    }
    if (!message.preKey)
        return std::nullopt;
    // A new conversation from them (or they started again).
    auto accepted = Session::accept(
        m_account->identity, message, [this](quint32 id) { return m_keys->signedPreKey(id); },
        [this](quint32 id) { return m_keys->oneTimePreKey(id); });
    if (!accepted)
        return std::nullopt;
    if (m_keys->checkIdentity(envelope.from, accepted->session.remoteIdentityKey()) == KeyStore::Trust::Changed)
        emit safetyNumberChanged(envelope.from);
    m_keys->saveSession(envelope.from, envelope.fromDevice, accepted->session);
    if (accepted->usedOneTimePreKey) {
        m_keys->removeOneTimePreKey(*accepted->usedOneTimePreKey);
        --m_serverPreKeys;
        replenishPreKeys();
    }
    return accepted->plaintext;
}

void Messenger::handleContent(const QString &from, const Protocol::Envelope &envelope,
                              const Protocol::Content &content)
{
    if (m_store->isBlocked(from))
        return; // dropped, and they learn nothing
    qint64 conversation = m_store->directConversation(from);
    if (content.type == Protocol::Content::Type::Receipt) {
        if (conversation)
            m_store->advanceStatus(conversation, content.receiptFor,
                                   content.receipt == Protocol::Content::Receipt::Read ? QStringLiteral("read")
                                                                                       : QStringLiteral("delivered"));
        return;
    }
    // Someone new: a message request.
    if (!conversation)
        conversation = m_store->addConversation(QStringLiteral("direct"), from, from, true);
    if (conversation == 0 || m_store->hasMessage(conversation, content.id))
        return;
    // Their clock, unless it is far ahead of the server's.
    const qint64 sentAt = content.sentAt > envelope.serverTime + 5 * 60 * 1000 ? envelope.serverTime : content.sentAt;
    const ChatStore::Message message = m_store->addMessage(
        conversation, content.body, from, QDateTime::fromMSecsSinceEpoch(sentAt, QTimeZone::utc()), content.id);
    // Its attachments: fetched now, or once a request is accepted.
    for (const Attachment::Pointer &pointer : content.attachments) {
        ChatStore::StoredAttachment a;
        a.message = message.id;
        a.name = pointer.name;
        a.type = pointer.type;
        a.size = pointer.size;
        a.key = pointer.key;
        a.digest = pointer.digest;
        a.remoteId = pointer.id;
        a.state = QStringLiteral("download");
        m_store->addAttachment(a);
    }
    const bool request = m_store->conversation(conversation).request;
    if (!request) {
        sendReceipt(from, Protocol::Content::Receipt::Delivered, {content.id});
        downloadPending();
    }
    const QString shown = content.body.isEmpty() && !content.attachments.isEmpty()
                              ? QStringLiteral("📎 ") + content.attachments.first().name
                              : content.body;
    emit messageReceived(conversation, from, shown, request);
}

// --- Safety numbers ---

QString Messenger::safetyNumber(const QString &username) const
{
    const QByteArray theirs = m_keys->identityKey(username);
    if (!hasAccount() || theirs.isEmpty())
        return {};
    return SafetyNumber::compute(m_account->identity.publicKey(), m_account->username, theirs, username);
}

void Messenger::setVerified(const QString &username, bool verified)
{
    m_keys->setVerified(username, verified);
}
