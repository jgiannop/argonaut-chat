#include "ChatStore.h"
#include "MainWindow.h"
#include "TestSupport.h"

#include <QApplication>
#include <QDir>
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
};

CHAT_TEST_MAIN(TestMainWindow, QApplication)
#include "tst_mainwindow.moc"
