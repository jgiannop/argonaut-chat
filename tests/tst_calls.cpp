#include "CallAudio.h"
#include "CallConnection.h"
#include "Calls.h"
#include "ChatServer.h"
#include "Messenger.h"
#include "ServerStore.h"
#include "TestSupport.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>

#include <cmath>
#include <memory>

namespace {

constexpr int kWait = 10000;

struct Device
{
    Device(const QString &file, const QString &name)
        : name(name)
    {
        store = std::make_unique<ChatStore>(file);
        keys = std::make_unique<KeyStore>(store.get());
        messenger = std::make_unique<Messenger>(store.get(), keys.get());
        messenger->setReconnectDelay(50);
        calls = std::make_unique<Calls>(messenger.get());
        calls->setDevicesEnabled(false);
        calls->setProcessingForTests(false);
    }
    ~Device()
    {
        calls.reset();
        messenger.reset();
        keys.reset();
        store.reset();
    }
    // The newest note (a system message) in the conversation with `peer`.
    QString lastNote(const QString &peer) const
    {
        const qint64 conversation = store->directConversation(peer);
        const auto list = store->messages(conversation);
        for (auto it = list.crbegin(); it != list.crend(); ++it) {
            if (it->system)
                return it->body;
        }
        return {};
    }

    QString name;
    std::unique_ptr<ChatStore> store;
    std::unique_ptr<KeyStore> keys;
    std::unique_ptr<Messenger> messenger;
    std::unique_ptr<Calls> calls;
};

// 20 ms of a tone, going on from `phase` (samples so far).
QByteArray tone(double hz, qint64 *phase)
{
    QByteArray frame(CallAudio::FrameBytes, '\0');
    auto *samples = reinterpret_cast<qint16 *>(frame.data());
    for (int i = 0; i < CallAudio::FrameSamples; ++i, ++*phase)
        samples[i] = qint16(8000 * std::sin(2 * 3.14159265358979323846 * hz * double(*phase) / CallAudio::SampleRate));
    return frame;
}

double rms(const QByteArray &frame)
{
    const auto *samples = reinterpret_cast<const qint16 *>(frame.constData());
    const int count = int(frame.size() / 2);
    double sum = 0;
    for (int i = 0; i < count; ++i)
        sum += double(samples[i]) * samples[i];
    return count ? std::sqrt(sum / count) : 0;
}

} // namespace

class TestCalls : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        m_serverStore =
            std::make_unique<ServerStore>(m_dir.filePath(QStringLiteral("server-%1/server.sqlite").arg(++m_count)));
        m_server = std::make_unique<ChatServer>(m_serverStore.get());
        ChatServer::Limits limits;
        limits.sendsPerAccount = {10000, 10000};
        m_server->setLimits(limits);
        QVERIFY(m_server->listen());
        m_url = QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(m_server->port()));
    }

    void cleanup()
    {
        m_server.reset();
        m_serverStore.reset();
    }

    void rtpPackets()
    {
        CallConnection::Rtp packet;
        packet.sequence = 65535;
        packet.timestamp = 0x12345678;
        packet.ssrc = 0xdeadbeef;
        packet.payloadType = CallConnection::OpusPayloadType;
        packet.payload = QByteArray("opus bytes");
        const QByteArray wire = CallConnection::buildRtp(packet);
        QCOMPARE(wire.size(), 12 + 10);
        CallConnection::Rtp back;
        QVERIFY(CallConnection::parseRtp(wire, &back));
        QCOMPARE(back.sequence, packet.sequence);
        QCOMPARE(back.timestamp, packet.timestamp);
        QCOMPARE(back.ssrc, packet.ssrc);
        QCOMPARE(back.payloadType, packet.payloadType);
        QCOMPARE(back.payload, packet.payload);

        // A stranger's: everything checked against its length.
        QVERIFY(!CallConnection::parseRtp(wire.left(11), &back));
        QByteArray badVersion = wire;
        badVersion[0] = char(0x40);
        QVERIFY(!CallConnection::parseRtp(badVersion, &back));
        QByteArray csrcs = wire.left(12);
        csrcs[0] = char(0x8f); // 15 CSRCs, none there
        QVERIFY(!CallConnection::parseRtp(csrcs, &back));
        QByteArray extension = wire.left(12) + QByteArray::fromHex("bede00ff");
        extension[0] = char(0x90); // 255 words of extension, none there
        QVERIFY(!CallConnection::parseRtp(extension, &back));
        QByteArray padded = wire;
        padded[0] = char(0xa0);
        padded[padded.size() - 1] = char(200); // more padding than packet
        QVERIFY(!CallConnection::parseRtp(padded, &back));
        padded[padded.size() - 1] = char(2);
        QVERIFY(CallConnection::parseRtp(padded, &back));
        QCOMPARE(back.payload, QByteArray("opus byt"));
        QByteArray withExtension = wire.left(12) + QByteArray::fromHex("bede0001aabbccdd") + "x";
        withExtension[0] = char(0x90);
        QVERIFY(CallConnection::parseRtp(withExtension, &back));
        QCOMPARE(back.payload, QByteArray("x"));
        QVERIFY(!CallConnection::parseRtp(wire.left(12) + QByteArray(CallConnection::MaxPayloadBytes + 1, 'x'), &back));
    }

    void relayUrls()
    {
        const auto servers = CallConnection::iceServers(
            {"turn:relay.example.org:3478", "turn:[2001:db8::1]:3478?transport=tcp", "stun:relay.example.org",
             "turns:relay.example.org:5349", "http://evil", "turn:relay.example.org:99999", "turn:a b:1"},
            "1700000000:alice", "secret");
        QCOMPARE(servers.size(), 2);
        QCOMPARE(servers[0].host, QString("relay.example.org"));
        QCOMPARE(servers[0].port, quint16(3478));
        QVERIFY(servers[0].turn);
        QVERIFY(!servers[0].tcp);
        QCOMPARE(servers[0].username, QString("1700000000:alice"));
        QVERIFY(!servers[1].turn); // the TCP one left out (UDP only)
        QCOMPARE(servers[1].port, quint16(3478));
        const auto v6 = CallConnection::iceServers({"turn:[2001:db8::1]:3479"}, "u", "p");
        QCOMPARE(v6.size(), 1);
        QCOMPARE(v6[0].host, QString("2001:db8::1"));
        QCOMPARE(v6[0].port, quint16(3479));
        // TURN without a password: left out.
        QCOMPARE(CallConnection::iceServers({"turn:relay.example.org:3478"}, {}, {}).size(), 0);
    }

    void jitterBuffer()
    {
        CallAudio sender;
        CallAudio receiver;
        sender.setProcessing(false);
        receiver.setProcessing(false);
        QVERIFY(sender.isValid());
        qint64 phase = 0;
        QList<QByteArray> packets;
        for (int i = 0; i < 40; ++i)
            packets << sender.encode(tone(440, &phase), false);
        for (const QByteArray &packet : packets)
            QVERIFY(!packet.isEmpty() && packet.size() <= CallAudio::MaxPacketBytes);

        // Silence until a few have come.
        QCOMPARE(rms(receiver.nextFrame()), 0.0);
        const quint16 start = 65530; // wraps around 65535
        receiver.received(start, packets[0]);
        receiver.received(quint16(start + 2), packets[2]); // out of order
        QVERIFY(!receiver.isPlaying());
        receiver.received(quint16(start + 1), packets[1]);
        QVERIFY(receiver.isPlaying());
        receiver.received(quint16(start + 1), packets[1]); // twice: once
        QCOMPARE(receiver.bufferedFrames(), 3);
        double sound = 0;
        for (int i = 0; i < 3; ++i)
            sound += rms(receiver.nextFrame());
        QVERIFY(sound > 1000);
        QCOMPARE(receiver.concealedFrames(), 0);
        // One lost (start + 3): rebuilt from the next one's FEC.
        receiver.received(quint16(start + 4), packets[4]);
        receiver.received(quint16(start + 5), packets[5]);
        QVERIFY(rms(receiver.nextFrame()) > 500);
        QCOMPARE(receiver.concealedFrames(), 1);
        QVERIFY(rms(receiver.nextFrame()) > 500);
        QVERIFY(rms(receiver.nextFrame()) > 500);
        // Too late: dropped.
        receiver.received(quint16(start + 3), packets[3]);
        QCOMPARE(receiver.bufferedFrames(), 0);

        // Far behind: the oldest go, so it never lags more than MaxBufferedFrames.
        for (int i = 6; i < 40; ++i)
            receiver.received(quint16(start + i), packets[i]);
        QCOMPARE(receiver.bufferedFrames(), CallAudio::MaxBufferedFrames);

        // Rubbish: not a 20 ms Opus frame.
        receiver.received(quint16(start + 41), QByteArray(3, '\xff'));
        receiver.received(quint16(start + 42), QByteArray());
        QCOMPARE(receiver.bufferedFrames(), CallAudio::MaxBufferedFrames);

        // Muted: silence goes out, small.
        const QByteArray quiet = sender.encode(tone(440, &phase), true);
        QVERIFY(!quiet.isEmpty() && quiet.size() < packets[5].size());
    }

    void callThroughTheServer()
    {
        Device alice(file("alice"), "alice");
        Device bob(file("bob"), "bob");
        QVERIFY(friends(alice, bob));
        const qint64 toBob = alice.store->directConversation("bob");

        QSignalSpy ringing(bob.calls.get(), &Calls::incomingCall);
        QString error;
        QVERIFY2(alice.calls->call(toBob, &error), qPrintable(error));
        QCOMPARE(alice.calls->state(), Calls::State::Calling);
        QVERIFY(!alice.calls->call(toBob, &error)); // one at a time
        QTRY_COMPARE_WITH_TIMEOUT(ringing.size(), 1, kWait);
        QCOMPARE(ringing.first().first().toString(), QString("alice"));
        QCOMPARE(bob.calls->state(), Calls::State::Ringing);
        QVERIFY(bob.calls->isIncoming());
        QCOMPARE(bob.calls->callId(), alice.calls->callId());

        bob.calls->accept();
        QTRY_COMPARE_WITH_TIMEOUT(alice.calls->state(), Calls::State::Active, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Active, kWait);

        // Alice speaks (a tone), Bob hears it; and the other way.
        qint64 alicePhase = 0;
        qint64 bobPhase = 0;
        double bobHeard = 0;
        double aliceHeard = 0;
        QTimer clock;
        clock.setInterval(20);
        connect(&clock, &QTimer::timeout, this, [&] {
            alice.calls->pushMicrophoneFrame(tone(440, &alicePhase));
            bob.calls->pushMicrophoneFrame(tone(660, &bobPhase));
            if (bob.calls->audio())
                bobHeard = qMax(bobHeard, rms(bob.calls->audio()->nextFrame()));
            if (alice.calls->audio())
                aliceHeard = qMax(aliceHeard, rms(alice.calls->audio()->nextFrame()));
        });
        clock.start();
        QTRY_VERIFY_WITH_TIMEOUT(bobHeard > 2000 && aliceHeard > 2000, kWait);

        // Muted: Bob hears silence.
        alice.calls->setMuted(true);
        QVERIFY(alice.calls->isMuted());
        QTest::qWait(600);
        bobHeard = 0;
        QTest::qWait(400);
        QVERIFY2(bobHeard < 50, qPrintable(QString::number(bobHeard)));
        alice.calls->setMuted(false);
        QTRY_VERIFY_WITH_TIMEOUT(bobHeard > 2000, kWait);
        QVERIFY(alice.calls->activeMs() > 0);

        // Bob hangs up: both end, with a note.
        QSignalSpy aliceEnded(alice.calls.get(), &Calls::ended);
        bob.calls->hangUp();
        QCOMPARE(bob.calls->state(), Calls::State::Idle);
        QTRY_COMPARE_WITH_TIMEOUT(alice.calls->state(), Calls::State::Idle, kWait);
        QCOMPARE(aliceEnded.size(), 1);
        clock.stop();
        QVERIFY2(alice.lastNote("bob").startsWith(QString::fromUtf8("Outgoing call · 0:")), qPrintable(alice.lastNote("bob")));
        QVERIFY2(bob.lastNote("alice").startsWith(QString::fromUtf8("Incoming call · 0:")), qPrintable(bob.lastNote("alice")));

        // And again, Bob calling.
        QVERIFY(bob.calls->call(bob.store->directConversation("alice")));
        QTRY_COMPARE_WITH_TIMEOUT(alice.calls->state(), Calls::State::Ringing, kWait);
        alice.calls->accept();
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Active, kWait);
        alice.calls->hangUp();
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Idle, kWait);
    }

    // Through a real TURN relay only (no direct path): run with a coturn
    // (use-auth-secret) and CHAT_TEST_RELAY=turn:host:port,
    // CHAT_TEST_RELAY_SECRET=its static-auth-secret. Skipped otherwise.
    void throughTheRelay()
    {
        const QString url = qEnvironmentVariable("CHAT_TEST_RELAY");
        const QByteArray secret = qgetenv("CHAT_TEST_RELAY_SECRET");
        if (url.isEmpty() || secret.isEmpty())
            QSKIP("no relay to test with (CHAT_TEST_RELAY, CHAT_TEST_RELAY_SECRET)");
        m_server->setRelay({url}, secret);
        Device alice(file("alice"), "alice");
        Device bob(file("bob"), "bob");
        alice.calls->setRelayOnlyForTests(true);
        bob.calls->setRelayOnlyForTests(true);
        QVERIFY(friends(alice, bob));
        QVERIFY(alice.calls->call(alice.store->directConversation("bob")));
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Ringing, kWait);
        bob.calls->accept();
        QTRY_COMPARE_WITH_TIMEOUT(alice.calls->state(), Calls::State::Active, 20000);
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Active, 20000);
        qint64 phase = 0;
        double heard = 0;
        QTimer clock;
        clock.setInterval(20);
        connect(&clock, &QTimer::timeout, this, [&] {
            alice.calls->pushMicrophoneFrame(tone(440, &phase));
            if (bob.calls->audio())
                heard = qMax(heard, rms(bob.calls->audio()->nextFrame()));
        });
        clock.start();
        QTRY_VERIFY_WITH_TIMEOUT(heard > 2000, kWait);
        alice.calls->hangUp();
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Idle, kWait);
    }

    void declinedMissedAndBusy()
    {
        Device alice(file("alice"), "alice");
        Device bob(file("bob"), "bob");
        Device carol(file("carol"), "carol");
        QVERIFY(friends(alice, bob));
        QVERIFY(friends(carol, bob));
        const qint64 toBob = alice.store->directConversation("bob");

        // Declined.
        QVERIFY(alice.calls->call(toBob));
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Ringing, kWait);
        QSignalSpy aliceEnded(alice.calls.get(), &Calls::ended);
        bob.calls->decline();
        QCOMPARE(bob.calls->state(), Calls::State::Idle);
        QCOMPARE(bob.lastNote("alice"), QString("Declined call"));
        QTRY_COMPARE_WITH_TIMEOUT(aliceEnded.size(), 1, kWait);
        QVERIFY(aliceEnded.first().at(1).toString().contains("declined"));
        QCOMPARE(alice.lastNote("bob"), QString::fromUtf8("Outgoing call · declined"));

        // Cancelled before Bob answers: missed, for him.
        QVERIFY(alice.calls->call(toBob));
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Ringing, kWait);
        alice.calls->hangUp();
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Idle, kWait);
        QCOMPARE(bob.lastNote("alice"), QString("Missed call"));
        QCOMPARE(alice.lastNote("bob"), QString::fromUtf8("Outgoing call · cancelled"));

        // Nobody answers: given up after the ringing time.
        alice.calls->setRingMsForTests(1500);
        bob.calls->setRingMsForTests(60000);
        QVERIFY(alice.calls->call(toBob));
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Ringing, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.calls->state(), Calls::State::Idle, kWait);
        QCOMPARE(alice.lastNote("bob"), QString::fromUtf8("Outgoing call · no answer"));
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Idle, kWait);
        QCOMPARE(bob.lastNote("alice"), QString("Missed call"));
        alice.calls->setRingMsForTests(Calls::RingMs);

        // Busy: Bob is talking to Alice when Carol calls.
        QVERIFY(alice.calls->call(toBob));
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Ringing, kWait);
        bob.calls->accept();
        QTRY_COMPARE_WITH_TIMEOUT(alice.calls->state(), Calls::State::Active, kWait);
        QSignalSpy carolEnded(carol.calls.get(), &Calls::ended);
        QVERIFY(carol.calls->call(carol.store->directConversation("bob")));
        QTRY_COMPARE_WITH_TIMEOUT(carolEnded.size(), 1, kWait);
        QVERIFY(carolEnded.first().at(1).toString().contains("another call"));
        QCOMPARE(carol.lastNote("bob"), QString::fromUtf8("Outgoing call · busy"));
        QCOMPARE(bob.lastNote("carol"), QString("Missed call"));
        QCOMPARE(bob.calls->state(), Calls::State::Active); // still with Alice
        alice.calls->hangUp();
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Idle, kWait);
    }

    void strangersCannotRing()
    {
        Device bob(file("bob"), "bob");
        Device mallory(file("mallory"), "mallory");
        QVERIFY(signUp(bob, "bob"));
        QVERIFY(signUp(mallory, "mallory"));
        // Mallory wrote to Bob; he has not accepted: her calls never ring.
        const qint64 toBob = startConversation(mallory, "bob");
        mallory.messenger->send(toBob, "hi");
        QTRY_VERIFY_WITH_TIMEOUT(bob.store->directConversation("mallory") > 0, kWait);
        QVERIFY(!bob.messenger->canCall(bob.store->directConversation("mallory")));
        QString error;
        QVERIFY(!bob.calls->call(bob.store->directConversation("mallory"), &error)); // nor he them, before accepting
        QSignalSpy ringing(bob.calls.get(), &Calls::incomingCall);
        QVERIFY(mallory.calls->call(toBob)); // she may try: her side is accepted
        QTest::qWait(1500);
        QCOMPARE(ringing.size(), 0);
        QCOMPARE(bob.calls->state(), Calls::State::Idle);
        mallory.calls->hangUp();
        // Note to Self and groups: no calls.
        QVERIFY(!bob.messenger->canCall(bob.store->noteToSelf()));
    }

    void myOtherDeviceStopsRinging()
    {
        Device alice(file("alice"), "alice");
        Device bob(file("bob"), "bob");
        QVERIFY(friends(alice, bob));
        Device laptop(file("laptop"), "laptop");
        std::optional<QString> code;
        std::optional<bool> linked;
        laptop.messenger->linkThisDevice(m_url, [&](const QString &shown) { code = shown; },
                                         [&](bool ok, const QString &) { linked = ok; });
        QTRY_VERIFY_WITH_TIMEOUT(code.has_value(), kWait);
        bob.messenger->linkDevice(*code, [](bool, const QString &) {});
        QTRY_COMPARE_WITH_TIMEOUT(linked, std::optional<bool>(true), kWait);
        QTRY_VERIFY_WITH_TIMEOUT(laptop.store->directConversation("alice") > 0, kWait);
        // Bob's first computer learns of the laptop.
        std::optional<bool> listed;
        bob.messenger->listDevices([&](const QList<quint32> &, const QString &) { listed = true; });
        QTRY_VERIFY_WITH_TIMEOUT(listed.has_value(), kWait);

        // Both ring; the first answers; the laptop stops.
        QVERIFY(alice.calls->call(alice.store->directConversation("bob")));
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Ringing, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(laptop.calls->state(), Calls::State::Ringing, kWait);
        bob.calls->accept();
        QTRY_COMPARE_WITH_TIMEOUT(laptop.calls->state(), Calls::State::Idle, kWait);
        QCOMPARE(laptop.lastNote("alice"), QString::fromUtf8("Incoming call · answered on another device"));
        QTRY_COMPARE_WITH_TIMEOUT(alice.calls->state(), Calls::State::Active, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Active, kWait);
        alice.calls->hangUp();
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Idle, kWait);

        // Declined on the laptop: the first stops too.
        QVERIFY(alice.calls->call(alice.store->directConversation("bob")));
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Ringing, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(laptop.calls->state(), Calls::State::Ringing, kWait);
        laptop.calls->decline();
        QTRY_COMPARE_WITH_TIMEOUT(bob.calls->state(), Calls::State::Idle, kWait);
        QTRY_COMPARE_WITH_TIMEOUT(alice.calls->state(), Calls::State::Idle, kWait);
        QCOMPARE(bob.lastNote("alice"), QString("Declined call"));
    }

private:
    QString file(const QString &name)
    {
        return m_dir.filePath(QStringLiteral("%1-%2/chat.sqlite").arg(name).arg(++m_count));
    }

    bool signUp(Device &device, const QString &username)
    {
        std::optional<bool> result;
        device.messenger->signUp(m_url, username, [&](bool ok, const QString &) { result = ok; });
        if (!QTest::qWaitFor([&] { return result.has_value(); }, kWait))
            return false;
        return *result;
    }

    qint64 startConversation(Device &device, const QString &username)
    {
        std::optional<qint64> result;
        device.messenger->startConversation(username, [&](qint64 conversation, const QString &) { result = conversation; });
        if (!QTest::qWaitFor([&] { return result.has_value(); }, kWait))
            return -1;
        return *result;
    }

    // Signed up (if not yet), and each has accepted the other.
    bool friends(Device &a, Device &b)
    {
        if (!a.messenger->hasAccount() && !signUp(a, a.name))
            return false;
        if (!b.messenger->hasAccount() && !signUp(b, b.name))
            return false;
        const qint64 toB = startConversation(a, b.name);
        if (toB <= 0)
            return false;
        a.messenger->send(toB, "hello");
        if (!QTest::qWaitFor([&] { return b.store->directConversation(a.name) > 0; }, kWait))
            return false;
        const qint64 toA = b.store->directConversation(a.name);
        b.messenger->acceptRequest(toA);
        return a.messenger->canCall(toB) && b.messenger->canCall(toA);
    }

    QTemporaryDir m_dir;
    int m_count = 0;
    std::unique_ptr<ServerStore> m_serverStore;
    std::unique_ptr<ChatServer> m_server;
    QUrl m_url;
};

CHAT_TEST_MAIN(TestCalls, QCoreApplication)
#include "tst_calls.moc"
