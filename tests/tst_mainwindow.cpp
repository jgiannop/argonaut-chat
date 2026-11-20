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
};

CHAT_TEST_MAIN(TestMainWindow, QApplication)
#include "tst_mainwindow.moc"
