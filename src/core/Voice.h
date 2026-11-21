#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

// Voice messages: plain audio, as WAV (16-bit PCM), so no codec is needed on
// any system. Recorded mono at SampleRate; a minute is about 2 MB, about what
// Signal's are. Sent as an encrypted attachment marked "voice".
namespace Voice {

constexpr int SampleRate = 16000;
constexpr int MaxSeconds = 300; // a recording stops by itself then
constexpr const char *Type = "audio/wav";

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

// 16-bit samples from one rate and channel count to another (channels mixed
// down then copied, linear resampling): devices may insist on their own.
QByteArray convert(const QByteArray &samples, int fromRate, int fromChannels, int toRate, int toChannels);
// 32-bit float samples (−1 to 1) to 16-bit, and back.
QByteArray fromFloat(const QByteArray &floats);
QByteArray toFloat(const QByteArray &samples);

// "0:07", "4:59".
QString formatDuration(qint64 ms);

} // namespace Voice
