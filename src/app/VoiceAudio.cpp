#include "VoiceAudio.h"

#include "Voice.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>

namespace {

// 16-bit if the device takes it, else its own preference (16-bit or float).
QAudioFormat formatFor(const QAudioDevice &device, int rate, int channels)
{
    QAudioFormat wanted;
    wanted.setSampleRate(rate);
    wanted.setChannelCount(channels);
    wanted.setSampleFormat(QAudioFormat::Int16);
    if (device.isFormatSupported(wanted))
        return wanted;
    QAudioFormat preferred = device.preferredFormat();
    if (preferred.sampleFormat() != QAudioFormat::Float)
        preferred.setSampleFormat(QAudioFormat::Int16);
    return preferred;
}

} // namespace

VoiceRecorder::VoiceRecorder(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (!m_input)
            return;
        m_raw.append(m_input->readAll());
        const qint64 ms = elapsedMs();
        emit tick(ms);
        if (ms >= Voice::MaxSeconds * 1000LL)
            emit limitReached();
    });
}

VoiceRecorder::~VoiceRecorder()
{
    cancel();
}

bool VoiceRecorder::start(QString *error)
{
    cancel();
    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        if (error)
            *error = tr("There is no microphone.");
        return false;
    }
    m_format = formatFor(device, Voice::SampleRate, 1);
    m_source = std::make_unique<QAudioSource>(device, m_format);
    m_input = m_source->start();
    if (!m_input || m_source->error() != QAudio::NoError) {
        m_source.reset();
        m_input = nullptr;
        if (error)
            *error = tr("The microphone could not be opened.");
        return false;
    }
    m_raw.clear();
    m_clock.start();
    m_timer.start();
    return true;
}

QByteArray VoiceRecorder::stop()
{
    if (!m_source)
        return {};
    m_raw.append(m_input->readAll());
    m_source->stop();
    QByteArray samples = m_raw;
    if (m_format.sampleFormat() == QAudioFormat::Float)
        samples = Voice::fromFloat(samples);
    samples = Voice::convert(samples, m_format.sampleRate(), m_format.channelCount(), Voice::SampleRate, 1);
    cancel();
    return samples.left(qsizetype(Voice::MaxSeconds) * Voice::SampleRate * 2);
}

void VoiceRecorder::cancel()
{
    m_timer.stop();
    if (m_source)
        m_source->stop();
    m_source.reset();
    m_input = nullptr;
    m_raw.clear();
}

qint64 VoiceRecorder::elapsedMs() const
{
    return m_source ? m_clock.elapsed() : 0;
}

VoicePlayer::VoicePlayer(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (!m_sink)
            return;
        if (m_buffer.atEnd() && m_sink->state() != QAudio::ActiveState) {
            finish(); // all of it heard
            return;
        }
        emit progress(m_id, positionMs(), m_durationMs);
    });
}

VoicePlayer::~VoicePlayer()
{
    stop();
}

bool VoicePlayer::play(qint64 id, const QByteArray &wav, QString *error)
{
    stop();
    const auto audio = Voice::fromWav(wav);
    if (!audio) {
        if (error)
            *error = tr("This voice message cannot be played.");
        return false;
    }
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        if (error)
            *error = tr("There is no speaker.");
        return false;
    }
    const QAudioFormat format = formatFor(device, audio->sampleRate, audio->channels);
    m_data = Voice::convert(audio->samples, audio->sampleRate, audio->channels, format.sampleRate(), format.channelCount());
    if (format.sampleFormat() == QAudioFormat::Float)
        m_data = Voice::toFloat(m_data);
    m_buffer.setData(m_data);
    m_buffer.open(QIODevice::ReadOnly);
    m_sink = std::make_unique<QAudioSink>(device, format);
    m_sink->start(&m_buffer);
    if (m_sink->error() != QAudio::NoError) {
        stop();
        if (error)
            *error = tr("The sound could not be played.");
        return false;
    }
    m_id = id;
    m_durationMs = audio->durationMs();
    m_timer.start();
    emit progress(m_id, 0, m_durationMs);
    return true;
}

qint64 VoicePlayer::positionMs() const
{
    return m_sink ? qMin(m_durationMs, m_sink->processedUSecs() / 1000) : 0;
}

void VoicePlayer::stop()
{
    m_timer.stop();
    if (m_sink)
        m_sink->stop();
    m_sink.reset();
    m_buffer.close();
    m_data.clear();
    const bool was = m_id != 0;
    m_id = 0;
    m_durationMs = 0;
    if (was)
        emit progress(0, 0, 0);
}

void VoicePlayer::finish()
{
    stop();
}
