#include "CallConnection.h"

#include <QCoreApplication>
#include <QPointer>
#include <QRegularExpression>
#include <QtEndian>

#include <rtc/rtc.hpp>

#include <sodium.h>

#include <variant>

namespace {

constexpr char Mid[] = "audio";

} // namespace

// What libdatachannel's callbacks hold: the way back to the Qt thread. Only
// touched there (the owner clears it when it goes).
struct CallConnection::Bridge
{
    QPointer<CallConnection> owner;

    template <typename F>
    static void post(const std::weak_ptr<Bridge> &weak, F &&f)
    {
        QCoreApplication *app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app,
            [weak, f = std::forward<F>(f)]() mutable {
                const std::shared_ptr<Bridge> bridge = weak.lock();
                if (bridge && bridge->owner)
                    f(bridge->owner.data());
            },
            Qt::QueuedConnection);
    }
};

struct CallConnection::Private
{
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
};

QList<CallConnection::IceServer> CallConnection::iceServers(const QStringList &urls, const QString &username,
                                                            const QString &password)
{
    static const QRegularExpression shape(QStringLiteral(
        "^(turns?|stun):(\\[[0-9A-Fa-f:.]+\\]|[A-Za-z0-9.\\-]+)(?::([0-9]{1,5}))?(?:\\?transport=(udp|tcp))?$"));
    QList<IceServer> servers;
    for (const QString &url : urls) {
        const QRegularExpressionMatch match = shape.match(url);
        if (!match.hasMatch() || match.captured(1) == QLatin1String("turns"))
            continue; // TURN over TLS: not offered by our relay
        IceServer server;
        server.turn = match.captured(1) == QLatin1String("turn");
        server.host = match.captured(2);
        if (server.host.startsWith(QLatin1Char('[')))
            server.host = server.host.mid(1, server.host.size() - 2);
        if (!match.captured(3).isEmpty()) {
            const uint port = match.captured(3).toUInt();
            if (port == 0 || port > 65535)
                continue;
            server.port = quint16(port);
        }
        server.tcp = match.captured(4) == QLatin1String("tcp");
        if (server.tcp)
            continue; // libjuice (libdatachannel's ICE) speaks TURN over UDP only
        if (server.turn) {
            if (username.isEmpty() || password.isEmpty())
                continue;
            server.username = username;
            server.password = password;
        }
        servers << server;
    }
    return servers;
}

CallConnection::CallConnection(const QList<IceServer> &servers, bool relayOnly, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
    , m_bridge(std::make_shared<Bridge>())
{
    m_bridge->owner = this;
    // For finding out why a call does not connect: libdatachannel's own log
    // on the terminal (CHAT_RTC_LOG=1; addresses in it, so never by default).
    static const bool logging = [] {
        if (qEnvironmentVariableIsSet("CHAT_RTC_LOG"))
            rtc::InitLogger(rtc::LogLevel::Debug);
        return true;
    }();
    Q_UNUSED(logging);
    randombytes_buf(&m_ssrc, sizeof m_ssrc);
    randombytes_buf(&m_sequence, sizeof m_sequence);

    rtc::Configuration config;
    for (const IceServer &server : servers) {
        if (server.turn)
            config.iceServers.emplace_back(server.host.toStdString(), server.port, server.username.toStdString(),
                                           server.password.toStdString(),
                                           server.tcp ? rtc::IceServer::RelayType::TurnTcp
                                                      : rtc::IceServer::RelayType::TurnUdp);
        else
            config.iceServers.emplace_back(server.host.toStdString(), server.port);
    }
    config.disableAutoNegotiation = true; // our descriptions, when we say
    if (relayOnly)
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;
    d->pc = std::make_shared<rtc::PeerConnection>(config);

    const std::weak_ptr<Bridge> weak = m_bridge;
    d->pc->onStateChange([weak](rtc::PeerConnection::State state) {
        Bridge::post(weak, [state](CallConnection *self) { self->onState(int(state)); });
    });
    d->pc->onGatheringStateChange([weak](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete)
            Bridge::post(weak, [](CallConnection *self) { self->onGathered(); });
    });

    m_gatherTimer.setSingleShot(true);
    m_gatherTimer.setInterval(GatheringTimeoutMs);
    connect(&m_gatherTimer, &QTimer::timeout, this, &CallConnection::onGathered);
    m_disconnectTimer.setSingleShot(true);
    m_disconnectTimer.setInterval(DisconnectedTimeoutMs);
    connect(&m_disconnectTimer, &QTimer::timeout, this, [this] {
        if (!m_failed) {
            m_failed = true;
            emit failed();
        }
    });
}

CallConnection::~CallConnection()
{
    m_bridge->owner = nullptr;
    if (d->track)
        d->track->resetCallbacks();
    if (d->pc) {
        d->pc->resetCallbacks();
        d->pc->close();
    }
}

void CallConnection::attachTrack()
{
    rtc::Description::Audio media(Mid, rtc::Description::Direction::SendRecv);
    media.addOpusCodec(OpusPayloadType);
    media.addSSRC(m_ssrc, std::string("argonaut"));
    d->track = d->pc->addTrack(media);
    const std::weak_ptr<Bridge> weak = m_bridge;
    d->track->onMessage(
        [weak](rtc::binary data) {
            QByteArray bytes(reinterpret_cast<const char *>(data.data()), qsizetype(data.size()));
            Bridge::post(weak, [bytes](CallConnection *self) { self->onAudio(bytes); });
        },
        nullptr);
}

void CallConnection::offer()
{
    try {
        attachTrack();
        d->pc->setLocalDescription(rtc::Description::Type::Offer);
    } catch (const std::exception &) {
        QTimer::singleShot(0, this, [this] {
            m_failed = true;
            emit failed();
        });
        return;
    }
    m_gatherTimer.start();
}

bool CallConnection::answer(const QString &offerSdp)
{
    try {
        rtc::Description description(offerSdp.toStdString(), rtc::Description::Type::Offer);
        // Exactly one audio medium, ours by name, with Opus at our payload type.
        if (description.mediaCount() != 1)
            return false;
        const auto entry = description.media(0);
        const auto *audio = std::get_if<rtc::Description::Media *>(&entry);
        if (!audio || (*audio)->type() != "audio" || (*audio)->mid() != Mid
            || !(*audio)->hasPayloadType(OpusPayloadType))
            return false;
        d->pc->setRemoteDescription(description);
        // The offered track, now with our side of it (the same mid: the same track).
        attachTrack();
        d->pc->setLocalDescription(rtc::Description::Type::Answer);
    } catch (const std::exception &) {
        return false;
    }
    m_gatherTimer.start();
    return true;
}

bool CallConnection::accept(const QString &answerSdp)
{
    try {
        d->pc->setRemoteDescription(rtc::Description(answerSdp.toStdString(), rtc::Description::Type::Answer));
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

void CallConnection::onGathered()
{
    if (m_described || m_failed)
        return;
    m_gatherTimer.stop();
    const std::optional<rtc::Description> local = d->pc->localDescription();
    if (!local) {
        m_failed = true;
        emit failed();
        return;
    }
    m_described = true;
    emit described(QString::fromStdString(std::string(*local)));
}

void CallConnection::onState(int value)
{
    using State = rtc::PeerConnection::State;
    switch (State(value)) {
    case State::Connected:
        m_disconnectTimer.stop();
        if (!m_connected) {
            m_connected = true;
            emit connected();
        }
        break;
    case State::Disconnected:
        // Maybe back in a moment (a network change); if not, gone.
        if (!m_disconnectTimer.isActive())
            m_disconnectTimer.start();
        break;
    case State::Failed:
    case State::Closed:
        if (!m_failed) {
            m_failed = true;
            m_disconnectTimer.stop();
            emit failed();
        }
        break;
    default:
        break;
    }
}

bool CallConnection::sendAudio(const QByteArray &packet, quint32 timestamp)
{
    if (!d->track || !d->track->isOpen() || packet.isEmpty() || packet.size() > MaxPayloadBytes)
        return false;
    Rtp rtp;
    rtp.sequence = m_sequence++;
    rtp.timestamp = timestamp;
    rtp.ssrc = m_ssrc;
    rtp.payloadType = OpusPayloadType;
    rtp.payload = packet;
    const QByteArray data = buildRtp(rtp);
    try {
        return d->track->send(reinterpret_cast<const std::byte *>(data.constData()), size_t(data.size()));
    } catch (const std::exception &) {
        return false;
    }
}

void CallConnection::onAudio(const QByteArray &data)
{
    Rtp rtp;
    if (!parseRtp(data, &rtp) || rtp.payloadType != OpusPayloadType || rtp.payload.isEmpty())
        return; // RTCP, or not ours to play
    // One stream: the first source heard (a stray second one is ignored).
    if (!m_haveRemoteSsrc) {
        m_remoteSsrc = rtp.ssrc;
        m_haveRemoteSsrc = true;
    } else if (rtp.ssrc != m_remoteSsrc) {
        return;
    }
    emit audioReceived(rtp.sequence, rtp.timestamp, rtp.payload);
}

QByteArray CallConnection::buildRtp(const Rtp &packet)
{
    QByteArray data(12, '\0');
    auto *p = reinterpret_cast<uchar *>(data.data());
    p[0] = 0x80; // version 2, no padding, no extension, no CSRC
    p[1] = uchar(packet.payloadType & 0x7f);
    qToBigEndian<quint16>(packet.sequence, p + 2);
    qToBigEndian<quint32>(packet.timestamp, p + 4);
    qToBigEndian<quint32>(packet.ssrc, p + 8);
    return data + packet.payload;
}

bool CallConnection::parseRtp(const QByteArray &data, Rtp *packet)
{
    if (data.size() < 12 || data.size() > 12 + 15 * 4 + 4 + 65535 * 4)
        return false;
    const auto *p = reinterpret_cast<const uchar *>(data.constData());
    if ((p[0] >> 6) != 2)
        return false;
    const bool padding = p[0] & 0x20;
    const bool extension = p[0] & 0x10;
    const int csrcs = p[0] & 0x0f;
    qsizetype offset = 12 + 4 * csrcs;
    if (data.size() < offset)
        return false;
    if (extension) {
        if (data.size() < offset + 4)
            return false;
        const qsizetype words = qFromBigEndian<quint16>(p + offset + 2);
        offset += 4 + 4 * words;
        if (data.size() < offset)
            return false;
    }
    qsizetype end = data.size();
    if (padding) {
        const int count = p[end - 1];
        if (count == 0 || end - count < offset)
            return false;
        end -= count;
    }
    if (end - offset > MaxPayloadBytes)
        return false;
    packet->payloadType = p[1] & 0x7f;
    packet->sequence = qFromBigEndian<quint16>(p + 2);
    packet->timestamp = qFromBigEndian<quint32>(p + 4);
    packet->ssrc = qFromBigEndian<quint32>(p + 8);
    packet->payload = data.mid(offset, end - offset);
    return true;
}
