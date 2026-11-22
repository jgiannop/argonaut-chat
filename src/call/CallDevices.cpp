#include "CallDevices.h"

#include "CallAudio.h"
#include "Voice.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

// 16-bit if the device takes it, else its own preference (16-bit or float).
QAudioFormat formatFor(const QAudioDevice &device)
{
    QAudioFormat wanted;
    wanted.setSampleRate(CallAudio::SampleRate);
    wanted.setChannelCount(1);
    wanted.setSampleFormat(QAudioFormat::Int16);
    if (device.isFormatSupported(wanted))
        return wanted;
    QAudioFormat preferred = device.preferredFormat();
    if (preferred.sampleFormat() != QAudioFormat::Float)
        preferred.setSampleFormat(QAudioFormat::Int16);
    return preferred;
}

int bytesPerFrame(const QAudioFormat &format)
{
    return (format.sampleFormat() == QAudioFormat::Float ? 4 : 2) * format.channelCount();
}

constexpr int TickMs = 10;
constexpr int AheadFrames = 3; // played ahead of now: ~60 ms
constexpr double Pi = 3.14159265358979323846;

} // namespace

CallDevices::CallDevices(CallAudio *audio, QObject *parent)
    : QObject(parent)
    , m_audio(audio)
{
    m_timer.setInterval(TickMs);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &CallDevices::pump);
}

CallDevices::~CallDevices()
{
    stop();
}

bool CallDevices::start(QString *error)
{
    stop();
    QStringList missing;
    const QAudioDevice input = QMediaDevices::defaultAudioInput();
    if (!input.isNull()) {
        m_inFormat = formatFor(input);
        m_source = std::make_unique<QAudioSource>(input, m_inFormat);
        m_source->setBufferSize(m_inFormat.bytesForDuration(100 * 1000));
        m_input = m_source->start();
        if (!m_input || m_source->error() != QAudio::NoError) {
            m_source.reset();
            m_input = nullptr;
        }
    }
    if (!m_input)
        missing << tr("The microphone could not be opened.");
    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (!output.isNull()) {
        m_outFormat = formatFor(output);
        m_sink = std::make_unique<QAudioSink>(output, m_outFormat);
        m_sink->setBufferSize(m_outFormat.bytesForDuration(AheadFrames * 20 * 1000 * 2));
        m_output = m_sink->start();
        if (!m_output || m_sink->error() != QAudio::NoError) {
            m_sink.reset();
            m_output = nullptr;
        }
    }
    if (!m_output)
        missing << tr("The speaker could not be opened.");
    m_timer.start();
    if (error)
        *error = missing.join(QLatin1Char(' '));
    return missing.isEmpty();
}

void CallDevices::stop()
{
    m_timer.stop();
    if (m_source)
        m_source->stop();
    if (m_sink)
        m_sink->stop();
    m_source.reset();
    m_sink.reset();
    m_input = nullptr;
    m_output = nullptr;
    m_captured.clear();
    m_frames.clear();
    m_toPlay.clear();
}

void CallDevices::pump()
{
    // In: the microphone's, in whole blocks of the rates' ratio, as 48 kHz mono.
    if (m_input) {
        m_captured.append(m_input->readAll());
        const int rate = m_inFormat.sampleRate();
        const int channels = m_inFormat.channelCount();
        const int frameBytes = bytesPerFrame(m_inFormat);
        if (rate > 0 && channels > 0) {
            const qsizetype block = rate / std::gcd(rate, CallAudio::SampleRate);
            qsizetype frames = m_captured.size() / frameBytes;
            frames -= frames % block;
            if (frames > 0) {
                QByteArray chunk = m_captured.left(frames * frameBytes);
                m_captured.remove(0, chunk.size());
                if (m_inFormat.sampleFormat() == QAudioFormat::Float)
                    chunk = Voice::fromFloat(chunk);
                m_frames.append(Voice::convert(chunk, rate, channels, CallAudio::SampleRate, 1));
            }
        }
        while (m_frames.size() >= CallAudio::FrameBytes) {
            const QByteArray frame = m_frames.left(CallAudio::FrameBytes);
            m_frames.remove(0, CallAudio::FrameBytes);
            emit frameCaptured(frame);
        }
    }
    // Out: kept ahead, a frame at a time.
    if (m_output && m_sink) {
        const int rate = m_outFormat.sampleRate();
        const int channels = m_outFormat.channelCount();
        if (rate <= 0 || channels <= 0)
            return;
        const qsizetype want = m_outFormat.bytesForDuration(AheadFrames * 20 * 1000);
        while (m_toPlay.size() < m_sink->bytesFree() && m_toPlay.size() < want) {
            QByteArray frame = Voice::convert(m_audio->nextFrame(), CallAudio::SampleRate, 1, rate, channels);
            if (m_outFormat.sampleFormat() == QAudioFormat::Float)
                frame = Voice::toFloat(frame);
            m_toPlay.append(frame);
        }
        const qint64 written = m_output->write(m_toPlay.left(m_sink->bytesFree()));
        if (written > 0)
            m_toPlay.remove(0, written);
    }
}

CallRinger::CallRinger(QObject *parent)
    : QObject(parent)
{
    // 440 + 480 Hz for a second (soft at its ends), at a quarter of full scale.
    const int rate = CallAudio::SampleRate;
    m_sound.resize(rate * 2);
    auto *samples = reinterpret_cast<qint16 *>(m_sound.data());
    for (int i = 0; i < rate; ++i) {
        const double t = double(i) / rate;
        const double fade = std::min({1.0, t / 0.02, (1.0 - t) / 0.02});
        samples[i] = qint16(4000 * fade * (std::sin(2 * Pi * 440 * t) + std::sin(2 * Pi * 480 * t)));
    }
    m_again.setSingleShot(true);
    m_again.setInterval(3000);
    connect(&m_again, &QTimer::timeout, this, &CallRinger::playOnce);
}

CallRinger::~CallRinger()
{
    stop();
}

void CallRinger::start()
{
    if (m_ringing)
        return;
    m_ringing = true;
    playOnce();
}

void CallRinger::stop()
{
    m_ringing = false;
    m_again.stop();
    if (m_sink)
        m_sink->stop();
    m_sink.reset();
    m_buffer.close();
}

void CallRinger::playOnce()
{
    if (!m_ringing)
        return;
    m_again.start();
    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    if (output.isNull())
        return;
    QAudioFormat format;
    format.setSampleRate(CallAudio::SampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    QByteArray sound = m_sound;
    if (!output.isFormatSupported(format)) {
        const QAudioFormat preferred = formatFor(output);
        sound = Voice::convert(m_sound, CallAudio::SampleRate, 1, preferred.sampleRate(), preferred.channelCount());
        if (preferred.sampleFormat() == QAudioFormat::Float)
            sound = Voice::toFloat(sound);
        format = preferred;
    }
    if (m_sink)
        m_sink->stop();
    m_buffer.close();
    m_buffer.setData(sound);
    m_buffer.open(QIODevice::ReadOnly);
    m_sink = std::make_unique<QAudioSink>(output, format);
    m_sink->start(&m_buffer);
}
