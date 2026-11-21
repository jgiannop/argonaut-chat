#include "Attachment.h"
#include "TestSupport.h"
#include "Voice.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QTest>
#include <QtEndian>

#include <cmath>

namespace {

// A second of a 440 Hz tone, 16-bit mono at `rate`.
QByteArray tone(int rate, int seconds = 1)
{
    QByteArray out(rate * seconds * 2, Qt::Uninitialized);
    auto *s = reinterpret_cast<qint16 *>(out.data());
    for (int i = 0; i < rate * seconds; ++i)
        s[i] = qToLittleEndian(qint16(10000 * std::sin(2 * M_PI * 440 * i / rate)));
    return out;
}

void set16(QByteArray &data, int at, quint16 v)
{
    qToLittleEndian(v, data.data() + at);
}

void set32(QByteArray &data, int at, quint32 v)
{
    qToLittleEndian(v, data.data() + at);
}

} // namespace

class TestVoice : public QObject
{
    Q_OBJECT

private slots:
    void wavBothWays()
    {
        const QByteArray samples = tone(Voice::SampleRate, 2);
        const QByteArray wav = Voice::toWav(samples);
        QCOMPARE(wav.size(), samples.size() + 44);
        const auto audio = Voice::fromWav(wav);
        QVERIFY(audio);
        QCOMPARE(audio->samples, samples);
        QCOMPARE(audio->sampleRate, Voice::SampleRate);
        QCOMPARE(audio->channels, 1);
        QCOMPARE(audio->durationMs(), 2000);
        QCOMPARE(Voice::formatDuration(audio->durationMs()), QStringLiteral("0:02"));
        QCOMPARE(Voice::formatDuration(299400), QStringLiteral("4:59"));
        // Other chunks (as other programs write) are skipped.
        QByteArray withList = wav.left(36) + QByteArray("LIST\x05\0\0\0abcde\0", 14) + wav.mid(36);
        set32(withList, 4, quint32(withList.size() - 8));
        QCOMPARE(Voice::fromWav(withList)->samples, samples);
    }

    // What a stranger sends: anything but a plain, consistent 16-bit WAV is refused.
    void strangersWavs()
    {
        const QByteArray good = Voice::toWav(tone(Voice::SampleRate));
        auto broken = [&](auto change) {
            QByteArray wav = good;
            change(wav);
            return !Voice::fromWav(wav).has_value();
        };
        QVERIFY(!Voice::fromWav(QByteArray()));
        QVERIFY(!Voice::fromWav(QByteArray(44, '\0')));
        QVERIFY(!Voice::fromWav(good.left(40)));
        QVERIFY(broken([](QByteArray &w) { w.replace(0, 4, "RIFX"); }));
        QVERIFY(broken([](QByteArray &w) { w.replace(8, 4, "AVI "); }));
        QVERIFY(broken([](QByteArray &w) { set32(w, 4, quint32(w.size())); }));     // longer than the file
        QVERIFY(broken([](QByteArray &w) { set16(w, 20, 3); }));                    // float
        QVERIFY(broken([](QByteArray &w) { set16(w, 34, 24); }));                   // 24-bit
        QVERIFY(broken([](QByteArray &w) { set16(w, 22, 6); }));                    // 6 channels
        QVERIFY(broken([](QByteArray &w) { set32(w, 24, 96000); }));                // 96 kHz
        QVERIFY(broken([](QByteArray &w) { set32(w, 28, 12345); }));                // inconsistent rate
        QVERIFY(broken([](QByteArray &w) { set32(w, 40, quint32(w.size())); }));    // data past the end
        QVERIFY(broken([](QByteArray &w) { set32(w, 40, 3); }));                    // half a sample
        QVERIFY(broken([](QByteArray &w) { w.replace(12, 4, "data"); }));           // data before fmt
        // Longer than voice messages are.
        const QByteArray tooLong = Voice::toWav(QByteArray((Voice::MaxSeconds + 10) * Voice::SampleRate * 2, '\0'));
        QVERIFY(!Voice::fromWav(tooLong));
        // Stereo, 48 kHz: fine.
        QByteArray stereo = good;
        set16(stereo, 22, 2);
        set32(stereo, 24, 8000);
        set32(stereo, 28, 8000 * 4);
        set16(stereo, 32, 4);
        QCOMPARE(Voice::fromWav(stereo)->channels, 2);
    }

    void conversions()
    {
        // 48 kHz stereo (as devices like it) to 16 kHz mono, and back.
        const QByteArray mono = tone(48000);
        const QByteArray stereo = Voice::convert(mono, 48000, 1, 48000, 2);
        QCOMPARE(stereo.size(), mono.size() * 2);
        const QByteArray down = Voice::convert(stereo, 48000, 2, Voice::SampleRate, 1);
        QCOMPARE(down.size(), Voice::SampleRate * 2);
        // The tone survives: the same as computed at 16 kHz, give or take.
        const QByteArray expected = tone(Voice::SampleRate);
        const auto *a = reinterpret_cast<const qint16 *>(down.constData());
        const auto *b = reinterpret_cast<const qint16 *>(expected.constData());
        for (int i = 0; i < Voice::SampleRate; i += 97)
            QVERIFY2(qAbs(a[i] - b[i]) < 400, qPrintable(QString::number(i)));
        QCOMPARE(Voice::convert(mono, 48000, 1, 48000, 1), mono);
        QVERIFY(Voice::convert(mono, 0, 1, 16000, 1).isEmpty());
        // Float and back.
        const QByteArray floats = Voice::toFloat(expected);
        QCOMPARE(floats.size(), expected.size() * 2);
        const QByteArray again = Voice::fromFloat(floats);
        const auto *c = reinterpret_cast<const qint16 *>(again.constData());
        for (int i = 0; i < Voice::SampleRate; i += 97)
            QVERIFY(qAbs(c[i] - b[i]) <= 1);
    }

    // The pointer says "voice" only for audio/wav; older versions ignore it.
    void voicePointers()
    {
        Attachment::Pointer p{QString(32, QLatin1Char('a')), QByteArray(32, 'k'), QByteArray(32, 'd'), 1000,
                              QStringLiteral("Voice message.wav"), QStringLiteral("audio/wav"), true};
        QJsonObject json = Attachment::toJson(p);
        QCOMPARE(json.value(QStringLiteral("voice")).toBool(), true);
        QVERIFY(Attachment::fromJson(json)->voice);
        p.voice = false;
        QVERIFY(!Attachment::toJson(p).contains(QStringLiteral("voice")));
        QVERIFY(!Attachment::fromJson(Attachment::toJson(p))->voice);
        json.insert(QStringLiteral("type"), QStringLiteral("image/png"));
        QVERIFY(!Attachment::fromJson(json)); // a "voice" picture
        json.insert(QStringLiteral("type"), QStringLiteral("audio/wav"));
        json.insert(QStringLiteral("voice"), QStringLiteral("yes"));
        QVERIFY(!Attachment::fromJson(json));
    }
};

CHAT_TEST_MAIN(TestVoice, QCoreApplication)
#include "tst_voice.moc"
