#include "Attachment.h"
#include "Crypto.h"
#include "TestSupport.h"
#include "Voice.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QTest>
#include <QtEndian>

#include <algorithm>
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

    // Opus: a sixteenth of the size, the same sound back (at 48 kHz).
    void opusRoundTrip()
    {
        const QByteArray samples = chord(3.0);
        const QByteArray ogg = Voice::toOpus(samples);
        QVERIFY(ogg.startsWith("OggS"));
        QVERIFY2(ogg.size() < samples.size() / 12, qPrintable(QString::number(ogg.size())));
        QVERIFY(!ogg.contains("Voice message")); // no tags of ours

        const auto audio = Voice::fromOpus(ogg);
        QVERIFY(audio);
        QCOMPARE(audio->sampleRate, 48000);
        QCOMPARE(audio->channels, 1);
        QVERIFY(qAbs(audio->durationMs() - 3000) <= 30);
        QCOMPARE(Voice::decode(ogg)->samples, audio->samples);
        // Lossy, but the same tone: as loud, and alike sample by sample.
        const QByteArray back = Voice::convert(audio->samples, 48000, 1, Voice::SampleRate, 1);
        const auto *a = reinterpret_cast<const qint16 *>(samples.constData());
        const auto *b = reinterpret_cast<const qint16 *>(back.constData());
        double energyA = 0, energyB = 0, error = 0;
        const int n = int(std::min(samples.size(), back.size()) / 2);
        // Opus delays nothing once decoded (the pre-skip is removed), so the two line up.
        for (int i = Voice::SampleRate / 10; i < n - Voice::SampleRate / 10; ++i) {
            energyA += double(a[i]) * a[i];
            energyB += double(b[i]) * b[i];
            error += double(a[i] - b[i]) * (a[i] - b[i]);
        }
        QVERIFY(energyB > energyA * 0.7 && energyB < energyA * 1.3);
        QVERIFY2(error < energyA * 0.1, qPrintable(QString::number(error / energyA)));

        // Its length without decoding; a WAV's too.
        QVERIFY(qAbs(Voice::durationMs(ogg) - 3000) <= 30);
        QCOMPARE(Voice::durationMs(Voice::toWav(samples)), 3000);
        QCOMPARE(Voice::durationMs("neither"), -1);

        // Made in pieces while recording: the same sound as all at once.
        Voice::OpusWriter writer;
        QVERIFY(writer.isValid());
        for (qsizetype at = 0; at < samples.size(); at += 6400) // 200 ms
            QVERIFY(writer.write(samples.mid(at, 6400)));
        QCOMPARE(writer.samplesWritten(), samples.size() / 2);
        const QByteArray pieces = writer.finish();
        QVERIFY(!writer.isValid());
        QVERIFY(!writer.write(samples)); // finished
        // (Not to the bit: the encoder's analysis sees other boundaries.)
        const auto piecewise = Voice::fromOpus(pieces);
        QVERIFY(piecewise);
        QCOMPARE(piecewise->samples.size(), audio->samples.size());
        const auto *w = reinterpret_cast<const qint16 *>(audio->samples.constData());
        const auto *v = reinterpret_cast<const qint16 *>(piecewise->samples.constData());
        double whole = 0, apart = 0;
        for (qsizetype i = 0; i < audio->samples.size() / 2; ++i) {
            whole += double(w[i]) * w[i];
            apart += double(w[i] - v[i]) * (w[i] - v[i]);
        }
        QVERIFY2(apart < whole * 0.05, qPrintable(QString::number(apart / whole)));
        QVERIFY(Voice::OpusWriter().finish().isEmpty()); // nothing written
        QVERIFY(!Voice::OpusWriter(96000).isValid());

        QVERIFY(Voice::toOpus(QByteArray()).isEmpty());
        QVERIFY(Voice::toOpus(QByteArray(3, '\0')).isEmpty()); // half a sample
        QVERIFY(Voice::toOpus(samples, 96000).isEmpty());
    }

    // A stranger's Ogg: anything but one clean Opus stream, short enough, is refused.
    void strangersOpus()
    {
        const QByteArray ogg = Voice::toOpus(chord(1.0));
        QVERIFY(Voice::fromOpus(ogg));
        QVERIFY(!Voice::fromOpus(QByteArray()));
        QVERIFY(!Voice::fromOpus("OggS"));
        QVERIFY(!Voice::fromOpus(QByteArray("OggS") + Crypto::randomBytes(4000)));
        QVERIFY(!Voice::fromOpus(Voice::toWav(chord(1.0)))); // a WAV is not Ogg
        QVERIFY(!Voice::decode("nothing at all"));
        QVERIFY(Voice::decode(Voice::toWav(chord(1.0)))); // WAV from before 0.3.2 still plays
        // Cut short, or a byte changed in the middle (Ogg's checksum finds it).
        QVERIFY(!Voice::fromOpus(ogg.left(ogg.size() / 2)));
        QByteArray changed = ogg;
        changed[changed.size() / 2] = char(changed.at(changed.size() / 2) ^ 0x40);
        QVERIFY(!Voice::fromOpus(changed));
        QCOMPARE(Voice::durationMs(changed), -1);
        // Two streams one after the other (they could differ in channels).
        QVERIFY(!Voice::fromOpus(ogg + Voice::toOpus(chord(1.0))));
        // Longer than a recording can be.
        const QByteArray tooLong = Voice::toOpus(QByteArray(qsizetype(Voice::MaxSeconds + 6) * Voice::SampleRate * 2, '\0'));
        QVERIFY(!tooLong.isEmpty());
        QVERIFY(!Voice::fromOpus(tooLong));
    }

    // The pointer says "voice" only for audio/wav (what older versions know),
    // "voiceNote" only for audio/ogg (which they ignore).
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

        Attachment::Pointer opus{QString(32, QLatin1Char('a')), QByteArray(32, 'k'), QByteArray(32, 'd'), 1000,
                                 QStringLiteral("Voice message.ogg"), QStringLiteral("audio/ogg"), true};
        json = Attachment::toJson(opus);
        QVERIFY(!json.contains(QStringLiteral("voice"))); // 0.3.1 would drop the message
        QCOMPARE(json.value(QStringLiteral("voiceNote")).toBool(), true);
        QVERIFY(Attachment::fromJson(json)->voice);
        QJsonObject wrong = json;
        wrong.insert(QStringLiteral("type"), QStringLiteral("audio/wav"));
        QVERIFY(!Attachment::fromJson(wrong)); // "voiceNote" with a WAV
        wrong = json;
        wrong.insert(QStringLiteral("voice"), true);
        QVERIFY(!Attachment::fromJson(wrong)); // "voice" with Ogg, and both
        wrong = json;
        wrong.insert(QStringLiteral("voiceNote"), 1);
        QVERIFY(!Attachment::fromJson(wrong));
        wrong.insert(QStringLiteral("voiceNote"), false);
        QVERIFY(!Attachment::fromJson(wrong)->voice);
    }

private:
    // Mono 16-bit at Voice::SampleRate: a 440 Hz tone with a slower one under it.
    static QByteArray chord(double seconds)
    {
        const int n = int(seconds * Voice::SampleRate);
        QByteArray out(qsizetype(n) * 2, Qt::Uninitialized);
        auto *pcm = reinterpret_cast<qint16 *>(out.data());
        for (int i = 0; i < n; ++i) {
            const double t = double(i) / Voice::SampleRate;
            pcm[i] = qint16(6000 * std::sin(2 * 3.141592653589793 * 440 * t) + 3000 * std::sin(2 * 3.141592653589793 * 150 * t));
        }
        return out;
    }
};

CHAT_TEST_MAIN(TestVoice, QCoreApplication)
#include "tst_voice.moc"
