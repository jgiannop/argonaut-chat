#include "ChatServer.h"
#include "ChatStore.h"
#include "Dialogs.h"
#include "KeyStore.h"
#include "MainWindow.h"
#include "Messenger.h"
#include "ServerStore.h"
#include "TestSupport.h"

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QTest>

class TestMainWindow : public QObject
{
    Q_OBJECT

private slots:
    void writeANoteToSelf()
    {
        ChatStore store(QDir::homePath() + QStringLiteral("/window.sqlite"));
        MainWindow window(&store);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // Note to Self is there from the start, and open.
        QCOMPARE(window.conversations()->count(), 1);
        QCOMPARE(window.currentConversation(), store.noteToSelf());

        QTest::keyClicks(window.composeField(), QStringLiteral("Buy bread"));
        QTest::keyClick(window.composeField(), Qt::Key_Return, Qt::ShiftModifier); // a new line, not sent
        QTest::keyClicks(window.composeField(), QStringLiteral("and milk"));
        QCOMPARE(window.messages()->count(), 0);
        QTest::keyClick(window.composeField(), Qt::Key_Return); // sent
        QCOMPARE(window.messages()->count(), 1);
        QCOMPARE(window.messages()->item(0)->text(), QStringLiteral("Buy bread\nand milk"));
        QVERIFY(window.composeField()->toPlainText().isEmpty());
        QCOMPARE(store.messages(store.noteToSelf()).size(), 1);
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("Anna"), QStringLiteral("anna"));
            store.addMessage(anna, QStringLiteral("Are we still on for Sunday?"), QStringLiteral("anna"));
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
    }

    void othersMessagesAreUnreadUntilOpened()
    {
        ChatStore store(QDir::homePath() + QStringLiteral("/unread.sqlite"));
        MainWindow window(&store);
        window.show();
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("Anna"), QStringLiteral("anna"));
        store.addMessage(anna, QStringLiteral("hello"), QStringLiteral("anna"));
        QCOMPARE(store.conversation(anna).unread, 1);
        window.openConversation(anna);
        QCOMPARE(store.conversation(anna).unread, 0);
        QCOMPARE(window.messages()->count(), 1);
    }

    // Alice in the window, Bob without one, through the real server.
    void chatThroughTheServer()
    {
        ServerStore serverStore(QDir::homePath() + QStringLiteral("/server.sqlite"));
        ChatServer server(&serverStore);
        QVERIFY(server.listen());
        const QString url = QStringLiteral("ws://127.0.0.1:%1").arg(server.port());

        ChatStore store(QDir::homePath() + QStringLiteral("/alice.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.connectionLabel()->text().contains("sign up"));

        // Signing up through the dialog.
        SignUpDialog dialog(&messenger, &window);
        QVERIFY(!dialog.signUpButton()->isEnabled());
        dialog.serverField()->setText(url);
        dialog.usernameField()->setText(QStringLiteral("Alice"));
        QVERIFY(dialog.signUpButton()->isEnabled());
        dialog.signUpButton()->click();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 5000);
        QTRY_VERIFY(window.connectionLabel()->text().contains("@alice"));
        QVERIFY(window.connectionLabel()->text().contains("connected"));

        ChatStore bobStore(QDir::homePath() + QStringLiteral("/bob.sqlite"));
        KeyStore bobKeys(&bobStore);
        Messenger bob(&bobStore, &bobKeys);
        std::optional<bool> signedUp;
        bob.signUp(QUrl(url), QStringLiteral("bob"), [&](bool ok, const QString &) { signedUp = ok; });
        QTRY_VERIFY(signedUp.has_value());
        QVERIFY(*signedUp);

        // Nobody by that name: said, nothing opened.
        window.startChatWith(QStringLiteral("nobody"));
        QTRY_VERIFY(window.noticeLabel()->isVisible());
        QVERIFY(window.noticeLabel()->text().contains("nobody"));

        // A new chat with Bob, and a message to him.
        window.startChatWith(QStringLiteral("bob"));
        QTRY_VERIFY(store.directConversation(QStringLiteral("bob")) > 0);
        QTRY_COMPARE(window.currentConversation(), store.directConversation(QStringLiteral("bob")));
        QVERIFY(window.safetyNumberButton()->isVisible());
        QTest::keyClicks(window.composeField(), QStringLiteral("Hello Bob"));
        QTest::keyClick(window.composeField(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(bobStore.directConversation(QStringLiteral("alice")) > 0, 5000);
        const qint64 toAlice = bobStore.directConversation(QStringLiteral("alice"));
        QTRY_COMPARE_WITH_TIMEOUT(bobStore.messages(toAlice).size(), 1, 5000);
        QCOMPARE(bobStore.messages(toAlice).first().body, QStringLiteral("Hello Bob"));

        // His answer shows in the window.
        bob.send(toAlice, QStringLiteral("Hi Alice"));
        QTRY_COMPARE_WITH_TIMEOUT(window.messages()->count(), 2, 5000);
        QCOMPARE(window.messages()->item(1)->text(), QStringLiteral("Hi Alice"));

        // The safety number, and verifying it.
        SafetyNumberDialog safety(&messenger, QStringLiteral("bob"), &window);
        const QString shown = safety.numberLabel()->text();
        QCOMPARE(shown.count(QLatin1Char('\n')), 2); // three lines
        QCOMPARE(QString(shown).remove(QLatin1Char(' ')).remove(QLatin1Char('\n')), bob.safetyNumber("alice"));
        safety.verifyButton()->click();
        QVERIFY(messenger.isVerified(QStringLiteral("bob")));

        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
    }

    // Strangers write first: requests, accepted or blocked from the window.
    void messageRequests()
    {
        ServerStore serverStore(QDir::homePath() + QStringLiteral("/server2.sqlite"));
        ChatServer server(&serverStore);
        QVERIFY(server.listen());
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(server.port()));
        auto signUp = [&](Messenger &m, const QString &name) {
            std::optional<bool> ok;
            m.signUp(url, name, [&](bool done, const QString &) { ok = done; });
            return QTest::qWaitFor([&] { return ok.has_value(); }, 5000) && *ok;
        };

        ChatStore store(QDir::homePath() + QStringLiteral("/me.sqlite"));
        KeyStore keys(&store);
        Messenger me(&store, &keys);
        QVERIFY(signUp(me, QStringLiteral("myself")));
        MainWindow window(&store, &me);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        ChatStore annaStore(QDir::homePath() + QStringLiteral("/anna.sqlite"));
        KeyStore annaKeys(&annaStore);
        Messenger anna(&annaStore, &annaKeys);
        QVERIFY(signUp(anna, QStringLiteral("anna")));
        ChatStore spamStore(QDir::homePath() + QStringLiteral("/spam.sqlite"));
        KeyStore spamKeys(&spamStore);
        Messenger spam(&spamStore, &spamKeys);
        QVERIFY(signUp(spam, QStringLiteral("spammer")));

        auto writeTo = [](Messenger &from, ChatStore &fromStore, const QString &text) {
            std::optional<qint64> conversation;
            from.startConversation(QStringLiteral("myself"), [&](qint64 id, const QString &) { conversation = id; });
            if (!QTest::qWaitFor([&] { return conversation.has_value(); }, 5000) || *conversation == 0)
                return qint64(0);
            return from.send(*conversation, text) ? *conversation : 0;
        };
        const qint64 annaToMe = writeTo(anna, annaStore, QStringLiteral("Hi, it's Anna"));
        QVERIFY(annaToMe > 0);
        QTRY_VERIFY_WITH_TIMEOUT(store.directConversation(QStringLiteral("anna")) > 0, 5000);
        const qint64 fromAnna = store.directConversation(QStringLiteral("anna"));
        QTRY_COMPARE_WITH_TIMEOUT(store.messages(fromAnna).size(), 1, 5000);

        // Opened: the request bar, not the composer; Anna still sees only ✓.
        window.openConversation(fromAnna);
        QVERIFY(window.requestBar()->isVisible());
        QVERIFY(!window.composer()->isVisible());
        QCOMPARE(window.messages()->count(), 1);
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
        const qint64 annaMessage = annaStore.messages(annaToMe).first().id;
        QTest::qWait(200);
        QCOMPARE(annaStore.message(annaMessage).status, QStringLiteral("sent"));

        // Accepted: a chat like any other, and Anna sees it read.
        window.findChild<QPushButton *>(QStringLiteral("acceptButton"))->click();
        QVERIFY(!window.requestBar()->isVisible());
        QVERIFY(window.composer()->isVisible());
        QVERIFY(!store.conversation(fromAnna).request);
        QTRY_COMPARE_WITH_TIMEOUT(annaStore.message(annaMessage).status, QStringLiteral("read"), 5000);

        // The spammer: blocked from the bar, then unblocked in Blocked People.
        QVERIFY(writeTo(spam, spamStore, QStringLiteral("Buy now")) > 0);
        QTRY_VERIFY_WITH_TIMEOUT(store.directConversation(QStringLiteral("spammer")) > 0, 5000);
        window.openConversation(store.directConversation(QStringLiteral("spammer")));
        QVERIFY(window.requestBar()->isVisible());
        window.blockRequest(false);
        QCOMPARE(store.directConversation(QStringLiteral("spammer")), 0);
        QCOMPARE(window.currentConversation(), store.noteToSelf());
        BlockedDialog blocked(&me, &window);
        QCOMPARE(blocked.list()->count(), 1);
        QCOMPARE(blocked.list()->item(0)->text(), QStringLiteral("spammer"));
        QVERIFY(!blocked.unblockButton()->isEnabled());
        blocked.list()->setCurrentRow(0);
        blocked.unblockButton()->click();
        QCOMPARE(blocked.list()->count(), 0);
        QVERIFY(!me.isBlocked(QStringLiteral("spammer")));
    }
};

CHAT_TEST_MAIN(TestMainWindow, QApplication)
#include "tst_mainwindow.moc"
