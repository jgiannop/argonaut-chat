#pragma once

#include <QAudioFormat>
#include <QBuffer>
#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <memory>

class CallAudio;
class QAudioSink;
class QAudioSource;
class QIODevice;

// The default microphone and speaker during a call: what the microphone
// gives, as 48 kHz mono 16-bit frames of 20 ms (`frameCaptured`), and the
// speaker kept filled from CallAudio::nextFrame (a little ahead, ~60 ms).
class CallDevices : public QObject
{
    Q_OBJECT

public:
    explicit CallDevices(CallAudio *audio, QObject *parent = nullptr);
    ~CallDevices() override;

    // False (and why) if there is no microphone or speaker: the call goes
    // on, without that half.
    bool start(QString *error = nullptr);
    void stop();

signals:
    void frameCaptured(const QByteArray &frame);

private:
    void pump();

    CallAudio *m_audio;
    std::unique_ptr<QAudioSource> m_source;
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice *m_input = nullptr;
    QIODevice *m_output = nullptr;
    QAudioFormat m_inFormat;
    QAudioFormat m_outFormat;
    QByteArray m_captured; // the device's, not yet a whole block
    QByteArray m_frames;   // 48 kHz mono, not yet a whole frame
    QByteArray m_toPlay;   // in the speaker's format, not yet taken
    QTimer m_timer;
};

// The ring of an incoming call on the default speaker: two tones for a
// second, two seconds of quiet, again until stopped. Nothing without a speaker.
class CallRinger : public QObject
{
    Q_OBJECT

public:
    explicit CallRinger(QObject *parent = nullptr);
    ~CallRinger() override;
    void start();
    void stop();
    bool isRinging() const { return m_ringing; }

private:
    void playOnce();

    std::unique_ptr<QAudioSink> m_sink;
    QByteArray m_sound;
    QBuffer m_buffer;
    QTimer m_again;
    bool m_ringing = false;
};
