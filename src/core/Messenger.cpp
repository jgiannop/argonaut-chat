#include "Messenger.h"

#include "Voice.h"

#include "Attachment.h"
#include "SafetyNumber.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QTimeZone>

#include <memory>

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

    m_expiry.setSingleShot(true);
    connect(&m_expiry, &QTimer::timeout, this, &Messenger::expireNow);
    QTimer::singleShot(0, this, &Messenger::expireNow); // what came due while closed
    m_rotation.setInterval(6 * 3600 * 1000);
    connect(&m_rotation, &QTimer::timeout, this, &Messenger::rotateSignedPreKey);
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, [this] {
        if (m_state == State::Online)
            flushOutbox();
    });
    m_typingExpiry.setInterval(1000);
    connect(&m_typingExpiry, &QTimer::timeout, this, &Messenger::expireTyping);
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &Messenger::connectNow);
    connect(&m_client, &ChatClient::connected, this, &Messenger::onConnected);
    connect(&m_client, &ChatClient::disconnected, this, [this] { onConnectionLost(tr("The connection was lost.")); });
    connect(&m_client, &ChatClient::failed, this, &Messenger::onConnectionLost);
    connect(&m_client, &ChatClient::envelopeReceived, this, &Messenger::receive);
    connect(&m_client, &ChatClient::provisionReceived, this, &Messenger::onProvision);
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
    if (m_linkDone) {
        // This device, to be linked: a slot, and the code to show.
        m_client.openProvisioning([this](const ChatClient::Result &result) {
            const QString slot = result.data.value(QStringLiteral("id")).toString();
            if (!m_linkDone)
                return;
            if (!result.ok || !Attachment::isValidId(slot)) {
                finishLinking(false, result.message.isEmpty() ? tr("The server did not open a slot.") : result.message);
                return;
            }
            if (m_linkCode)
                m_linkCode(encodeLinkCode(m_linkKey.publicKey, slot));
        });
        return;
    }
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
                       shareProfileWithAll();
                       fetchAvatars();
                       refreshMyDevices();
                   });
}

void Messenger::onConnectionLost(const QString &why)
{
    if (m_linkDone) {
        finishLinking(false, why);
        return;
    }
    if (m_signUpDone) {
        finishSignUp(false, why);
        return;
    }
    if (m_state != State::Connecting && m_state != State::Online)
        return;
    m_inFlight.clear();
    m_downloading = false;
    m_avatarUploading = false;
    m_avatarFetching = false;
    m_profileInFlight.clear();
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

qint64 Messenger::nextLocalTime()
{
    m_lastLocalTime = qMax(now(), m_lastLocalTime + 1);
    return m_lastLocalTime;
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
        shareProfile(name); // they see who writes
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
            if (to == m_account->username && bundle.device == m_account->device)
                continue; // this device itself
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

qint64 Messenger::send(qint64 conversation, const QString &text, qint64 replyTo)
{
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.id == 0 || text.isEmpty())
        return 0;
    // Only a message of this conversation can be answered.
    const auto quote = replyTo != 0 && m_store->message(replyTo).conversation == conversation ? quoteOf(replyTo)
                                                                                               : std::nullopt;
    if (c.kind == QLatin1String("self")) {
        // An id as the others' have, so a reply to it can find it.
        const qint64 id =
            m_store->addMessage(conversation, text, QString(), QDateTime::currentDateTimeUtc(), Protocol::newMessageId()).id;
        if (id != 0 && quote)
            m_store->setQuote(id, quote->id, quote->author, quote->text);
        if (id != 0 && c.expireSeconds > 0) {
            m_store->setMessageExpiry(id, c.expireSeconds, now());
            scheduleExpiry();
        }
        return id;
    }
    if (c.left)
        return 0; // not in that group any more
    if (c.request)
        acceptRequest(conversation);
    const Protocol::Content content = Protocol::Content::text(text);
    const ChatStore::Message message =
        m_store->addMessage(conversation, text, QString(), QDateTime::fromMSecsSinceEpoch(content.sentAt, QTimeZone::utc()),
                            content.id, QStringLiteral("sending"));
    if (message.id != 0 && quote)
        m_store->setQuote(message.id, quote->id, quote->author, quote->text);
    m_typingSentAt.remove(conversation); // the message says it: no "stopped" needed
    if (message.id != 0 && c.expireSeconds > 0) {
        m_store->setMessageExpiry(message.id, c.expireSeconds, now()); // mine: from now
        scheduleExpiry();
    }
    if (message.id != 0 && m_state == State::Online)
        deliver(m_store->message(message.id));
    return message.id;
}

std::optional<Protocol::Quote> Messenger::quoteOf(qint64 message) const
{
    const ChatStore::Message m = m_store->message(message);
    if (m.id == 0 || m.system || m.deleted)
        return std::nullopt;
    Protocol::Quote quote;
    quote.id = m.uid;
    // Mine by my username, as the others know me; a note to self's by mine too (or "me", with no account).
    quote.author = m.outgoing() ? (hasAccount() ? username() : QStringLiteral("me")) : m.author;
    QString text = m.body.simplified();
    if (text.isEmpty()) {
        const QList<ChatStore::StoredAttachment> files = m_store->attachments(m.id);
        if (!files.isEmpty())
            text = files.first().voice ? tr("🎤 Voice message") : QStringLiteral("📎 ") + files.first().name;
    }
    if (text.size() > Protocol::MaxQuoteChars)
        text = text.left(Protocol::MaxQuoteChars - 1) + QStringLiteral("…");
    quote.text = text;
    return quote;
}

void Messenger::flushOutbox()
{
    for (const ChatStore::Message &message : m_store->outbox())
        deliver(message);
    sendPendingReactions();
    sendPendingEdits();
    sendPendingDeletes();
}

// --- Typing ---

void Messenger::setTypingIndicators(bool on)
{
    if (on == m_typingIndicators)
        return;
    if (!on) {
        // Off: say so where I was typing, and show nobody's.
        for (const qint64 conversation : m_typingSentAt.keys())
            setTyping(conversation, false);
        const QList<qint64> shown = m_typing.keys();
        m_typing.clear();
        for (const qint64 conversation : shown)
            emit typingChanged(conversation);
    }
    m_typingIndicators = on;
}

void Messenger::setTyping(qint64 conversation, bool typing)
{
    if (!typing && !m_typingSentAt.contains(conversation))
        return;
    if (typing && (!m_typingIndicators || m_state != State::Online))
        return;
    if (typing && m_typingSentAt.contains(conversation) && now() - m_typingSentAt.value(conversation) < TypingRepeatMs)
        return; // said already
    const ChatStore::Conversation c = m_store->conversation(conversation);
    const bool group = c.kind == QLatin1String("group");
    if ((c.kind != QLatin1String("direct") && !group) || c.request || c.left)
        return;
    if (typing)
        m_typingSentAt.insert(conversation, now());
    else
        m_typingSentAt.remove(conversation);
    if (m_state != State::Online)
        return;
    const Protocol::Content content = Protocol::Content::typingNow(typing, group ? c.peer : QString());
    const Done ignore = [](bool, const QString &) {};
    if (group)
        fanOut(othersIn(c.id), content, ignore);
    else
        sendContent(c.peer, content, ignore);
}

QStringList Messenger::typingIn(qint64 conversation) const
{
    QStringList who = m_typing.value(conversation).keys();
    who.sort();
    return who;
}

void Messenger::expireTyping()
{
    const qint64 t = now();
    for (auto it = m_typing.begin(); it != m_typing.end();) {
        bool changed = false;
        for (auto person = it->begin(); person != it->end();) {
            if (*person <= t) {
                person = it->erase(person);
                changed = true;
            } else {
                ++person;
            }
        }
        const qint64 conversation = it.key();
        it = it->isEmpty() ? m_typing.erase(it) : std::next(it);
        if (changed)
            emit typingChanged(conversation);
    }
    if (m_typing.isEmpty())
        m_typingExpiry.stop();
}

void Messenger::stopShowingTyping(qint64 conversation, const QString &who)
{
    auto it = m_typing.find(conversation);
    if (it == m_typing.end() || !it->remove(who))
        return;
    if (it->isEmpty())
        m_typing.erase(it);
    emit typingChanged(conversation);
}

void Messenger::handleTyping(const QString &from, const Protocol::Envelope &envelope, const Protocol::Content &content)
{
    // Off, or late (a server before 0.2 queues it): not shown.
    if (!m_typingIndicators || qAbs(QDateTime::currentMSecsSinceEpoch() - envelope.serverTime) > 30000)
        return;
    qint64 conversation = 0;
    if (!content.group.isEmpty()) {
        conversation = m_store->groupConversation(content.group);
        if (conversation == 0 || !m_store->isMember(conversation, from))
            return;
    } else {
        conversation = m_store->directConversation(from);
    }
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (conversation == 0 || c.request || c.left)
        return; // nothing for a stranger, nor for a group I am out of
    if (!content.typing) {
        stopShowingTyping(conversation, from);
        return;
    }
    m_typing[conversation].insert(from, now() + TypingShownMs);
    if (!m_typingExpiry.isActive())
        m_typingExpiry.start();
    emit typingChanged(conversation);
}

// --- Editing ---

bool Messenger::canEdit(qint64 message) const
{
    const ChatStore::Message m = m_store->message(message);
    const ChatStore::Conversation c = m_store->conversation(m.conversation);
    if (m.id == 0 || !m.outgoing() || m.system || m.deleted)
        return false;
    if (c.kind == QLatin1String("self"))
        return true; // a note: whenever
    const bool sent = m.status == QLatin1String("sent") || m.status == QLatin1String("delivered")
                      || m.status == QLatin1String("read");
    return sent && !m.uid.isEmpty() && (c.kind == QLatin1String("direct") || c.kind == QLatin1String("group")) && !c.left
           && now() - m.sentAt.toMSecsSinceEpoch() <= EditMs;
}

bool Messenger::edit(qint64 message, const QString &text)
{
    const ChatStore::Message m = m_store->message(message);
    const bool hasFiles = !m_store->attachments(message).isEmpty();
    if (!canEdit(message) || text == m.body || (text.isEmpty() && !hasFiles)
        || text.toUtf8().size() > Protocol::MaxTextBytes)
        return false;
    const bool note = m_store->conversation(m.conversation).kind == QLatin1String("self");
    m_store->editMessage(message, text, qMax(nextLocalTime(), m.editedAt + 1), !note);
    sendPendingEdits();
    return true;
}

void Messenger::sendPendingEdits()
{
    if (m_state != State::Online)
        return;
    for (const qint64 id : m_store->pendingEdits()) {
        if (m_editsInFlight.contains(id))
            continue; // the newest goes once this one is done
        const ChatStore::Message m = m_store->message(id);
        const ChatStore::Conversation c = m_store->conversation(m.conversation);
        const bool group = c.kind == QLatin1String("group");
        if ((c.kind != QLatin1String("direct") && !group) || c.left) {
            m_store->setEditSent(id, m.editedAt);
            continue;
        }
        const Protocol::Content content = Protocol::Content::edit(m.uid, username(), m.body, group ? c.peer : QString());
        Protocol::Content copy = content;
        if (!group)
            copy.sentTo = c.peer;
        const qint64 at = m.editedAt;
        m_editsInFlight.insert(id);
        const Done done = [this, id, at, copy](bool ok, const QString &error) {
            m_editsInFlight.remove(id);
            if (!ok && (error == QLatin1String("disconnected") || error == QLatin1String("rate-limited")))
                return; // again with the outbox
            m_store->setEditSent(id, at);
            if (ok)
                syncToMyDevices(copy);
            sendPendingEdits(); // a newer one made meanwhile
        };
        if (group)
            fanOut(othersIn(c.id), content, done);
        else
            sendContent(c.peer, content, done);
    }
}

void Messenger::handleEdit(const QString &from, const Protocol::Envelope &envelope, const Protocol::Content &content,
                           bool mine)
{
    if (!content.quote || content.quote->author != from)
        return; // only one's own messages
    qint64 conversation = 0;
    if (!content.group.isEmpty()) {
        conversation = m_store->groupConversation(content.group);
        if (conversation == 0 || !m_store->isMember(conversation, from))
            return;
    } else {
        conversation = m_store->directConversation(mine ? content.sentTo : from);
    }
    const qint64 target = conversation ? m_store->messageByUid(conversation, content.quote->id) : 0;
    const ChatStore::Message m = m_store->message(target);
    // Theirs (mine, from another device of mine), not long ago by the server's
    // clock, and newer than the edit already here (they may come out of order).
    if (target == 0 || m.system || m.deleted || (mine ? !m.outgoing() : m.author != from)
        || envelope.serverTime - m.sentAt.toMSecsSinceEpoch() > 2 * EditMs || content.sentAt < m.editedAt
        || (content.body.isEmpty() && m_store->attachments(target).isEmpty()))
        return;
    m_store->editMessage(target, content.body, content.sentAt);
}

// --- Deleting ---

void Messenger::deleteForMe(qint64 message)
{
    m_store->deleteMessage(message);
}

bool Messenger::canDeleteForEveryone(qint64 message) const
{
    const ChatStore::Message m = m_store->message(message);
    const ChatStore::Conversation c = m_store->conversation(m.conversation);
    const bool sent = m.status == QLatin1String("sent") || m.status == QLatin1String("delivered")
                      || m.status == QLatin1String("read");
    return m.id != 0 && m.outgoing() && !m.system && !m.deleted && sent && !m.uid.isEmpty()
           && (c.kind == QLatin1String("direct") || c.kind == QLatin1String("group")) && !c.left
           && now() - m.sentAt.toMSecsSinceEpoch() <= DeleteForEveryoneMs;
}

bool Messenger::deleteForEveryone(qint64 message)
{
    if (!canDeleteForEveryone(message))
        return false;
    m_store->markDeleted(message, true);
    sendPendingDeletes();
    return true;
}

void Messenger::sendPendingDeletes()
{
    if (m_state != State::Online)
        return;
    for (const qint64 id : m_store->pendingDeletes()) {
        if (m_deletesInFlight.contains(id))
            continue;
        const ChatStore::Message m = m_store->message(id);
        const ChatStore::Conversation c = m_store->conversation(m.conversation);
        const bool group = c.kind == QLatin1String("group");
        if ((c.kind != QLatin1String("direct") && !group) || c.left) {
            m_store->setDeleteSent(id);
            continue;
        }
        const Protocol::Content content = Protocol::Content::deletion(m.uid, username(), group ? c.peer : QString());
        Protocol::Content copy = content;
        if (!group)
            copy.sentTo = c.peer;
        m_deletesInFlight.insert(id);
        const Done done = [this, id, copy](bool ok, const QString &error) {
            m_deletesInFlight.remove(id);
            if (!ok && (error == QLatin1String("disconnected") || error == QLatin1String("rate-limited")))
                return; // again with the outbox
            m_store->setDeleteSent(id);
            if (ok)
                syncToMyDevices(copy);
        };
        if (group)
            fanOut(othersIn(c.id), content, done);
        else
            sendContent(c.peer, content, done);
    }
}

void Messenger::handleDelete(const QString &from, const Protocol::Envelope &envelope, const Protocol::Content &content,
                             bool mine)
{
    if (!content.quote || content.quote->author != from)
        return; // only one's own messages
    qint64 conversation = 0;
    if (!content.group.isEmpty()) {
        conversation = m_store->groupConversation(content.group);
        if (conversation == 0 || !m_store->isMember(conversation, from))
            return;
    } else {
        conversation = m_store->directConversation(mine ? content.sentTo : from);
    }
    const qint64 target = conversation ? m_store->messageByUid(conversation, content.quote->id) : 0;
    const ChatStore::Message m = m_store->message(target);
    // Theirs (mine, from another device of mine), and not long ago by the server's clock.
    if (target == 0 || m.system || m.deleted || (mine ? !m.outgoing() : m.author != from)
        || envelope.serverTime - m.sentAt.toMSecsSinceEpoch() > 2 * DeleteForEveryoneMs)
        return;
    m_store->markDeleted(target);
}

// --- Reactions ---

QStringList Messenger::quickReactions()
{
    return {QStringLiteral("👍"), QStringLiteral("❤️"), QStringLiteral("😂"), QStringLiteral("😮"),
            QStringLiteral("😢"), QStringLiteral("🙏")};
}

bool Messenger::react(qint64 message, const QString &emoji)
{
    const ChatStore::Message m = m_store->message(message);
    const ChatStore::Conversation c = m_store->conversation(m.conversation);
    if (m.id == 0 || m.system || m.deleted || (!emoji.isEmpty() && !Protocol::isValidReaction(emoji)))
        return false;
    if (c.kind == QLatin1String("self")) {
        m_store->setReaction(message, QString(), emoji, now()); // here only
        return true;
    }
    if (c.request || c.left || m.uid.isEmpty())
        return false;
    if (emoji.isEmpty() && m_store->myReaction(message).isEmpty())
        return true; // nothing to take back
    m_store->setReaction(message, QString(), emoji, nextLocalTime(), true);
    sendPendingReactions();
    return true;
}

void Messenger::sendPendingReactions()
{
    if (m_state != State::Online)
        return;
    for (const ChatStore::Reaction &r : m_store->pendingReactions()) {
        if (m_reactionsInFlight.contains(r.message))
            continue; // the newest goes once this one is done
        const ChatStore::Message m = m_store->message(r.message);
        const ChatStore::Conversation c = m_store->conversation(m.conversation);
        const bool group = c.kind == QLatin1String("group");
        if (m.id == 0 || (c.kind != QLatin1String("direct") && !group) || c.left) {
            m_store->setReactionSent(r.message, r.at);
            continue;
        }
        Protocol::Content content = Protocol::Content::reaction(m.uid, m.outgoing() ? username() : m.author, r.emoji,
                                                                r.emoji.isEmpty(), group ? c.peer : QString());
        Protocol::Content copy = content; // to my other devices: which conversation
        if (!group)
            copy.sentTo = c.peer;
        const qint64 id = r.message;
        const qint64 at = r.at;
        m_reactionsInFlight.insert(id);
        const Done done = [this, id, at, copy](bool ok, const QString &error) {
            m_reactionsInFlight.remove(id);
            if (!ok && (error == QLatin1String("disconnected") || error == QLatin1String("rate-limited")))
                return; // again with the outbox
            m_store->setReactionSent(id, at); // sent, or never can be
            if (ok)
                syncToMyDevices(copy);
            sendPendingReactions(); // a newer one made meanwhile
        };
        if (group)
            fanOut(othersIn(c.id), content, done);
        else
            sendContent(c.peer, content, done);
    }
}

void Messenger::handleReaction(const QString &from, const Protocol::Content &content, bool mine)
{
    qint64 conversation = 0;
    if (!content.group.isEmpty()) {
        conversation = m_store->groupConversation(content.group);
        if (conversation == 0 || m_store->conversation(conversation).left || !m_store->isMember(conversation, from))
            return;
    } else {
        // Only on a conversation there already is: a reaction starts nothing.
        conversation = m_store->directConversation(mine ? content.sentTo : from);
    }
    if (conversation == 0 || !content.quote)
        return;
    const qint64 target = m_store->messageByUid(conversation, content.quote->id);
    const ChatStore::Message m = m_store->message(target);
    // The message must be the one named: its id and its author.
    if (target == 0 || m.system || m.deleted
        || (m.outgoing() ? content.quote->author != username() : m.author != content.quote->author))
        return;
    m_store->setReaction(target, mine ? QString() : from, content.remove ? QString() : content.emoji, now());
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (!mine && !content.remove && m.outgoing() && !c.request) {
        QString text = m.body.simplified();
        if (text.isEmpty()) {
            const auto quote = quoteOf(m.id);
            text = quote ? quote->text : QString();
        }
        emit reactionReceived(conversation, from, content.emoji, text);
    }
}

void Messenger::deliver(const ChatStore::Message &message)
{
    if (m_inFlight.contains(message.id) || m_state != State::Online)
        return;
    const ChatStore::Conversation c = m_store->conversation(message.conversation);
    const bool group = c.kind == QLatin1String("group");
    if ((c.kind != QLatin1String("direct") && !group) || c.left) {
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
    content.expireSeconds = message.expireSeconds;
    if (message.isReply() && Protocol::isValidMessageId(message.quoteUid))
        content.quote = Protocol::Quote{message.quoteUid, message.quoteAuthor, message.quoteText};
    for (const ChatStore::StoredAttachment &a : attachments)
        content.attachments.append({a.remoteId, a.key, a.digest, a.size, a.name, a.type, a.voice});
    if (Protocol::encodeContent(content).isEmpty()) {
        m_store->setStatus(id, QStringLiteral("failed"));
        return;
    }
    m_inFlight.insert(id);
    Protocol::Content copy = content; // for my other devices: whom it went to
    if (group)
        copy.group = c.peer;
    else
        copy.sentTo = c.peer;
    const Done finished = [this, id, copy](bool ok, const QString &error) {
        m_inFlight.remove(id);
        if (ok) {
            if (m_store->message(id).status == QLatin1String("sending")) // a receipt may have come first
                m_store->setStatus(id, QStringLiteral("sent"));
            syncToMyDevices(copy);
        } else {
            deliveryFailed(id, error, error);
        }
    };
    if (group) {
        content.group = c.peer;
        fanOut(othersIn(c.id), content, finished);
    } else {
        sendContent(c.peer, content, finished);
    }
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

qint64 Messenger::sendFile(qint64 conversation, const QString &path, const QString &caption, QString *error,
                           qint64 replyTo)
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
    return sendFileData(conversation, QFileInfo(path).fileName(), file.readAll(), caption, error, replyTo);
}

qint64 Messenger::sendFileData(qint64 conversation, const QString &fileName, const QByteArray &data, const QString &caption,
                               QString *error, qint64 replyTo)
{
    if (data.size() > Attachment::MaxFileBytes) {
        if (error)
            *error = tr("The file is larger than %1 MB.").arg(Attachment::MaxFileBytes / (1024 * 1024));
        return 0;
    }
    QString type = QMimeDatabase().mimeTypeForFileNameAndData(fileName, data.left(4096)).name();
    if (!Attachment::isValidType(type))
        type = QStringLiteral("application/octet-stream");
    return sendData(conversation, fileName, type, data, caption, error, replyTo, false);
}

qint64 Messenger::sendVoice(qint64 conversation, const QByteArray &samples, QString *error, qint64 replyTo)
{
    if (samples.size() < Voice::SampleRate / 5) { // under 0.1 s: a slip of the finger
        if (error)
            *error = tr("The recording is too short.");
        return 0;
    }
    const QString name = QStringLiteral("Voice message %1.wav")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss")));
    return sendData(conversation, name, QString::fromLatin1(Voice::Type), Voice::toWav(samples), QString(), error,
                    replyTo, true);
}

qint64 Messenger::sendData(qint64 conversation, const QString &fileName, const QString &type, const QByteArray &data,
                           const QString &caption, QString *error, qint64 replyTo, bool voice)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return qint64(0);
    };
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.id == 0)
        return fail(tr("No such conversation."));
    if (data.size() > Attachment::MaxFileBytes)
        return fail(tr("The file is larger than %1 MB.").arg(Attachment::MaxFileBytes / (1024 * 1024)));
    const auto sealed = Attachment::encrypt(data);
    if (!sealed)
        return fail(tr("The file could not be encrypted."));
    const QString blobFile = m_store->writeBlob(sealed->blob);
    if (blobFile.isEmpty())
        return fail(tr("The file could not be kept here."));

    const bool direct = c.kind != QLatin1String("self"); // a person or a group: it goes out
    if (c.left)
        return fail(tr("You are no longer in this group."));
    if (direct && c.request)
        acceptRequest(conversation);
    const Protocol::Content content = Protocol::Content::text(caption);
    const ChatStore::Message message =
        m_store->addMessage(conversation, caption, QString(), QDateTime::fromMSecsSinceEpoch(content.sentAt, QTimeZone::utc()),
                            content.id, direct ? QStringLiteral("sending") : QStringLiteral("sent"));
    if (message.id == 0)
        return fail(tr("The message could not be kept."));
    if (c.expireSeconds > 0) {
        m_store->setMessageExpiry(message.id, c.expireSeconds, now());
        scheduleExpiry();
    }
    ChatStore::StoredAttachment a;
    a.message = message.id;
    a.name = Attachment::safeName(fileName);
    a.type = type;
    a.voice = voice;
    a.size = data.size();
    a.key = sealed->key;
    a.digest = sealed->digest;
    a.file = blobFile;
    a.state = direct ? QStringLiteral("upload") : QStringLiteral("ready"); // Note to Self: here only
    m_store->addAttachment(a);
    if (replyTo != 0 && m_store->message(replyTo).conversation == conversation) {
        if (const auto quote = quoteOf(replyTo))
            m_store->setQuote(message.id, quote->id, quote->author, quote->text);
    }
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
    }, content.type == Protocol::Content::Type::Typing); // worthless later: never queued
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
    // Seen: disappearing ones start counting (after the marking below).
    QTimer::singleShot(0, this, [this, conversation] {
        m_store->startExpiry(conversation, now());
        scheduleExpiry();
    });
    if (c.kind == QLatin1String("group")) {
        const QHash<QString, QStringList> byAuthor = m_store->unseenUidsByAuthor(conversation);
        m_store->markRead(conversation);
        if (m_state == State::Online && !c.left) {
            QStringList all;
            for (auto it = byAuthor.cbegin(); it != byAuthor.cend(); ++it) {
                if (m_readReceipts)
                    sendReceipt(it.key(), Protocol::Content::Receipt::Read, it.value());
                all << it.value();
            }
            if (!all.isEmpty())
                syncToMyDevices(Protocol::Content::receiptOf(Protocol::Content::Receipt::Read, all));
        }
        return;
    }
    const QStringList ids = m_store->unseenUids(conversation);
    m_store->markRead(conversation);
    if (!ids.isEmpty() && c.kind == QLatin1String("direct") && m_state == State::Online) {
        if (m_readReceipts)
            sendReceipt(c.peer, Protocol::Content::Receipt::Read, ids);
        syncToMyDevices(Protocol::Content::receiptOf(Protocol::Content::Receipt::Read, ids)); // mine know anyway
    }
}

void Messenger::acceptRequest(qint64 conversation)
{
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (!c.request)
        return;
    m_store->setAccepted(conversation);
    markRead(conversation); // read covers delivered
    downloadPending();      // their files, now
    if (c.kind == QLatin1String("group")) {
        for (const QString &member : othersIn(conversation))
            shareProfile(member);
    } else {
        shareProfile(c.peer); // my profile, to them
    }
    fetchAvatars(); // and their pictures
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
    // From anyone, my other devices too; never from this one.
    const bool self = hasAccount() && envelope.from == m_account->username && envelope.fromDevice == m_account->device;
    if (message && hasAccount() && !self)
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
    // A device of mine has my identity key, or it is not one.
    if (envelope.from == m_account->username && accepted->session.remoteIdentityKey() != m_account->identity.publicKey())
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
    if (from == username()) {
        if (!m_myDevices.contains(envelope.fromDevice))
            m_myDevices << envelope.fromDevice; // a device of mine I had not heard of
        handleSync(envelope, content);
        return;
    }
    if (m_store->isBlocked(from))
        return; // dropped, and they learn nothing
    if (content.type == Protocol::Content::Type::Profile) {
        // Kept, even before a request is accepted (shown with their username);
        // the picture is fetched only once accepted.
        std::optional<Attachment::Pointer> avatar = content.avatar;
        if (avatar && avatar->size > MaxAvatarBytes)
            avatar.reset();
        m_store->setProfile(from, content.name, avatar);
        fetchAvatars();
        return;
    }
    if (content.type == Protocol::Content::Type::Group) {
        handleGroupUpdate(from, content);
        return;
    }
    if (content.type == Protocol::Content::Type::Leave) {
        handleLeave(from, content);
        return;
    }
    if (content.type == Protocol::Content::Type::Timer) {
        handleTimer(from, content);
        return;
    }
    if (content.type == Protocol::Content::Type::Reaction) {
        handleReaction(from, content, false);
        return;
    }
    if (content.type == Protocol::Content::Type::Delete) {
        handleDelete(from, envelope, content, false);
        return;
    }
    if (content.type == Protocol::Content::Type::Edit) {
        handleEdit(from, envelope, content, false);
        return;
    }
    if (content.type == Protocol::Content::Type::Typing) {
        handleTyping(from, envelope, content);
        return;
    }
    if (content.type == Protocol::Content::Type::Receipt) {
        // With mine off, theirs say no more than "delivered" (later turned on: not after the fact).
        const bool read = content.receipt == Protocol::Content::Receipt::Read && m_readReceipts;
        m_store->advanceStatusFrom(from, content.receiptFor, read ? QStringLiteral("read") : QStringLiteral("delivered"));
        return;
    }
    qint64 conversation = 0;
    if (!content.group.isEmpty()) {
        // A group's: only a group I am in, only from one of its members.
        conversation = m_store->groupConversation(content.group);
        const ChatStore::Conversation c = m_store->conversation(conversation);
        if (conversation == 0 || c.left || !m_store->isMember(conversation, from))
            return;
    } else {
        conversation = m_store->directConversation(from);
        // Someone new: a message request.
        if (!conversation)
            conversation = m_store->addConversation(QStringLiteral("direct"), from, from, true);
    }
    if (conversation == 0 || m_store->hasMessage(conversation, content.id))
        return;
    // Their clock, unless it is far ahead of the server's.
    const qint64 sentAt = content.sentAt > envelope.serverTime + 5 * 60 * 1000 ? envelope.serverTime : content.sentAt;
    const ChatStore::Message message = m_store->addMessage(
        conversation, content.body, from, QDateTime::fromMSecsSinceEpoch(sentAt, QTimeZone::utc()), content.id);
    if (content.quote)
        m_store->setQuote(message.id, content.quote->id, content.quote->author, content.quote->text);
    if (content.expireSeconds > 0)
        m_store->setMessageExpiry(message.id, content.expireSeconds, 0); // counts once seen
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
        a.voice = pointer.voice;
        m_store->addAttachment(a);
    }
    stopShowingTyping(conversation, from); // it came
    const bool request = m_store->conversation(conversation).request;
    if (!request) {
        sendReceipt(from, Protocol::Content::Receipt::Delivered, {content.id});
        downloadPending();
    }
    const QString shown = !content.body.isEmpty() || content.attachments.isEmpty() ? content.body
                          : content.attachments.first().voice ? tr("🎤 Voice message")
                                                              : QStringLiteral("📎 ") + content.attachments.first().name;
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

// --- Profiles ---

namespace {

// PNG or JPEG, by their first bytes; empty if neither.
QString imageType(const QByteArray &data)
{
    if (data.startsWith("\x89PNG\r\n\x1a\n"))
        return QStringLiteral("image/png");
    if (data.startsWith("\xff\xd8\xff"))
        return QStringLiteral("image/jpeg");
    return {};
}

constexpr qint64 kAvatarReuseMs = 25LL * 24 * 3600 * 1000; // the server keeps it 30 days

} // namespace

bool Messenger::setMyProfile(const QString &name, const QByteArray &avatar, QString *error)
{
    if (!avatar.isEmpty() && (avatar.size() > MaxAvatarBytes || imageType(avatar).isEmpty())) {
        if (error)
            *error = tr("The picture must be a PNG or JPEG of at most %1 KB.").arg(MaxAvatarBytes / 1024);
        return false;
    }
    m_store->setMyProfile(Protocol::cleanDisplayName(name), avatar);
    shareProfileWithAll();
    return true;
}

QString Messenger::displayName(const QString &username) const
{
    const QString name = m_store->profile(username).name;
    return name.isEmpty() ? username : name;
}

QStringList Messenger::knownPeople() const
{
    QStringList people;
    for (const ChatStore::Conversation &c : m_store->conversations()) {
        if (c.request || c.left)
            continue;
        if (c.kind == QLatin1String("direct"))
            people << c.peer;
        else if (c.kind == QLatin1String("group"))
            people << othersIn(c.id);
    }
    if (haveOtherDevices())
        people << username(); // my other devices: my profile
    people.removeDuplicates();
    return people;
}

void Messenger::shareProfileWithAll()
{
    for (const QString &peer : knownPeople())
        shareProfile(peer);
}

void Messenger::shareProfile(const QString &peer)
{
    const int version = m_store->myProfileVersion();
    if (m_state != State::Online || version == 0 || m_store->profileSentVersion(peer) >= version
        || m_profileInFlight.contains(peer) || m_store->isBlocked(peer))
        return;
    const ChatStore::Profile mine = m_store->myProfile();
    std::optional<Attachment::Pointer> pointer;
    if (!mine.avatar.isEmpty()) {
        pointer = m_store->myAvatarPointer(now() - kAvatarReuseMs);
        if (!pointer) {
            // The picture first, once for everyone; then the profile to each.
            if (m_avatarUploading)
                return;
            const auto sealed = Attachment::encrypt(mine.avatar);
            if (!sealed)
                return;
            m_avatarUploading = true;
            const Attachment::Pointer draft{QString(), sealed->key, sealed->digest, mine.avatar.size(),
                                            QStringLiteral("avatar"), imageType(mine.avatar)};
            m_client.uploadAttachment(sealed->blob, {},
                                      [this, draft, version](const QString &id, const ChatClient::Result &result) {
                                          m_avatarUploading = false;
                                          if (!result.ok)
                                              return; // tried again on the next sign-in
                                          if (m_store->myProfileVersion() == version) {
                                              Attachment::Pointer uploaded = draft;
                                              uploaded.id = id;
                                              m_store->setMyAvatarPointer(uploaded, now());
                                          }
                                          // Changed meanwhile or not: everyone gets the newest.
                                          shareProfileWithAll();
                                      });
            return;
        }
    }
    m_profileInFlight.insert(peer);
    sendContent(peer, Protocol::Content::profile(mine.name, pointer), [this, peer, version](bool ok, const QString &) {
        m_profileInFlight.remove(peer);
        if (!ok)
            return; // on the next sign-in
        m_store->setProfileSent(peer, version);
        // Changed while this one was on its way: the newer one now.
        if (m_store->myProfileVersion() > version)
            shareProfile(peer);
    });
}

void Messenger::fetchAvatars()
{
    if (m_state != State::Online || m_avatarFetching)
        return;
    for (const QString &person : knownPeople()) {
        const std::optional<Attachment::Pointer> pointer = m_store->pendingAvatar(person);
        if (!pointer)
            continue;
        m_avatarFetching = true;
        const QString peer = person;
        const Attachment::Pointer wanted = *pointer;
        m_client.downloadAttachment(
            wanted.id, Attachment::blobSize(wanted.size), {},
            [this, peer, wanted](const QByteArray &blob, const ChatClient::Result &result) {
                m_avatarFetching = false;
                if (result.error == QLatin1String("disconnected"))
                    return; // on the next sign-in
                // Their profile changed while this came (a newer picture, or none): this one is stale.
                const auto still = m_store->pendingAvatar(peer);
                if (still && still->id == wanted.id) {
                    const auto image = result.ok ? Attachment::decrypt(blob, wanted.key, wanted.digest, wanted.size)
                                                 : std::nullopt;
                    // Kept only if it is a picture; otherwise the old one stays.
                    m_store->setAvatar(peer,
                                       image && !imageType(*image).isEmpty() ? *image : m_store->profile(peer).avatar);
                }
                fetchAvatars(); // the next one
            });
        return;
    }
}

// --- Groups ---

QStringList Messenger::othersIn(qint64 conversation) const
{
    QStringList members = m_store->group(conversation).members;
    members.removeAll(username());
    return members;
}

void Messenger::fanOut(const QStringList &to, const Protocol::Content &content, const Done &done)
{
    if (to.isEmpty()) {
        done(true, QString());
        return;
    }
    struct Progress
    {
        int left = 0;
        QString again; // "disconnected" or "rate-limited": some must be tried again
    };
    auto progress = std::make_shared<Progress>();
    progress->left = int(to.size());
    for (const QString &member : to) {
        sendContent(member, content, [progress, done](bool ok, const QString &error) {
            if (!ok && (error == QLatin1String("disconnected") || error == QLatin1String("rate-limited")))
                progress->again = error;
            // Anything else (someone gone from the server): never reachable, not a reason to fail the rest.
            if (--progress->left == 0)
                done(progress->again.isEmpty(), progress->again);
        });
    }
}

void Messenger::checkPeople(const QStringList &people, const Done &done)
{
    if (people.isEmpty()) {
        done(true, QString());
        return;
    }
    const QString first = people.first();
    const QStringList rest = people.mid(1);
    if (!m_keys->sessionDevices(first).isEmpty()) {
        checkPeople(rest, done);
        return;
    }
    fetchSessions(first, [this, rest, done](bool ok, const QString &error) {
        if (!ok)
            done(false, error);
        else
            checkPeople(rest, done);
    });
}

void Messenger::sendGroupUpdate(const ChatStore::Group &group, const QStringList &to)
{
    const Protocol::Content update =
        Protocol::Content::groupUpdate(group.id, group.name, group.admin, group.members, group.revision);
    QStringList recipients = to;
    recipients.removeAll(username());
    fanOut(recipients, update, [](bool, const QString &) {});
    syncToMyDevices(update);
}

void Messenger::createGroup(const QString &name, const QStringList &members, const GroupDone &done)
{
    QStringList people;
    for (const QString &member : members) {
        const QString clean = Protocol::normalizeUsername(member);
        if (clean.isEmpty()) {
            done(0, tr("\"%1\" is not a username.").arg(member));
            return;
        }
        if (m_store->isBlocked(clean)) {
            done(0, tr("You have blocked %1.").arg(clean));
            return;
        }
        if (clean != username() && !people.contains(clean))
            people << clean;
    }
    if (people.isEmpty()) {
        done(0, tr("A group needs someone besides you."));
        return;
    }
    if (people.size() + 1 > Protocol::MaxGroupMembers) {
        done(0, tr("A group has at most %1 people.").arg(Protocol::MaxGroupMembers));
        return;
    }
    if (m_state != State::Online) {
        done(0, tr("Not connected to the server."));
        return;
    }
    const QString groupName = Protocol::cleanDisplayName(name);
    checkPeople(people, [this, people, groupName, done](bool ok, const QString &error) {
        if (!ok) {
            done(0, error);
            return;
        }
        ChatStore::Group group;
        group.id = Protocol::newGroupId();
        group.name = groupName.isEmpty() ? tr("Group") : groupName;
        group.admin = username();
        group.members = QStringList(people) << username();
        group.revision = 1;
        const qint64 conversation = m_store->addGroup(group, false);
        group.conversation = conversation;
        sendGroupUpdate(group, group.members);
        for (const QString &member : people)
            shareProfile(member);
        done(conversation, QString());
    });
}

void Messenger::changeGroup(qint64 conversation, const QString &name, const QStringList &members, const GroupDone &done)
{
    ChatStore::Group group = m_store->group(conversation);
    if (group.conversation == 0 || group.admin != username() || m_store->conversation(conversation).left) {
        done(0, tr("Only the group's admin can change it."));
        return;
    }
    QStringList people{username()};
    for (const QString &member : members) {
        const QString clean = Protocol::normalizeUsername(member);
        if (clean.isEmpty()) {
            done(0, tr("\"%1\" is not a username.").arg(member));
            return;
        }
        if (!people.contains(clean))
            people << clean;
    }
    if (people.size() > Protocol::MaxGroupMembers) {
        done(0, tr("A group has at most %1 people.").arg(Protocol::MaxGroupMembers));
        return;
    }
    if (m_state != State::Online) {
        done(0, tr("Not connected to the server."));
        return;
    }
    QStringList added;
    for (const QString &person : people) {
        if (!group.members.contains(person))
            added << person;
    }
    const QStringList before = group.members;
    const QString groupName = Protocol::cleanDisplayName(name);
    checkPeople(added, [this, group, people, before, added, groupName, done](bool ok, const QString &error) mutable {
        if (!ok) {
            done(0, error);
            return;
        }
        if (!groupName.isEmpty())
            group.name = groupName;
        group.members = people;
        ++group.revision;
        m_store->updateGroup(group);
        // To everyone in it, and to those taken out (so they know).
        QStringList to = before;
        for (const QString &person : people) {
            if (!to.contains(person))
                to << person;
        }
        sendGroupUpdate(group, to);
        for (const QString &person : added)
            shareProfile(person);
        done(group.conversation, QString());
    });
}

void Messenger::leaveGroup(qint64 conversation)
{
    const ChatStore::Group group = m_store->group(conversation);
    if (group.conversation == 0 || m_store->conversation(conversation).left)
        return;
    const Protocol::Content leave = Protocol::Content::leave(group.id);
    fanOut(othersIn(conversation), leave, [](bool, const QString &) {});
    syncToMyDevices(leave);
    m_store->setLeft(conversation);
}

void Messenger::handleGroupUpdate(const QString &from, const Protocol::Content &content)
{
    const qint64 conversation = m_store->groupConversation(content.group);
    ChatStore::Group group;
    group.id = content.group;
    group.name = content.groupName.isEmpty() ? tr("Group") : content.groupName;
    group.admin = content.groupAdmin;
    group.members = content.groupMembers;
    group.revision = content.groupRevision;
    const bool mine = content.groupMembers.contains(username());
    if (conversation == 0) {
        // A new group: from its admin, with me in it. From someone I have
        // not accepted, it is a request, as their own message would be.
        if (!mine || content.groupAdmin != from)
            return;
        const qint64 direct = m_store->directConversation(from);
        const bool known = from == username() || (direct != 0 && !m_store->conversation(direct).request);
        m_store->addGroup(group, !known);
        return;
    }
    // Only its admin, and only newer.
    const ChatStore::Group current = m_store->group(conversation);
    if (from != current.admin || content.groupRevision <= current.revision)
        return;
    group.conversation = conversation;
    m_store->updateGroup(group);
    if (!mine)
        m_store->setLeft(conversation);
    else if (m_store->conversation(conversation).left)
        m_store->setAccepted(conversation); // added back
    for (const QString &member : group.members)
        shareProfile(member); // new members get my profile (if accepted)
}

void Messenger::handleLeave(const QString &from, const Protocol::Content &content)
{
    const qint64 conversation = m_store->groupConversation(content.group);
    if (conversation == 0 || !m_store->isMember(conversation, from))
        return;
    ChatStore::Group group = m_store->group(conversation);
    group.members.removeAll(from);
    if (group.admin == username() && !m_store->conversation(conversation).left) {
        // I am the admin: the new list, to everyone.
        ++group.revision;
        m_store->updateGroup(group);
        sendGroupUpdate(group, group.members);
    } else {
        m_store->updateGroup(group); // the same revision: the admin's next update still counts
    }
}

// --- Linked devices ---

QString Messenger::encodeLinkCode(const QByteArray &publicKey, const QString &slot)
{
    const QByteArray raw = publicKey + QByteArray::fromHex(slot.toLatin1());
    return QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool Messenger::decodeLinkCode(const QString &code, QByteArray *publicKey, QString *slot)
{
    const QString trimmed = code.simplified().remove(QLatin1Char(' '));
    auto decoded = QByteArray::fromBase64Encoding(trimmed.toLatin1(), QByteArray::Base64UrlEncoding
                                                                          | QByteArray::OmitTrailingEquals
                                                                          | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() != Crypto::KeySize + 16)
        return false;
    *publicKey = decoded.decoded.left(Crypto::KeySize);
    *slot = QString::fromLatin1(decoded.decoded.mid(Crypto::KeySize).toHex());
    return true;
}

void Messenger::linkThisDevice(const QUrl &server, const LinkCode &showCode, const Done &done)
{
    if (hasAccount() || m_signUpDone || m_linkDone) {
        done(false, tr("There is an account already."));
        return;
    }
    m_client.close();
    m_keys->forgetAccount();
    m_linkKey = Crypto::DhKeyPair::generate(); // one time: only for this link
    m_linkCode = showCode;
    m_linkDone = done;
    QString error;
    // The server for the account's keys; this device's own entry comes once linked.
    m_account = KeyStore::Account{QString(), 0, server, Crypto::IdentityKeyPair(), false};
    if (!m_client.open(server, &error)) {
        finishLinking(false, error);
        return;
    }
    setState(State::Connecting);
}

void Messenger::cancelLinking()
{
    if (m_linkDone)
        finishLinking(false, tr("Cancelled."));
}

void Messenger::finishLinking(bool ok, const QString &error)
{
    const Done done = std::exchange(m_linkDone, {});
    m_linkCode = {};
    m_linkKey = Crypto::DhKeyPair();
    if (!ok) {
        m_keys->forgetAccount();
        m_account.reset();
        setState(State::NoAccount);
        m_client.close();
    } else {
        m_running = true;
        m_delay = m_firstDelay;
        setState(State::Online);
        refreshMyDevices();
    }
    if (done)
        done(ok, error);
}

void Messenger::onProvision(const QByteArray &sealed)
{
    if (!m_linkDone)
        return;
    // Only what was sealed to this device's one-time key: the server cannot make one.
    const auto opened = Crypto::openSealed(m_linkKey, sealed);
    const QJsonObject json = opened ? QJsonDocument::fromJson(*opened).object() : QJsonObject();
    const QString name = json.value(QStringLiteral("username")).toString();
    const auto secret = Protocol::Json::bytes(json.value(QStringLiteral("identity")), 64);
    const Crypto::IdentityKeyPair identity = secret ? Crypto::IdentityKeyPair::fromSecretKey(*secret)
                                                    : Crypto::IdentityKeyPair();
    if (!opened || json.value(QStringLiteral("v")).toInt() != 1 || !Protocol::isValidUsername(name)
        || !identity.isValid()) {
        finishLinking(false, tr("What came was not an account."));
        return;
    }
    const QString profileName = Protocol::cleanDisplayName(json.value(QStringLiteral("name")).toString());
    const auto avatar = Protocol::Json::bytes(json.value(QStringLiteral("avatar")), -1, int(MaxAvatarBytes));

    // Kept before the server hears of it, as at sign-up.
    KeyStore::Account account{name, 0, m_account->server, identity, false};
    m_keys->saveAccount(account);
    const X3dh::SignedPreKey signedPreKey = X3dh::makeSignedPreKey(identity, m_keys->takePreKeyIds(1));
    const QList<X3dh::OneTimePreKey> preKeys = X3dh::makeOneTimePreKeys(m_keys->takePreKeyIds(PreKeyBatch), PreKeyBatch);
    m_keys->addSignedPreKey(signedPreKey, now(), true);
    m_keys->addOneTimePreKeys(preKeys);
    m_account = account;
    const QJsonObject state = json;
    m_client.addDevice(name, identity, Protocol::publicPart(signedPreKey), Protocol::publicParts(preKeys),
                       [this, account, profileName, avatar, state](const ChatClient::Result &result) {
                           const auto device = Protocol::Json::integer(result.data.value(QStringLiteral("device")), 2,
                                                                       std::numeric_limits<quint32>::max());
                           if (!result.ok || !device) {
                               finishLinking(false, result.error == QLatin1String("too-many-devices")
                                                        ? tr("That account has as many devices as it may.")
                                                        : result.message);
                               return;
                           }
                           KeyStore::Account linked = account;
                           linked.device = quint32(*device);
                           linked.registered = true;
                           m_keys->saveAccount(linked);
                           m_account = linked;
                           m_serverPreKeys = PreKeyBatch;
                           if (!profileName.isEmpty() || avatar) {
                               m_store->setMyProfile(profileName, avatar.value_or(QByteArray()));
                               m_store->setProfileSent(username(), m_store->myProfileVersion()); // no echo
                           }
                           takeLinkedState(state);
                           finishLinking(true, QString());
                       });
}

void Messenger::linkDevice(const QString &code, const Done &done)
{
    QByteArray key;
    QString slot;
    if (!decodeLinkCode(code, &key, &slot)) {
        done(false, tr("That is not a link code."));
        return;
    }
    if (!hasAccount() || m_state != State::Online) {
        done(false, tr("Not connected to the server."));
        return;
    }
    const ChatStore::Profile mine = m_store->myProfile();
    QJsonObject json{{QStringLiteral("v"), 1},
                     {QStringLiteral("username"), username()},
                     {QStringLiteral("identity"), Protocol::Json::base64(m_account->identity.secretKey())},
                     {QStringLiteral("name"), mine.name}};
    if (!mine.avatar.isEmpty())
        json.insert(QStringLiteral("avatar"), Protocol::Json::base64(mine.avatar));
    // Whom I have accepted, my groups, whom I blocked: the same trust there
    // (but none of the messages).
    QJsonArray contacts;
    QJsonArray groups;
    for (const ChatStore::Conversation &c : m_store->conversations()) {
        if (c.request || c.left)
            continue;
        if (c.kind == QLatin1String("direct")) {
            contacts.append(c.peer);
        } else if (c.kind == QLatin1String("group")) {
            const ChatStore::Group g = m_store->group(c.id);
            groups.append(QJsonObject{{QStringLiteral("id"), g.id},
                                      {QStringLiteral("name"), g.name},
                                      {QStringLiteral("admin"), g.admin},
                                      {QStringLiteral("members"), QJsonArray::fromStringList(g.members)},
                                      {QStringLiteral("revision"), g.revision}});
        }
    }
    json.insert(QStringLiteral("contacts"), contacts);
    json.insert(QStringLiteral("groups"), groups);
    json.insert(QStringLiteral("blocked"), QJsonArray::fromStringList(m_store->blocked()));
    QByteArray plain = QJsonDocument(json).toJson(QJsonDocument::Compact);
    const QByteArray sealed = Crypto::sealTo(key, plain);
    Crypto::wipe(plain);
    if (sealed.isEmpty()) {
        done(false, tr("That is not a link code."));
        return;
    }
    m_client.sendProvisioning(slot, sealed, [this, done](const ChatClient::Result &result) {
        if (!result.ok) {
            done(false, result.error == QLatin1String("not-found")
                            ? tr("No computer is waiting with that code (it lasts ten minutes).")
                            : result.message);
            return;
        }
        // The new one joins in a moment: then it is one of mine.
        QTimer::singleShot(2000, this, &Messenger::refreshMyDevices);
        done(true, QString());
    });
}

void Messenger::listDevices(const DevicesDone &done)
{
    if (m_state != State::Online) {
        done({}, tr("Not connected to the server."));
        return;
    }
    m_client.listDevices([done](const ChatClient::Result &result) {
        QList<quint32> devices;
        const QJsonValue list = result.data.value(QStringLiteral("devices"));
        if (!result.ok || !list.isArray()) {
            done({}, result.ok ? tr("A bad answer.") : result.message);
            return;
        }
        for (const QJsonValue &item : list.toArray()) {
            const auto device = Protocol::Json::integer(item.toObject().value(QStringLiteral("device")), 1, 1000);
            if (device)
                devices << quint32(*device);
        }
        done(devices, QString());
    });
}

void Messenger::removeDevice(quint32 device, const Done &done)
{
    if (m_state != State::Online) {
        done(false, tr("Not connected to the server."));
        return;
    }
    m_client.removeDevice(device, [this, device, done](const ChatClient::Result &result) {
        if (result.ok) {
            m_keys->removeSession(username(), device);
            m_myDevices.removeAll(device);
        }
        done(result.ok, result.message);
    });
}

void Messenger::refreshMyDevices()
{
    listDevices([this](const QList<quint32> &devices, const QString &error) {
        if (error.isEmpty())
            m_myDevices = devices;
    });
}

bool Messenger::haveOtherDevices() const
{
    for (quint32 device : m_myDevices) {
        if (m_account && device != m_account->device)
            return true;
    }
    return false;
}

void Messenger::syncToMyDevices(const Protocol::Content &content)
{
    if (!haveOtherDevices() || m_state != State::Online)
        return;
    sendContent(username(), content, [](bool, const QString &) {});
}

void Messenger::handleSync(const Protocol::Envelope &envelope, const Protocol::Content &content)
{
    switch (content.type) {
    case Protocol::Content::Type::Text: {
        // Mine, sent from another of my devices: shown here as mine.
        qint64 conversation = 0;
        if (!content.group.isEmpty()) {
            conversation = m_store->groupConversation(content.group);
            if (conversation == 0 || !m_store->isMember(conversation, username()))
                return;
        } else if (!content.sentTo.isEmpty()) {
            conversation = m_store->directConversation(content.sentTo);
            if (conversation == 0)
                conversation = m_store->addConversation(QStringLiteral("direct"), content.sentTo, content.sentTo);
            else
                m_store->setAccepted(conversation); // I wrote to them
        }
        if (conversation == 0 || m_store->hasMessage(conversation, content.id))
            return;
        const ChatStore::Message message = m_store->addMessage(
            conversation, content.body, QString(), QDateTime::fromMSecsSinceEpoch(content.sentAt, QTimeZone::utc()),
            content.id, QStringLiteral("sent"));
        if (content.quote)
            m_store->setQuote(message.id, content.quote->id, content.quote->author, content.quote->text);
        if (content.expireSeconds > 0) {
            m_store->setMessageExpiry(message.id, content.expireSeconds, content.sentAt);
            scheduleExpiry();
        }
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
            a.voice = pointer.voice;
            m_store->addAttachment(a);
        }
        downloadPending();
        return;
    }
    case Protocol::Content::Type::Receipt:
        // Read on another of my devices.
        if (content.receipt == Protocol::Content::Receipt::Read) {
            m_store->markSeenUids(content.receiptFor);
            m_store->startExpiryForUids(content.receiptFor, now());
            scheduleExpiry();
        }
        return;
    case Protocol::Content::Type::Profile:
        applyMyProfile(content.name, content.avatar);
        return;
    case Protocol::Content::Type::Group:
        handleGroupUpdate(username(), content); // I am its admin, on another device
        return;
    case Protocol::Content::Type::Leave:
        if (const qint64 conversation = m_store->groupConversation(content.group))
            m_store->setLeft(conversation);
        return;
    case Protocol::Content::Type::Timer:
        handleTimer(username(), content); // set on another device of mine
        return;
    case Protocol::Content::Type::Reaction:
        handleReaction(username(), content, true); // made on another device of mine
        return;
    case Protocol::Content::Type::Delete:
        handleDelete(username(), envelope, content, true); // deleted on another device of mine
        return;
    case Protocol::Content::Type::Edit:
        handleEdit(username(), envelope, content, true); // edited on another device of mine
        return;
    case Protocol::Content::Type::Typing:
        return; // my own typing, elsewhere: nothing to show
    }
}

void Messenger::applyMyProfile(const QString &name, const std::optional<Attachment::Pointer> &avatar)
{
    const auto store = [this, name](const QByteArray &picture) {
        m_store->setMyProfile(name, picture);
        // Already everywhere: not sent back to my devices (no echo).
        m_store->setProfileSent(username(), m_store->myProfileVersion());
    };
    if (!avatar || avatar->size > MaxAvatarBytes) {
        store(QByteArray());
        return;
    }
    const Attachment::Pointer wanted = *avatar;
    m_client.downloadAttachment(wanted.id, Attachment::blobSize(wanted.size), {},
                                [this, wanted, store](const QByteArray &blob, const ChatClient::Result &result) {
                                    const auto image = result.ok ? Attachment::decrypt(blob, wanted.key, wanted.digest,
                                                                                       wanted.size)
                                                                 : std::nullopt;
                                    store(image && !imageType(*image).isEmpty() ? *image : m_store->myProfile().avatar);
                                });
}

void Messenger::takeLinkedState(const QJsonObject &json)
{
    // From my own device, sealed to this one; still checked like anything else.
    for (const QJsonValue &value : json.value(QStringLiteral("contacts")).toArray()) {
        const QString peer = value.toString();
        if (Protocol::isValidUsername(peer) && peer != username() && m_store->directConversation(peer) == 0)
            m_store->addConversation(QStringLiteral("direct"), peer, peer);
    }
    for (const QJsonValue &value : json.value(QStringLiteral("blocked")).toArray()) {
        if (Protocol::isValidUsername(value.toString()))
            m_store->block(value.toString());
    }
    int count = 0;
    for (const QJsonValue &value : json.value(QStringLiteral("groups")).toArray()) {
        if (++count > 1000)
            break;
        const QJsonObject g = value.toObject();
        // The same checks as a group update from its admin.
        Protocol::Content update = Protocol::Content::groupUpdate(
            g.value(QStringLiteral("id")).toString(), g.value(QStringLiteral("name")).toString(),
            g.value(QStringLiteral("admin")).toString(),
            [&] {
                QStringList members;
                for (const QJsonValue &m : g.value(QStringLiteral("members")).toArray())
                    members << m.toString();
                return members;
            }(),
            g.value(QStringLiteral("revision")).toInt());
        const auto checked = Protocol::decodeContent(Protocol::encodeContent(update));
        if (!checked || !checked->groupMembers.contains(username())
            || m_store->groupConversation(checked->group) != 0)
            continue;
        ChatStore::Group group;
        group.id = checked->group;
        group.name = checked->groupName;
        group.admin = checked->groupAdmin;
        group.members = checked->groupMembers;
        group.revision = checked->groupRevision;
        m_store->addGroup(group, false);
    }
}

// --- Disappearing messages ---

QString Messenger::describeTimer(qint64 seconds)
{
    if (seconds <= 0)
        return tr("off");
    struct Unit
    {
        qint64 seconds;
        const char *one;
        const char *many;
    };
    static const Unit units[] = {{7 * 24 * 3600, "1 week", "%1 weeks"}, {24 * 3600, "1 day", "%1 days"},
                                 {3600, "1 hour", "%1 hours"},          {60, "1 minute", "%1 minutes"},
                                 {1, "1 second", "%1 seconds"}};
    for (const Unit &unit : units) {
        if (seconds % unit.seconds == 0) {
            const qint64 count = seconds / unit.seconds;
            return count == 1 ? tr(unit.one) : tr(unit.many).arg(count);
        }
    }
    return tr("%1 seconds").arg(seconds);
}

// The notes carry real time, as the messages do (the clock `now()` is for
// the countdowns, which tests move on).
void Messenger::setDisappearing(qint64 conversation, qint64 seconds)
{
    const ChatStore::Conversation c = m_store->conversation(conversation);
    seconds = qBound(qint64(0), seconds, Protocol::MaxExpireSeconds);
    if (c.id == 0 || c.left || c.expireSeconds == seconds)
        return;
    m_store->setExpireSeconds(conversation, seconds);
    m_store->addSystemNote(conversation, tr("You set disappearing messages to %1.").arg(describeTimer(seconds)),
                           QDateTime::currentDateTimeUtc());
    Protocol::Content timer = Protocol::Content::timer(seconds);
    Protocol::Content copy = timer;
    if (c.kind == QLatin1String("direct")) {
        sendContent(c.peer, timer, [](bool, const QString &) {});
        copy.sentTo = c.peer;
    } else if (c.kind == QLatin1String("group")) {
        timer.group = c.peer;
        copy.group = c.peer;
        fanOut(othersIn(conversation), timer, [](bool, const QString &) {});
    } else {
        return; // Note to Self: here only
    }
    syncToMyDevices(copy);
}

void Messenger::handleTimer(const QString &from, const Protocol::Content &content)
{
    qint64 conversation = 0;
    const bool mine = from == username();
    if (!content.group.isEmpty()) {
        conversation = m_store->groupConversation(content.group);
        if (conversation == 0 || m_store->conversation(conversation).left || !m_store->isMember(conversation, from))
            return;
    } else {
        // Theirs to me, or mine (another device of mine) to them.
        conversation = m_store->directConversation(mine ? content.sentTo : from);
        if (conversation == 0)
            return; // no conversation: nothing to set (it does not start one)
    }
    if (m_store->conversation(conversation).expireSeconds == content.expireSeconds)
        return;
    m_store->setExpireSeconds(conversation, content.expireSeconds);
    const QString who = mine ? tr("You") : displayName(from);
    m_store->addSystemNote(conversation,
                           tr("%1 set disappearing messages to %2.").arg(who, describeTimer(content.expireSeconds)),
                           QDateTime::currentDateTimeUtc());
}

void Messenger::expireNow()
{
    m_store->deleteExpired(now());
    scheduleExpiry();
}

void Messenger::scheduleExpiry()
{
    const qint64 next = m_store->nextExpiry();
    if (next == 0) {
        m_expiry.stop();
        return;
    }
    // At most a day ahead (a timer's int); it looks again then.
    m_expiry.start(int(qBound(qint64(0), next - now(), qint64(24 * 3600 * 1000))));
}
