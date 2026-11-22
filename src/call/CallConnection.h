#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

// One call's connection to the other side, through libdatachannel: ICE
// (direct, or through the relay), DTLS-SRTP, one Opus audio track. The
// session descriptions are exchanged by the caller (inside end-to-end
// encrypted messages); each carries its side's DTLS fingerprint, which the
// other checks, so nobody between them can take part. Whole descriptions
// only (every candidate gathered first: no trickling).
//
// libdatachannel calls back on its own threads; everything here is handed
// over to the Qt thread first, and nothing arrives after destruction.
class CallConnection : public QObject
{
    Q_OBJECT

public:
    struct IceServer
    {
        QString host;
        quint16 port = 3478;
        bool turn = false; // else STUN
        bool tcp = false;
        QString username;
        QString password;
    };
    // From the server's relay answer (Messenger::Relay): "turn:host:port"
    // and "stun:host:port" (over UDP); others skipped (TURN over TCP or TLS:
    // libjuice has none).
    static QList<IceServer> iceServers(const QStringList &urls, const QString &username, const QString &password);

    static constexpr int OpusPayloadType = 111;
    static constexpr int GatheringTimeoutMs = 5000;
    static constexpr int DisconnectedTimeoutMs = 10000;
    static constexpr int MaxPayloadBytes = 1275; // an Opus packet's most

    // `relayOnly`: no direct paths, through the relay only (tests).
    explicit CallConnection(const QList<IceServer> &servers, bool relayOnly = false, QObject *parent = nullptr);
    ~CallConnection() override;

    // The caller: makes the offer (`described` when ready).
    void offer();
    // The one called: takes the offer and makes the answer (`described`).
    bool answer(const QString &offerSdp);
    // The caller: takes the answer.
    bool accept(const QString &answerSdp);

    bool isConnected() const { return m_connected; }
    // One Opus packet of 20 ms (timestamps at 48 kHz).
    bool sendAudio(const QByteArray &packet, quint32 timestamp);

    // RTP (RFC 3550) without the library's handlers: ours to build, and to
    // read strictly what the other side sends.
    struct Rtp
    {
        quint16 sequence = 0;
        quint32 timestamp = 0;
        quint32 ssrc = 0;
        int payloadType = 0;
        QByteArray payload;
    };
    static QByteArray buildRtp(const Rtp &packet);
    static bool parseRtp(const QByteArray &data, Rtp *packet);

signals:
    // The local session description, every candidate in it.
    void described(const QString &sdp);
    void connected();
    // Could not connect, or the connection went.
    void failed();
    void audioReceived(quint16 sequence, quint32 timestamp, const QByteArray &packet);

private:
    struct Private;
    struct Bridge;
    void onState(int state);
    void onGathered();
    void onAudio(const QByteArray &data);
    void attachTrack();

    std::unique_ptr<Private> d;
    std::shared_ptr<Bridge> m_bridge;
    QTimer m_gatherTimer;
    QTimer m_disconnectTimer;
    bool m_described = false;
    bool m_connected = false;
    bool m_failed = false;
    quint16 m_sequence = 0;
    quint32 m_ssrc = 0;
    quint32 m_remoteSsrc = 0;
    bool m_haveRemoteSsrc = false;
};
