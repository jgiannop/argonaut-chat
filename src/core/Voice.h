#pragma once

#include <QByteArray>
#include <QString>

#include <memory>
#include <optional>

// Voice messages: recorded mono at SampleRate, sent as Ogg Opus (RFC 7845,
// libopusenc; about 120 KB a minute, a sixteenth of WAV) in an encrypted
// attachment marked as a voice note. Those from before 0.3.2 are WAV
// (16-bit PCM, about 2 MB a minute) and still play.
namespace Voice {

constexpr int SampleRate = 16000;
constexpr int MaxSeconds = 300; // a recording stops by itself then
constexpr const char *Type = "audio/wav";     // before 0.3.2
constexpr const char *OpusType = "audio/ogg"; // Ogg Opus
constexpr int OpusBitrate = 16000;            // wideband speech

struct Audio
{
    QByteArray samples; // 16-bit little-endian PCM, interleaved
    int sampleRate = SampleRate;
    int channels = 1;
    qint64 durationMs() const;
};

// A WAV file of 16-bit mono samples at `sampleRate`.
QByteArray toWav(const QByteArray &samples, int sampleRate = SampleRate);

// What arrives is a stranger's: only a plain WAV of 16-bit PCM, 1 or 2
// channels, 8 to 48 kHz, every size consistent, at most MaxSeconds (and a
// little), or nothing.
std::optional<Audio> fromWav(const QByteArray &wav);

// Ogg Opus made a piece at a time, while recording, so that sending does not
// wait (five minutes take seconds to encode). Mono 16-bit samples in.
// Invalid if it cannot be made (or this build has no Opus: the server's).
class OpusWriter
{
public:
    explicit OpusWriter(int sampleRate = SampleRate);
    ~OpusWriter();
    OpusWriter(const OpusWriter &) = delete;
    OpusWriter &operator=(const OpusWriter &) = delete;

    bool isValid() const;
    bool write(const QByteArray &samples);
    // The whole file; empty if anything failed. Nothing can be written after.
    QByteArray finish();
    qint64 samplesWritten() const { return m_samples; }

private:
    struct Private;
    std::unique_ptr<Private> d;
    qint64 m_samples = 0;
};

// All at once (an OpusWriter); empty if it fails.
QByteArray toOpus(const QByteArray &samples, int sampleRate = SampleRate);

// What arrives is a stranger's: only Ogg Opus with one stream of 1 or 2
// channels that decodes without a gap or an error, at most MaxSeconds (and a
// little), or nothing. Decoded at 48 kHz, as Opus always is.
std::optional<Audio> fromOpus(const QByteArray &ogg);

// Either kind, told by its first bytes ("RIFF" or "OggS").
std::optional<Audio> decode(const QByteArray &data);
// How long either kind is, without decoding an Ogg (its pages are checked,
// its length read from them); -1 if it is neither. For showing, not playing.
qint64 durationMs(const QByteArray &data);
// The same, for Ogg Opus only (-1 if it is not one).
qint64 opusDurationMs(const QByteArray &ogg);

// 16-bit samples from one rate and channel count to another (channels mixed
// down then copied, linear resampling): devices may insist on their own.
QByteArray convert(const QByteArray &samples, int fromRate, int fromChannels, int toRate, int toChannels);
// 32-bit float samples (−1 to 1) to 16-bit, and back.
QByteArray fromFloat(const QByteArray &floats);
QByteArray toFloat(const QByteArray &samples);

// "0:07", "4:59".
QString formatDuration(qint64 ms);

} // namespace Voice
