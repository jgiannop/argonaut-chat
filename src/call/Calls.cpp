#include "Calls.h"

#include "CallAudio.h"
#include "CallConnection.h"
#include "CallDevices.h"
#include "Messenger.h"
#include "Voice.h"

using Call = Protocol::Call;

Calls::Calls(Messenger *messenger, QObject *parent)
    : QObject(parent)
    , m_messenger(messenger)
{
    connect(m_messenger, &Messenger::callSignal, this, &Calls::onSignal);
    m_ringTimer.setSingleShot(true);
    m_ringTimer.setInterval(RingMs);
    connect(&m_ringTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Calling) {
            sendHangup(Call::Reason::NoAnswer);
            finish(End::NoAnswer, tr("%1 did not answer.").arg(m_messenger->displayName(m_peer)));
        } else if (m_state == State::Ringing) {
            finish(End::Missed);
        }
    });
    m_connectTimer.setSingleShot(true);
    m_connectTimer.setInterval(ConnectMs);
    connect(&m_connectTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Connecting) {
            sendHangup(Call::Reason::Failed);
            finish(End::Failed, tr("The call could not connect."));
        }
    });
}

Calls::~Calls()
{
    if (m_state != State::Idle) {
        sendHangup(Call::Reason::Normal);
        finish(End::Hungup);
    }
}

qint64 Calls::activeMs() const
{
    return m_state == State::Active && m_activeClock.isValid() ? m_activeClock.elapsed() : 0;
}

void Calls::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

bool Calls::call(qint64 conversation, QString *error)
{
    if (m_state != State::Idle) {
        if (error)
            *error = tr("You are in a call already.");
        return false;
    }
    if (!m_messenger->canCall(conversation)) {
        if (error)
            *error = tr("Calls are only with people whose chats you have accepted.");
        return false;
    }
    if (m_messenger->state() != Messenger::State::Online) {
        if (error)
            *error = tr("Not connected to the server.");
        return false;
    }
    m_audio = std::make_unique<CallAudio>();
    m_audio->setProcessing(m_processing);
    if (!m_audio->isValid()) {
        m_audio.reset();
        if (error)
            *error = tr("Calls cannot be made with this build.");
        return false;
    }
    m_id = Protocol::newCallId();
    m_peer = m_messenger->callPeer(conversation);
    m_peerDevice = 0;
    m_incoming = false;
    m_wasActive = false;
    m_muted = false;
    setState(State::Calling);
    const QString id = m_id;
    connectWith([this, id](bool ok) {
        if (m_id != id || m_state != State::Calling)
            return;
        if (!ok) {
            finish(End::Failed, tr("The call could not start."));
            return;
        }
        connect(m_connection.get(), &CallConnection::described, this, [this, id](const QString &sdp) {
            if (m_id != id || m_state != State::Calling)
                return;
            Call offer;
            offer.kind = Call::Kind::Offer;
            offer.id = id;
            offer.sdp = sdp;
            if (!Protocol::isValidSdp(sdp)) {
                finish(End::Failed, tr("The call could not start."));
                return;
            }
            m_messenger->sendCall(m_peer, offer, [this, id](bool sent, const QString &why) {
                if (m_id != id || m_state != State::Calling || sent)
                    return;
                finish(End::Failed, why);
            });
            m_ringTimer.start();
        });
        m_connection->offer();
    });
    return true;
}

void Calls::connectWith(const std::function<void(bool ok)> &then)
{
    const QString id = m_id;
    m_messenger->fetchRelay([this, id, then](const Messenger::Relay &relay, const QString &) {
        if (m_id != id)
            return;
        // Without the relay (none, or it failed): direct only.
        m_connection = std::make_unique<CallConnection>(
            CallConnection::iceServers(relay.urls, relay.username, relay.credential), m_relayOnly);
        connect(m_connection.get(), &CallConnection::connected, this, &Calls::onConnected);
        connect(m_connection.get(), &CallConnection::failed, this, [this] {
            if (m_state == State::Idle)
                return;
            sendHangup(Call::Reason::Failed);
            finish(End::Failed, m_state == State::Active ? tr("The call was cut off.")
                                                         : tr("The call could not connect."));
        });
        connect(m_connection.get(), &CallConnection::audioReceived, this,
                [this](quint16 sequence, quint32, const QByteArray &packet) {
                    if (m_audio)
                        m_audio->received(sequence, packet);
                });
        then(true);
    });
}

void Calls::onConnected()
{
    if (m_state != State::Connecting)
        return;
    m_connectTimer.stop();
    m_wasActive = true;
    m_activeClock.start();
    if (m_devicesEnabled) {
        m_devices = std::make_unique<CallDevices>(m_audio.get());
        connect(m_devices.get(), &CallDevices::frameCaptured, this, &Calls::pushMicrophoneFrame);
        QString error;
        if (!m_devices->start(&error))
            emit problem(error);
    }
    setState(State::Active);
}

void Calls::pushMicrophoneFrame(const QByteArray &frame)
{
    if (m_state != State::Active || !m_audio || !m_connection)
        return;
    const QByteArray packet = m_audio->encode(frame, m_muted);
    m_timestamp += CallAudio::FrameSamples;
    if (!packet.isEmpty())
        m_connection->sendAudio(packet, m_timestamp);
}

void Calls::setMuted(bool muted)
{
    m_muted = muted;
}

void Calls::accept()
{
    if (m_state != State::Ringing)
        return;
    m_ringTimer.stop();
    setState(State::Connecting);
    m_connectTimer.start();
    const QString id = m_id;
    connectWith([this, id](bool) {
        if (m_id != id || m_state != State::Connecting)
            return;
        connect(m_connection.get(), &CallConnection::described, this, [this, id](const QString &sdp) {
            if (m_id != id || m_state == State::Idle)
                return;
            Call answer;
            answer.kind = Call::Kind::Answer;
            answer.id = id;
            answer.sdp = sdp;
            if (!Protocol::isValidSdp(sdp)) {
                sendHangup(Call::Reason::Failed);
                finish(End::Failed, tr("The call could not connect."));
                return;
            }
            send(answer);
            Call elsewhere;
            elsewhere.kind = Call::Kind::Hangup;
            elsewhere.id = id;
            elsewhere.reason = Call::Reason::AnsweredElsewhere;
            send(elsewhere);
        });
        if (!m_connection->answer(m_offerSdp)) {
            sendHangup(Call::Reason::Failed);
            finish(End::Failed, tr("The call could not connect."));
        }
    });
}

void Calls::decline()
{
    if (m_state != State::Ringing)
        return;
    sendHangup(Call::Reason::Declined);
    Call elsewhere;
    elsewhere.kind = Call::Kind::Hangup;
    elsewhere.id = m_id;
    elsewhere.reason = Call::Reason::DeclinedElsewhere;
    send(elsewhere);
    finish(End::Declined);
}

void Calls::hangUp()
{
    if (m_state == State::Idle)
        return;
    if (m_state == State::Ringing) {
        decline();
        return;
    }
    sendHangup(Call::Reason::Normal);
    finish(End::Hungup);
}

void Calls::send(const Call &call)
{
    m_messenger->sendCall(m_peer, call, [](bool, const QString &) {});
}

void Calls::sendHangup(Call::Reason reason)
{
    Call hangup;
    hangup.kind = Call::Kind::Hangup;
    hangup.id = m_id;
    hangup.reason = reason;
    send(hangup);
}

void Calls::onSignal(const QString &peer, quint32 device, const Call &call, qint64 age, bool mine)
{
    if (mine) {
        // Another device of mine answered or declined: stop ringing here.
        if (m_state == State::Ringing && call.id == m_id && peer == m_peer)
            finish(End::Elsewhere, call.reason == Call::Reason::AnsweredElsewhere
                                       ? tr("Answered on another device.")
                                       : QString());
        return;
    }
    if (call.kind == Call::Kind::Offer) {
        onIncoming(peer, device, call, age);
        return;
    }
    if (call.id != m_id || peer != m_peer || m_state == State::Idle)
        return; // not about this call (an old one's, or for another device of mine)
    // Once one of their devices has it, only that one speaks for them.
    if (m_peerDevice != 0 && device != m_peerDevice)
        return;
    switch (call.kind) {
    case Call::Kind::Offer:
        return;
    case Call::Kind::Answer:
        if (m_state != State::Calling || m_incoming)
            return;
        m_ringTimer.stop();
        m_peerDevice = device;
        if (!m_connection || !m_connection->accept(call.sdp)) {
            sendHangup(Call::Reason::Failed);
            finish(End::Failed, tr("The call could not connect."));
            return;
        }
        setState(State::Connecting);
        m_connectTimer.start();
        return;
    case Call::Kind::Busy:
        if (m_state == State::Calling)
            finish(End::Busy, tr("%1 is on another call.").arg(m_messenger->displayName(m_peer)));
        return;
    case Call::Kind::Hangup:
        if (m_state == State::Ringing) {
            finish(End::Missed); // they gave up before I answered
        } else if (m_state == State::Calling && call.reason == Call::Reason::Declined) {
            finish(End::Declined, tr("%1 declined the call.").arg(m_messenger->displayName(m_peer)));
        } else if (m_state == State::Calling && !m_incoming) {
            finish(End::TheyHungUp);
        } else {
            finish(End::TheyHungUp, call.reason == Call::Reason::Failed ? tr("The call was cut off.") : QString());
        }
        return;
    }
}

void Calls::onIncoming(const QString &peer, quint32 device, const Call &call, qint64 age)
{
    if (call.id == m_id)
        return; // again (a retry): already ringing
    if (age > MaxOfferAgeMs) {
        // Held by the server while this device was away: too late to ring.
        m_messenger->addCallNote(peer, tr("Missed call"));
        return;
    }
    if (m_state != State::Idle) {
        Call busy;
        busy.kind = Call::Kind::Busy;
        busy.id = call.id;
        m_messenger->sendCall(peer, busy, [](bool, const QString &) {});
        m_messenger->addCallNote(peer, tr("Missed call"));
        return;
    }
    auto audio = std::make_unique<CallAudio>();
    if (!audio->isValid())
        return;
    audio->setProcessing(m_processing);
    m_audio = std::move(audio);
    m_id = call.id;
    m_peer = peer;
    m_peerDevice = device;
    m_incoming = true;
    m_wasActive = false;
    m_muted = false;
    m_offerSdp = call.sdp;
    setState(State::Ringing);
    m_ringTimer.start();
    emit incomingCall(peer);
}

void Calls::finish(End how, const QString &why)
{
    if (m_state == State::Idle)
        return;
    const QString peer = m_peer;
    const qint64 lasted = m_wasActive && m_activeClock.isValid() ? m_activeClock.elapsed() : 0;
    m_ringTimer.stop();
    m_connectTimer.stop();
    m_devices.reset();
    m_connection.reset();
    m_audio.reset();
    m_activeClock.invalidate();

    QString note;
    if (m_wasActive) {
        note = (m_incoming ? tr("Incoming call · %1") : tr("Outgoing call · %1")).arg(Voice::formatDuration(lasted));
    } else if (m_incoming) {
        switch (how) {
        case End::Declined:
            note = tr("Declined call");
            break;
        case End::Elsewhere:
            note = why.isEmpty() ? tr("Declined call") : tr("Incoming call · answered on another device");
            break;
        case End::Hungup:
        case End::Failed:
            note = tr("Incoming call · not connected");
            break;
        default:
            note = tr("Missed call");
            break;
        }
    } else {
        switch (how) {
        case End::NoAnswer:
            note = tr("Outgoing call · no answer");
            break;
        case End::Declined:
            note = tr("Outgoing call · declined");
            break;
        case End::Busy:
            note = tr("Outgoing call · busy");
            break;
        case End::Failed:
            note = tr("Outgoing call · not connected");
            break;
        default:
            note = tr("Outgoing call · cancelled");
            break;
        }
    }
    m_messenger->addCallNote(peer, note);

    m_id.clear();
    m_peer.clear();
    m_peerDevice = 0;
    m_offerSdp.clear();
    m_wasActive = false;
    m_muted = false;
    setState(State::Idle);
    emit ended(peer, why);
}
