#pragma once

#include <QByteArray>
#include <QMap>

#include <memory>

// A call's sound, without the devices (CallDevices plays and records): 48 kHz
// mono, 20 ms frames.
//  - Going out: the microphone's frame, the echo of what was just played
//    taken out of it and the noise lowered (speexdsp), encoded as Opus (voice,
//    32 kbit/s, in-band FEC for a lost packet).
//  - Coming in: packets from a stranger's stream, kept in order in a small
//    jitter buffer (60 ms before playing starts, at most MaxBufferedFrames),
//    decoded strictly; a missing one rebuilt from the next one's FEC or
//    concealed by Opus.
class CallAudio
{
public:
    static constexpr int SampleRate = 48000;
    static constexpr int FrameSamples = 960; // 20 ms
    static constexpr int FrameBytes = FrameSamples * 2;
    static constexpr int Bitrate = 32000;
    static constexpr int MaxPacketBytes = 1275; // Opus's most for one frame
    static constexpr int StartFrames = 3;        // buffered before playing
    static constexpr int MaxBufferedFrames = 15; // more: the oldest dropped (latency)
    static constexpr int MaxConcealedFrames = 25; // nothing for 0.5 s: wait to fill again

    CallAudio();
    ~CallAudio();
    CallAudio(const CallAudio &) = delete;
    CallAudio &operator=(const CallAudio &) = delete;

    bool isValid() const;
    // Echo cancellation and noise suppression (on by default; tests turn it off).
    void setProcessing(bool on) { m_processing = on; }

    // One frame from the microphone (FrameBytes of 16-bit samples): an Opus
    // packet (silence when `muted`; empty on an error).
    QByteArray encode(const QByteArray &frame, bool muted);
    // A packet from the other side (RTP sequence number and payload).
    void received(quint16 sequence, const QByteArray &packet);
    // The next frame to play (always FrameBytes; silence while waiting).
    QByteArray nextFrame();

    int bufferedFrames() const { return int(m_buffer.size()); }
    bool isPlaying() const { return m_playing; }
    qint64 concealedFrames() const { return m_concealedTotal; }

private:
    struct Private;
    std::unique_ptr<Private> d;
    bool m_processing = true;

    // Sequence numbers extended to 64 bits (they wrap at 65536).
    QMap<qint64, QByteArray> m_buffer;
    qint64 m_lastExtended = -1;
    qint64 m_next = 0;
    bool m_playing = false;
    int m_concealed = 0;
    qint64 m_concealedTotal = 0;
};
