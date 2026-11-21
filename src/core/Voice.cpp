#include "Voice.h"

#include <QList>
#include <QString>
#include <QtEndian>

namespace Voice {

namespace {

void put16(QByteArray &out, quint16 v)
{
    char b[2];
    qToLittleEndian(v, b);
    out.append(b, 2);
}

void put32(QByteArray &out, quint32 v)
{
    char b[4];
    qToLittleEndian(v, b);
    out.append(b, 4);
}

quint16 get16(const QByteArray &data, qsizetype at)
{
    return qFromLittleEndian<quint16>(data.constData() + at);
}

quint32 get32(const QByteArray &data, qsizetype at)
{
    return qFromLittleEndian<quint32>(data.constData() + at);
}

} // namespace

qint64 Audio::durationMs() const
{
    const qint64 frames = samples.size() / (2 * qMax(1, channels));
    return sampleRate > 0 ? frames * 1000 / sampleRate : 0;
}

QByteArray toWav(const QByteArray &samples, int sampleRate)
{
    const quint32 size = quint32(samples.size() & ~qsizetype(1)); // whole samples
    QByteArray out;
    out.reserve(44 + size);
    out.append("RIFF", 4);
    put32(out, 36 + size);
    out.append("WAVE", 4);
    out.append("fmt ", 4);
    put32(out, 16);
    put16(out, 1); // PCM
    put16(out, 1); // mono
    put32(out, quint32(sampleRate));
    put32(out, quint32(sampleRate) * 2); // bytes per second
    put16(out, 2);                       // bytes per frame
    put16(out, 16);                      // bits per sample
    out.append("data", 4);
    put32(out, size);
    out.append(samples.constData(), size);
    return out;
}

std::optional<Audio> fromWav(const QByteArray &wav)
{
    if (wav.size() < 44 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE")
        return std::nullopt;
    const qint64 riffSize = get32(wav, 4);
    if (riffSize + 8 > wav.size() || riffSize < 36)
        return std::nullopt;
    const qsizetype end = qsizetype(riffSize + 8);
    std::optional<Audio> audio;
    bool haveFormat = false;
    int channels = 0;
    int rate = 0;
    qsizetype at = 12;
    // Its chunks: "fmt " before "data"; others (LIST…) skipped.
    while (at + 8 <= end) {
        const QByteArray id = wav.mid(at, 4);
        const qint64 size = get32(wav, at + 4);
        const qsizetype body = at + 8;
        if (size > end - body)
            return std::nullopt;
        if (id == "fmt ") {
            if (haveFormat || size < 16)
                return std::nullopt;
            const int format = get16(wav, body);
            channels = get16(wav, body + 2);
            rate = int(get32(wav, body + 4));
            const quint32 byteRate = get32(wav, body + 8);
            const int blockAlign = get16(wav, body + 12);
            const int bits = get16(wav, body + 14);
            if (format != 1 || bits != 16 || channels < 1 || channels > 2 || rate < 8000 || rate > 48000
                || blockAlign != 2 * channels || byteRate != quint32(rate * blockAlign))
                return std::nullopt;
            haveFormat = true;
        } else if (id == "data") {
            if (!haveFormat || size % (2 * channels) != 0)
                return std::nullopt;
            Audio a;
            a.samples = wav.mid(body, qsizetype(size));
            a.sampleRate = rate;
            a.channels = channels;
            if (a.durationMs() > (MaxSeconds + 5) * 1000LL)
                return std::nullopt;
            audio = a;
            break;
        }
        at = body + qsizetype(size) + (size & 1); // chunks are padded to even sizes
    }
    return audio;
}

QByteArray convert(const QByteArray &samples, int fromRate, int fromChannels, int toRate, int toChannels)
{
    if (fromRate <= 0 || toRate <= 0 || fromChannels < 1 || toChannels < 1)
        return {};
    if (fromRate == toRate && fromChannels == toChannels)
        return samples;
    const qsizetype frames = samples.size() / (2 * fromChannels);
    const auto *in = reinterpret_cast<const qint16 *>(samples.constData());
    // Mono first.
    QList<float> mono(frames);
    for (qsizetype f = 0; f < frames; ++f) {
        int sum = 0;
        for (int c = 0; c < fromChannels; ++c)
            sum += qFromLittleEndian(in[f * fromChannels + c]);
        mono[f] = float(sum) / fromChannels;
    }
    const qsizetype outFrames = frames * qint64(toRate) / fromRate;
    QByteArray out(outFrames * 2 * toChannels, Qt::Uninitialized);
    auto *o = reinterpret_cast<qint16 *>(out.data());
    for (qsizetype f = 0; f < outFrames; ++f) {
        const double position = double(f) * fromRate / toRate;
        const qsizetype i = qsizetype(position);
        const double t = position - double(i);
        const float a = mono.value(i);
        const float b = mono.value(qMin(i + 1, frames - 1));
        const qint16 v = qint16(qBound(-32768.0, a + (b - a) * t, 32767.0));
        for (int c = 0; c < toChannels; ++c)
            o[f * toChannels + c] = qToLittleEndian(v);
    }
    return out;
}

QByteArray fromFloat(const QByteArray &floats)
{
    const qsizetype n = floats.size() / 4;
    QByteArray out(n * 2, Qt::Uninitialized);
    const auto *in = reinterpret_cast<const float *>(floats.constData());
    auto *o = reinterpret_cast<qint16 *>(out.data());
    for (qsizetype i = 0; i < n; ++i)
        o[i] = qToLittleEndian(qint16(qBound(-32768.0f, in[i] * 32767.0f, 32767.0f)));
    return out;
}

QByteArray toFloat(const QByteArray &samples)
{
    const qsizetype n = samples.size() / 2;
    QByteArray out(n * 4, Qt::Uninitialized);
    const auto *in = reinterpret_cast<const qint16 *>(samples.constData());
    auto *o = reinterpret_cast<float *>(out.data());
    for (qsizetype i = 0; i < n; ++i)
        o[i] = float(qFromLittleEndian(in[i])) / 32768.0f;
    return out;
}

QString formatDuration(qint64 ms)
{
    const qint64 seconds = qMax<qint64>(0, (ms + 500) / 1000);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

} // namespace Voice
