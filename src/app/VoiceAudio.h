#pragma once

#include <QAudioFormat>
#include <QBuffer>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "Voice.h"

#include <memory>

class QAudioSink;
class QAudioSource;
class QIODevice;

// Records a voice message from the default microphone: what the device
// gives, turned into 16-bit mono at Voice::SampleRate and encoded as Opus as
// it comes (so sending does not wait). Stops by itself at Voice::MaxSeconds
// (`limitReached`).
class VoiceRecorder : public QObject
{
    Q_OBJECT

public:
    explicit VoiceRecorder(QObject *parent = nullptr);
    ~VoiceRecorder() override;

    bool start(QString *error = nullptr);
    // The Ogg Opus file so far, for Messenger::sendVoiceNote (empty if it
    // failed); recording ends.
    QByteArray stop();
    void cancel();
    bool isRecording() const { return m_source != nullptr; }
    qint64 elapsedMs() const;

signals:
    void tick(qint64 ms);
    void limitReached();

private:
    std::unique_ptr<QAudioSource> m_source;
    QIODevice *m_input = nullptr;
    QAudioFormat m_format;
    void feed(bool all);
    QByteArray m_pending; // what the device gave, not yet encoded
    std::unique_ptr<Voice::OpusWriter> m_writer;
    QElapsedTimer m_clock;
    QTimer m_timer;
};

// Plays one voice message at a time.
class VoicePlayer : public QObject
{
    Q_OBJECT

public:
    explicit VoicePlayer(QObject *parent = nullptr);
    ~VoicePlayer() override;

    // A WAV from Voice::toWav (or anyone's, checked); `id` names it (the attachment).
    bool play(qint64 id, const QByteArray &data, QString *error = nullptr);
    void stop();
    qint64 playing() const { return m_id; } // 0: nothing
    qint64 positionMs() const;
    qint64 durationMs() const { return m_durationMs; }

signals:
    // Now and then while playing, and once with id 0 when done.
    void progress(qint64 id, qint64 positionMs, qint64 durationMs);

private:
    void finish();

    std::unique_ptr<QAudioSink> m_sink;
    QBuffer m_buffer;
    QByteArray m_data;
    qint64 m_id = 0;
    qint64 m_durationMs = 0;
    QTimer m_timer;
};
