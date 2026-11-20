#include "Messenger.h"

#include "SafetyNumber.h"

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
    m_keys->addSignedPreKey(signedPreKey);
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
                       flushOutbox();
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
    m_replenishing = false;
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
    if (const qint64 existing = m_store->directConversation(name)) {
        done(existing, QString());
        return;
    }
    if (m_state != State::Online) {
        done(0, tr("Not connected to the server."));
        return;
    }
    fetchSessions(name, [this, name, done](bool ok, const QString &error) {
        if (!ok) {
            done(0, error);
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
    const Protocol::Content content = Protocol::Content::text(text);
    const ChatStore::Message message =
        m_store->addMessage(conversation, text, QString(), QDateTime::fromMSecsSinceEpoch(content.sentAt, Qt::UTC),
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
    if (m_inFlight.contains(message.id))
        return;
    const ChatStore::Conversation c = m_store->conversation(message.conversation);
    Protocol::Content content;
    content.type = Protocol::Content::Type::Text;
    content.id = message.uid;
    content.sentAt = message.sentAt.toMSecsSinceEpoch();
    content.body = message.body;
    if (c.kind != QLatin1String("direct") || Protocol::encodeContent(content).isEmpty()) {
        m_store->setStatus(message.id, QStringLiteral("failed"));
        return;
    }
    m_inFlight.insert(message.id);
    const qint64 id = message.id;
    sendContent(c.peer, content, [this, id](bool ok, const QString &error) {
        m_inFlight.remove(id);
        const QString status = m_store->message(id).status;
        if (ok) {
            if (status == QLatin1String("sending")) // a receipt may have come first
                m_store->setStatus(id, QStringLiteral("sent"));
        } else if (error != QLatin1String("disconnected")) {
            m_store->setStatus(id, QStringLiteral("failed"));
            emit problem(error);
        }
        // Disconnected: it stays in the outbox for next time.
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
    const QStringList ids = m_store->unseenUids(conversation);
    m_store->markRead(conversation);
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (!ids.isEmpty() && c.kind == QLatin1String("direct") && m_state == State::Online)
        sendReceipt(c.peer, Protocol::Content::Receipt::Read, ids);
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
    qint64 conversation = m_store->directConversation(from);
    if (content.type == Protocol::Content::Type::Receipt) {
        if (conversation)
            m_store->advanceStatus(conversation, content.receiptFor,
                                   content.receipt == Protocol::Content::Receipt::Read ? QStringLiteral("read")
                                                                                       : QStringLiteral("delivered"));
        return;
    }
    if (!conversation)
        conversation = m_store->addConversation(QStringLiteral("direct"), from, from);
    if (conversation == 0 || m_store->hasMessage(conversation, content.id))
        return;
    // Their clock, unless it is far ahead of the server's.
    const qint64 sentAt = content.sentAt > envelope.serverTime + 5 * 60 * 1000 ? envelope.serverTime : content.sentAt;
    m_store->addMessage(conversation, content.body, from, QDateTime::fromMSecsSinceEpoch(sentAt, Qt::UTC), content.id);
    sendReceipt(from, Protocol::Content::Receipt::Delivered, {content.id});
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
