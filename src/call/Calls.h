#pragma once

#include "Protocol.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <memory>

class CallAudio;
class CallConnection;
class CallDevices;
class Messenger;

// Voice calls, as Signal's, one at a time, only with people in accepted
// direct conversations (a stranger's request never rings; see
// Messenger::canCall). The offer goes to every device of theirs; the first
// that answers has the call, and tells its other devices to stop ringing.
// Signalling inside the end-to-end encrypted messages (Messenger::sendCall);
// sound through CallConnection (DTLS-SRTP, direct or through our relay).
// A note in the conversation says how it went ("Missed call",
// "Outgoing call · 2:31").
class Calls : public QObject
{
    Q_OBJECT

public:
    enum class State { Idle, Calling, Ringing, Connecting, Active };
    static constexpr int RingMs = 60000;        // unanswered: given up
    static constexpr int MaxOfferAgeMs = 45000; // older (held by the server): missed
    static constexpr int ConnectMs = 30000;     // answered but no connection

    explicit Calls(Messenger *messenger, QObject *parent = nullptr);
    ~Calls() override;

    State state() const { return m_state; }
    bool isIdle() const { return m_state == State::Idle; }
    bool isIncoming() const { return m_incoming; }
    QString peer() const { return m_peer; }
    QString callId() const { return m_id; }
    qint64 activeMs() const; // 0 until connected
    bool isMuted() const { return m_muted; }

    bool call(qint64 conversation, QString *error = nullptr);
    void accept();
    void decline();
    void hangUp();
    void setMuted(bool muted);

    // Tests: no microphone or speaker; frames go in through
    // pushMicrophoneFrame and come out of audio()->nextFrame().
    void setDevicesEnabled(bool enabled) { m_devicesEnabled = enabled; }
    void pushMicrophoneFrame(const QByteArray &frame);
    CallAudio *audio() const { return m_audio.get(); }
    void setRingMsForTests(int ms) { m_ringTimer.setInterval(ms); }
    void setProcessingForTests(bool on) { m_processing = on; }
    void setRelayOnlyForTests(bool on) { m_relayOnly = on; }

signals:
    void stateChanged(Calls::State state);
    void incomingCall(const QString &peer);
    // The call is over (`why` for people, maybe empty).
    void ended(const QString &peer, const QString &why);
    // Something to tell people that does not end the call.
    void problem(const QString &text);

private:
    enum class End { Hungup, TheyHungUp, Declined, NoAnswer, Busy, Failed, Elsewhere, Missed };
    void onSignal(const QString &peer, quint32 device, const Protocol::Call &call, qint64 age, bool mine);
    void onIncoming(const QString &peer, quint32 device, const Protocol::Call &call, qint64 age);
    void connectWith(const std::function<void(bool ok)> &then);
    void onConnected();
    void send(const Protocol::Call &call);
    void sendHangup(Protocol::Call::Reason reason);
    void finish(End how, const QString &why = QString());
    void setState(State state);

    Messenger *m_messenger;
    State m_state = State::Idle;
    QString m_id;
    QString m_peer;
    quint32 m_peerDevice = 0; // the one that has the call (0: not known yet)
    bool m_incoming = false;
    bool m_wasActive = false;
    QString m_offerSdp;
    bool m_muted = false;
    bool m_devicesEnabled = true;
    bool m_processing = true;
    bool m_relayOnly = false;
    std::unique_ptr<CallConnection> m_connection;
    std::unique_ptr<CallAudio> m_audio;
    std::unique_ptr<CallDevices> m_devices;
    quint32 m_timestamp = 0;
    QTimer m_ringTimer;
    QTimer m_connectTimer;
    QElapsedTimer m_activeClock;
};
