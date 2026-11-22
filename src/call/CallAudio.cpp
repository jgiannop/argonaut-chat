#include "CallAudio.h"

#include <opus.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

namespace {

constexpr int EchoTailSamples = CallAudio::SampleRate / 5; // 200 ms of echo path

} // namespace

struct CallAudio::Private
{
    OpusEncoder *encoder = nullptr;
    OpusDecoder *decoder = nullptr;
    SpeexEchoState *echo = nullptr;
    SpeexPreprocessState *preprocess = nullptr;

    ~Private()
    {
        if (preprocess)
            speex_preprocess_state_destroy(preprocess);
        if (echo)
            speex_echo_state_destroy(echo);
        if (decoder)
            opus_decoder_destroy(decoder);
        if (encoder)
            opus_encoder_destroy(encoder);
    }
};

CallAudio::CallAudio()
    : d(std::make_unique<Private>())
{
    int error = OPUS_OK;
    d->encoder = opus_encoder_create(SampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK)
        d->encoder = nullptr;
    if (d->encoder) {
        opus_encoder_ctl(d->encoder, OPUS_SET_BITRATE(Bitrate));
        opus_encoder_ctl(d->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(d->encoder, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(d->encoder, OPUS_SET_PACKET_LOSS_PERC(10));
        opus_encoder_ctl(d->encoder, OPUS_SET_COMPLEXITY(9));
    }
    d->decoder = opus_decoder_create(SampleRate, 1, &error);
    if (error != OPUS_OK)
        d->decoder = nullptr;
    d->echo = speex_echo_state_init(FrameSamples, EchoTailSamples);
    if (d->echo) {
        int rate = SampleRate;
        speex_echo_ctl(d->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    }
    d->preprocess = speex_preprocess_state_init(FrameSamples, SampleRate);
    if (d->preprocess) {
        int on = 1;
        speex_preprocess_ctl(d->preprocess, SPEEX_PREPROCESS_SET_DENOISE, &on);
        if (d->echo)
            speex_preprocess_ctl(d->preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, d->echo);
    }
}

CallAudio::~CallAudio() = default;

bool CallAudio::isValid() const
{
    return d->encoder && d->decoder && d->echo && d->preprocess;
}

QByteArray CallAudio::encode(const QByteArray &frame, bool muted)
{
    if (!isValid() || frame.size() != FrameBytes)
        return {};
    QByteArray clean(FrameBytes, '\0');
    if (!muted) {
        if (m_processing) {
            speex_echo_capture(d->echo, reinterpret_cast<const spx_int16_t *>(frame.constData()),
                               reinterpret_cast<spx_int16_t *>(clean.data()));
            speex_preprocess_run(d->preprocess, reinterpret_cast<spx_int16_t *>(clean.data()));
        } else {
            clean = frame;
        }
    } else if (m_processing) {
        // Still told what was captured, so it keeps up with what is played.
        QByteArray ignored(FrameBytes, '\0');
        speex_echo_capture(d->echo, reinterpret_cast<const spx_int16_t *>(frame.constData()),
                           reinterpret_cast<spx_int16_t *>(ignored.data()));
    }
    QByteArray packet(MaxPacketBytes, '\0');
    const opus_int32 size =
        opus_encode(d->encoder, reinterpret_cast<const opus_int16 *>(clean.constData()), FrameSamples,
                    reinterpret_cast<unsigned char *>(packet.data()), MaxPacketBytes);
    if (size <= 0)
        return {};
    packet.truncate(size);
    return packet;
}

void CallAudio::received(quint16 sequence, const QByteArray &packet)
{
    if (!isValid() || packet.isEmpty() || packet.size() > MaxPacketBytes)
        return;
    // Only what Opus would decode as one 20 ms frame, mono or stereo.
    const int samples = opus_packet_get_nb_samples(reinterpret_cast<const unsigned char *>(packet.constData()),
                                                   opus_int32(packet.size()), SampleRate);
    if (samples != FrameSamples)
        return;
    qint64 extended = sequence;
    if (m_lastExtended >= 0) {
        // The nearest to the last one heard.
        const qint64 base = m_lastExtended & ~qint64(0xffff);
        extended = base + sequence;
        if (extended - m_lastExtended > 0x8000)
            extended -= 0x10000;
        else if (m_lastExtended - extended > 0x8000)
            extended += 0x10000;
    }
    if (extended < 0)
        return;
    if (extended > m_lastExtended)
        m_lastExtended = extended;
    if (m_playing && extended < m_next)
        return; // too late
    m_buffer.insert(extended, packet);
    while (m_buffer.size() > MaxBufferedFrames) {
        // Behind: catch up, dropping the oldest.
        m_buffer.erase(m_buffer.begin());
        if (m_playing)
            m_next = qMax(m_next, m_buffer.firstKey());
    }
    if (!m_playing && m_buffer.size() >= StartFrames) {
        m_playing = true;
        m_next = m_buffer.firstKey();
        m_concealed = 0;
    }
}

QByteArray CallAudio::nextFrame()
{
    QByteArray out(FrameBytes, '\0');
    if (!isValid())
        return out;
    auto *samples = reinterpret_cast<opus_int16 *>(out.data());
    if (m_playing) {
        int decoded = -1;
        const auto it = m_buffer.constFind(m_next);
        if (it != m_buffer.constEnd()) {
            decoded = opus_decode(d->decoder, reinterpret_cast<const unsigned char *>(it->constData()),
                                  opus_int32(it->size()), samples, FrameSamples, 0);
            m_buffer.remove(m_next);
            m_concealed = 0;
        } else {
            const auto after = m_buffer.constFind(m_next + 1);
            if (after != m_buffer.constEnd()) // rebuilt from the next one's FEC
                decoded = opus_decode(d->decoder, reinterpret_cast<const unsigned char *>(after->constData()),
                                      opus_int32(after->size()), samples, FrameSamples, 1);
            else
                decoded = opus_decode(d->decoder, nullptr, 0, samples, FrameSamples, 0);
            ++m_concealed;
            ++m_concealedTotal;
        }
        if (decoded != FrameSamples)
            out.fill('\0');
        ++m_next;
        if (m_concealed > MaxConcealedFrames) {
            // Nothing for a while: silence, and start again when it comes.
            m_playing = false;
            m_buffer.clear();
            out.fill('\0');
        }
    }
    if (m_processing)
        speex_echo_playback(d->echo, reinterpret_cast<const spx_int16_t *>(out.constData()));
    return out;
}
