#include "ChatServer.h"
#include "ChatStore.h"
#include "Dialogs.h"
#include "KeyStore.h"
#include "ImageViewer.h"
#include "MainWindow.h"
#include "Protocol.h"
#include "Notifications.h"
#include "Messenger.h"
#include "ServerStore.h"
#include "TestSupport.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QMimeData>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QToolButton>
#include <QListWidget>

#include <memory>
#include <QPlainTextEdit>
#include <QMenuBar>
#include <QCheckBox>
#include <QTest>

class TestMainWindow : public QObject
{
    Q_OBJECT

private slots:
    void writeANoteToSelf()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/window.sqlite"));
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

    // Reply from a message's menu: the bar says what it answers, the bubble quotes it.
    void replyToAMessage()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/replies.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const int quoteAuthorRole = Qt::UserRole + 16;
        const int quoteTextRole = Qt::UserRole + 17;

        QTest::keyClicks(window.composeField(), QStringLiteral("Call the plumber"));
        QTest::keyClick(window.composeField(), Qt::Key_Return);
        const qint64 note = store.messages(store.noteToSelf()).last().id;
        std::unique_ptr<QMenu> menu(window.messageMenu(note));
        QAction *reply = menu->findChild<QAction *>(QStringLiteral("replyAction"));
        QVERIFY(reply && reply->isEnabled());
        reply->trigger();
        QCOMPARE(window.replyingTo(), note);
        QVERIFY(window.replyBar()->isVisible());
        QVERIFY(window.replyLabel()->text().contains(QStringLiteral("Call the plumber")));
        QVERIFY(window.replyLabel()->text().contains(QStringLiteral("You")));

        // Esc: not a reply after all.
        QTest::keyClick(window.composeField(), Qt::Key_Escape);
        QCOMPARE(window.replyingTo(), 0);
        QVERIFY(!window.replyBar()->isVisible());

        window.replyTo(note);
        QTest::keyClicks(window.composeField(), QStringLiteral("Done, Tuesday at 10"));
        QTest::keyClick(window.composeField(), Qt::Key_Return);
        QCOMPARE(window.replyingTo(), 0); // one message only
        QVERIFY(!window.replyBar()->isVisible());
        QCOMPARE(window.messages()->count(), 2);
        QListWidgetItem *bubble = window.messages()->item(1);
        QCOMPARE(bubble->data(quoteAuthorRole).toString(), QStringLiteral("You"));
        QCOMPARE(bubble->data(quoteTextRole).toString(), QStringLiteral("Call the plumber"));
        QVERIFY(window.messages()->item(0)->data(quoteAuthorRole).isNull());
        // The quote makes the bubble taller.
        QVERIFY(window.messages()->visualItemRect(bubble).height() > window.messages()->visualItemRect(window.messages()->item(0)).height());

        // A reply is to a message of the open conversation: another one ends it.
        window.replyTo(note);
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("Anna"), QStringLiteral("anna"));
        window.openConversation(anna);
        QCOMPARE(window.replyingTo(), 0);
        // A note in the conversation has no menu.
        const qint64 system = store.addSystemNote(anna, QStringLiteral("a note"), QDateTime::currentDateTimeUtc());
        QVERIFY(!std::unique_ptr<QMenu>(window.messageMenu(system)));
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            store.addMessage(anna, QStringLiteral("Are we still on for Sunday?"), QStringLiteral("anna"));
            const qint64 question = store.messages(anna).last().id;
            store.setAccepted(anna);
            window.openConversation(anna);
            window.replyTo(question);
            QTest::keyClicks(window.composeField(), QStringLiteral("Yes! 11 at the harbour?"));
            QTest::keyClick(window.composeField(), Qt::Key_Return);
            store.addMessage(anna, QStringLiteral("Perfect, see you there"), QStringLiteral("anna"));
            store.setQuote(store.messages(anna).last().id, store.messages(anna).at(store.messages(anna).size() - 2).uid,
                           QStringLiteral("me"), QStringLiteral("Yes! 11 at the harbour?"));
            window.replyTo(store.messages(anna).last().id);
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
    }

    void reactToAMessage()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/reactions.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const int reactionsRole = Qt::UserRole + 18;
        QTest::keyClicks(window.composeField(), QStringLiteral("Ran 10 km today"));
        QTest::keyClick(window.composeField(), Qt::Key_Return);
        const qint64 note = store.messages(store.noteToSelf()).last().id;
        const int plain = window.messages()->visualItemRect(window.messages()->item(0)).height();

        // React › 🎉 is not among them; 👍 is.
        auto pick = [&](const QString &emoji) {
            std::unique_ptr<QMenu> menu(window.messageMenu(note));
            QMenu *react = menu->findChild<QMenu *>(QStringLiteral("reactMenu"));
            QVERIFY(react && react->isEnabled());
            for (QAction *a : react->actions()) {
                if (a->data().toString() == emoji) {
                    a->trigger();
                    return;
                }
            }
            QFAIL("no such reaction");
        };
        pick(QStringLiteral("👍"));
        QCOMPARE(store.myReaction(note), QStringLiteral("👍"));
        QCOMPARE(window.messages()->item(0)->data(reactionsRole).toString(), QStringLiteral("👍"));
        QVERIFY(window.messages()->item(0)->toolTip().contains(QStringLiteral("You")));
        QVERIFY(window.messages()->visualItemRect(window.messages()->item(0)).height() > plain); // room for the pill
        {
            std::unique_ptr<QMenu> menu(window.messageMenu(note));
            QMenu *react = menu->findChild<QMenu *>(QStringLiteral("reactMenu"));
            QVERIFY(react->actions().first()->isChecked()); // mine, ticked
            QVERIFY(menu->findChild<QAction *>(QStringLiteral("removeReactionAction")));
        }
        // Another replaces it; the same again takes it back.
        pick(QStringLiteral("❤️"));
        QCOMPARE(store.myReaction(note), QStringLiteral("❤️"));
        pick(QStringLiteral("❤️"));
        QVERIFY(store.myReaction(note).isEmpty());
        QVERIFY(window.messages()->item(0)->data(reactionsRole).isNull());

        // Several people's: each emoji once, with how many.
        const qint64 anna = store.addConversation(QStringLiteral("group"), QStringLiteral("Runners"), QStringLiteral("0123456789abcdef0123456789abcdef"));
        const qint64 run = store.addMessage(anna, QStringLiteral("Sunday 8:00?"), QStringLiteral("anna")).id;
        store.setReaction(run, QStringLiteral("bob"), QStringLiteral("👍"), 1);
        store.setReaction(run, QStringLiteral("carol"), QStringLiteral("👍"), 2);
        store.setReaction(run, QString(), QStringLiteral("😂"), 3);
        window.openConversation(anna);
        QCOMPARE(window.messages()->item(0)->data(reactionsRole).toString(), QStringLiteral("👍 2  😂"));
        QVERIFY(window.messages()->item(0)->toolTip().contains(QStringLiteral("bob, carol")));
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
    }

    void deleteAMessage()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/deleting.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        MainWindow window(&store, &messenger);
        window.show();
        const int deletedRole = Qt::UserRole + 19;
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("anna"), QStringLiteral("anna"));
        const qint64 mine = store.addMessage(anna, QStringLiteral("See you at 5"), QString(), QDateTime::currentDateTimeUtc(),
                                             QStringLiteral("11111111-1111-4111-8111-111111111111"), QStringLiteral("sent")).id;
        const qint64 theirs = store.addMessage(anna, QStringLiteral("ok"), QStringLiteral("anna")).id;
        window.openConversation(anna);
        QCOMPARE(window.messages()->count(), 2);

        // Mine: for me or for everyone; theirs: for me only.
        std::unique_ptr<QMenu> menu(window.messageMenu(mine));
        QVERIFY(menu->findChild<QAction *>(QStringLiteral("deleteForMeAction")));
        QVERIFY(menu->findChild<QAction *>(QStringLiteral("deleteForEveryoneAction")));
        menu.reset(window.messageMenu(theirs));
        QVERIFY(menu->findChild<QAction *>(QStringLiteral("deleteForMeAction")));
        QVERIFY(!menu->findChild<QAction *>(QStringLiteral("deleteForEveryoneAction")));

        window.deleteMessage(theirs, false, false);
        QCOMPARE(window.messages()->count(), 1);
        window.deleteMessage(mine, true, false);
        QVERIFY(store.message(mine).deleted);
        QCOMPARE(window.messages()->count(), 1);
        QCOMPARE(window.messages()->item(0)->text(), QStringLiteral("This message was deleted"));
        QVERIFY(window.messages()->item(0)->data(deletedRole).toBool());
        QCOMPARE(window.conversations()->item(0)->data(Qt::UserRole + 5).toString(), QStringLiteral("This message was deleted"));
        // Its mark has Delete for Me only.
        menu.reset(window.messageMenu(mine));
        QCOMPARE(menu->actions().size(), 1);
        QCOMPARE(menu->actions().first()->objectName(), QStringLiteral("deleteForMeAction"));
    }

    void editAMessage()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/editing.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const int editedRole = Qt::UserRole + 20;
        QTest::keyClicks(window.composeField(), QStringLiteral("Buy bred"));
        QTest::keyClick(window.composeField(), Qt::Key_Return);
        const qint64 note = store.messages(store.noteToSelf()).last().id;
        std::unique_ptr<QMenu> menu(window.messageMenu(note));
        QAction *edit = menu->findChild<QAction *>(QStringLiteral("editAction"));
        QVERIFY(edit);
        edit->trigger();
        QCOMPARE(window.editing(), note);
        QCOMPARE(window.composeField()->toPlainText(), QStringLiteral("Buy bred"));
        QVERIFY(window.replyBar()->isVisible());
        QVERIFY(window.replyLabel()->text().startsWith(QStringLiteral("Editing")));
        // Esc: as it was, the field empty again.
        QTest::keyClick(window.composeField(), Qt::Key_Escape);
        QCOMPARE(window.editing(), 0);
        QVERIFY(window.composeField()->toPlainText().isEmpty());
        QCOMPARE(store.message(note).body, QStringLiteral("Buy bred"));

        window.startEdit(note);
        QTest::keyClick(window.composeField(), Qt::Key_Backspace);
        QTest::keyClick(window.composeField(), Qt::Key_Backspace);
        QTest::keyClicks(window.composeField(), QStringLiteral("ead"));
        QTest::keyClick(window.composeField(), Qt::Key_Return);
        QCOMPARE(store.message(note).body, QStringLiteral("Buy bread"));
        QCOMPARE(window.editing(), 0);
        QVERIFY(window.composeField()->toPlainText().isEmpty());
        QCOMPARE(window.messages()->count(), 1); // the same message, not a new one
        QCOMPARE(window.messages()->item(0)->text(), QStringLiteral("Buy bread"));
        QVERIFY(window.messages()->item(0)->data(editedRole).toBool());
        // Theirs: no Edit.
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("anna"), QStringLiteral("anna"));
        const qint64 theirs = store.addMessage(anna, QStringLiteral("hi"), QStringLiteral("anna")).id;
        window.openConversation(anna);
        menu.reset(window.messageMenu(theirs));
        QVERIFY(!menu->findChild<QAction *>(QStringLiteral("editAction")));
    }

    void typingIndicatorsSetting()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/typing.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        {
            MainWindow window(&store, &messenger);
            QVERIFY(window.typingIndicatorsAction()->isChecked()); // on at first, as Signal's
            QVERIFY(messenger.typingIndicators());
            window.typingIndicatorsAction()->setChecked(false);
            QVERIFY(!messenger.typingIndicators());
        }
        // Remembered.
        MainWindow again(&store, &messenger);
        QVERIFY(!again.typingIndicatorsAction()->isChecked());
        again.typingIndicatorsAction()->setChecked(true);
        QVERIFY(messenger.typingIndicators());
    }

    void readReceiptsSetting()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/receipts.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        {
            MainWindow window(&store, &messenger);
            QVERIFY(window.readReceiptsAction()->isChecked()); // on at first, as Signal's
            QVERIFY(messenger.readReceipts());
            window.readReceiptsAction()->setChecked(false);
            QVERIFY(!messenger.readReceipts());
        }
        // Remembered.
        Messenger later(&store, &keys);
        MainWindow again(&store, &later);
        QVERIFY(!again.readReceiptsAction()->isChecked());
        QVERIFY(!later.readReceipts());
        again.readReceiptsAction()->setChecked(true);
        QVERIFY(later.readReceipts());
    }

    // The app lock: set in its dialog; at start, by hand and when idle the
    // chats are hidden until the passphrase; menus and shortcuts off meanwhile.
    void appLock()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/applock.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("anna"), QStringLiteral("anna"));
        store.setAccepted(anna);
        store.addMessage(anna, QStringLiteral("secret plans"), QStringLiteral("anna"));
        {
            MainWindow window(&store, &messenger);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QVERIFY(!window.isLocked());
            QVERIFY(!window.lockAction()->isEnabled()); // no lock yet
            window.lockNow();
            QVERIFY(!window.isLocked());

            // Turned on in its dialog: the two must match, and be long enough.
            AppLockDialog dialog(window.appLock(), &window);
            QVERIFY(dialog.currentField()->isHidden());
            QVERIFY(dialog.turnOffButton()->isHidden());
            dialog.newField()->setText(QStringLiteral("open sesame"));
            dialog.repeatField()->setText(QStringLiteral("open sesamE"));
            dialog.okButton()->click();
            QVERIFY(dialog.errorLabel()->text().contains(QStringLiteral("not the same")));
            dialog.repeatField()->setText(QStringLiteral("open sesame"));
            dialog.idleBox()->setCurrentIndex(dialog.idleBox()->findData(15));
            dialog.okButton()->click();
            QCOMPARE(dialog.result(), int(QDialog::Accepted));
            QVERIFY(window.appLock()->isSet());
            QCOMPARE(window.appLock()->idleMinutes(), 15);
        }
        // Started again: locked at once, nothing of the chats showing.
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.isLocked());
        QVERIFY(window.lockScreen()->isVisible());
        QVERIFY(!window.conversations()->isVisible());
        QVERIFY(!window.messages()->isVisible());
        QVERIFY(!window.menuBar()->isVisible());
        QVERIFY(window.lockAction()->isEnabled());
        // Nothing marked read while locked; notifications without who or what.
        window.openConversation(anna);
        QCOMPARE(store.conversation(anna).unread, 1);
        QSignalSpy notified(&window, &MainWindow::notified);
        emit messenger.messageReceived(anna, QStringLiteral("anna"), QStringLiteral("meet at 5"), false, false);
        QCOMPARE(notified.size(), 1);
        QCOMPARE(notified.at(0).at(1).toString(), QStringLiteral("Argonaut Chat"));
        QCOMPARE(notified.at(0).at(2).toString(), QStringLiteral("New message"));
        // Shortcuts do nothing (Ctrl+F would open the find bar).
        QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(window.findBar()->isHidden());
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }

        // A wrong passphrase: still locked, and it says so; then the right one.
        auto *field = window.lockScreen()->findChild<QLineEdit *>(QStringLiteral("lockField"));
        auto *message = window.lockScreen()->findChild<QLabel *>(QStringLiteral("lockMessage"));
        QVERIFY(field && message);
        field->setText(QStringLiteral("wrong"));
        QTest::keyClick(field, Qt::Key_Return);
        QVERIFY(window.isLocked());
        QCOMPARE(message->text(), QStringLiteral("Wrong passphrase."));
        QVERIFY(field->text().isEmpty());
        field->setText(QStringLiteral("open sesame"));
        QTest::keyClick(field, Qt::Key_Return);
        QVERIFY(!window.isLocked());
        QVERIFY(window.conversations()->isVisible());
        QVERIFY(window.menuBar()->isVisible());
        QTRY_COMPARE(store.conversation(anna).unread, 0); // seen now

        // Ctrl+L locks; a dialog open then closes.
        window.activateWindow();
        QTRY_VERIFY(window.isActiveWindow());
        QTest::keyClick(window.composeField(), Qt::Key_L, Qt::ControlModifier);
        QVERIFY(window.isLocked());
        QVERIFY(window.unlock(QStringLiteral("open sesame")));
        auto *dialog = new AppLockDialog(window.appLock(), &window);
        dialog->show();
        window.lockNow();
        QVERIFY(!dialog->isVisible());
        QVERIFY(window.unlock(QStringLiteral("open sesame")));

        // Idle: locked after the time without a key or click; a key starts it again.
        window.setIdleLockMsForTests(400);
        QTest::qWait(250);
        QTest::keyClick(window.composeField(), Qt::Key_A);
        QTest::qWait(250);
        QVERIFY(!window.isLocked());
        QTRY_VERIFY_WITH_TIMEOUT(window.isLocked(), 2000);
        QVERIFY(window.unlock(QStringLiteral("open sesame")));

        // Turned off in the dialog, with the passphrase only.
        AppLockDialog off(window.appLock(), &window);
        QVERIFY(off.currentField()->isVisible() || !off.currentField()->isHidden());
        off.turnOffButton()->click();
        QVERIFY(window.appLock()->isSet()); // no passphrase given
        off.currentField()->setText(QStringLiteral("open sesame"));
        off.turnOffButton()->click();
        QVERIFY(!window.appLock()->isSet());
    }

    // @mentions in a group: "@" lists the members, one picked is sent as a mention.
    void mentionsInAGroup()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/mentions.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        ChatStore::Group g;
        g.id = Protocol::newGroupId();
        g.name = QStringLiteral("Trip");
        g.admin = QStringLiteral("anna");
        g.members = {QStringLiteral("anna"), QStringLiteral("bob"), QStringLiteral("carl")};
        g.revision = 1;
        const qint64 trip = store.addGroup(g, false);
        store.setProfile(QStringLiteral("anna"), QStringLiteral("Anna Smith"), std::nullopt);
        store.setProfile(QStringLiteral("bob"), QStringLiteral("Bob"), std::nullopt);
        const qint64 direct = store.addConversation(QStringLiteral("direct"), QStringLiteral("bob"), QStringLiteral("bob"));
        store.setAccepted(direct);
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *list = window.findChild<QListWidget *>(QStringLiteral("mentionList"));
        QVERIFY(list);
        QPlainTextEdit *field = window.composeField();

        // In a direct chat: no list.
        window.openConversation(direct);
        field->setFocus();
        QTest::keyClicks(field, QStringLiteral("@"));
        QVERIFY(list->isHidden());
        field->clear();

        window.openConversation(trip);
        field->setFocus();
        QTest::keyClicks(field, QStringLiteral("Hi @"));
        QVERIFY(list->isVisible());
        QCOMPARE(list->count(), 3);
        QTest::keyClicks(field, QStringLiteral("sm")); // her surname
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->text(), QStringLiteral("Anna Smith  @anna"));
        QTest::keyClick(field, Qt::Key_Return); // takes it, doesn't send
        QCOMPARE(field->toPlainText(), QStringLiteral("Hi @Anna Smith "));
        QVERIFY(list->isHidden());
        QCOMPARE(store.messages(trip).size(), 0);
        QTest::keyClicks(field, QStringLiteral("and @"));
        QCOMPARE(list->count(), 3);
        QTest::keyClick(field, Qt::Key_Down); // Bob
        QTest::keyClick(field, Qt::Key_Tab);
        QCOMPARE(field->toPlainText(), QStringLiteral("Hi @Anna Smith and @Bob "));
        // Esc: away until another "@".
        QTest::keyClicks(field, QStringLiteral("@c"));
        QVERIFY(list->isVisible());
        QTest::keyClick(field, Qt::Key_Escape);
        QVERIFY(list->isHidden());
        QTest::keyClicks(field, QStringLiteral("a"));
        QVERIFY(list->isHidden());
        QTest::keyClicks(field, QStringLiteral(" @"));
        QVERIFY(list->isVisible());
        QTest::keyClick(field, Qt::Key_Backspace);
        QTest::keyClick(field, Qt::Key_Backspace);
        QTest::keyClick(field, Qt::Key_Return); // sent
        QCOMPARE(store.messages(trip).size(), 1);
        const ChatStore::Message sent = store.messages(trip).last();
        QCOMPARE(sent.body, QStringLiteral("Hi @Anna Smith and @Bob @ca"));
        QCOMPARE(sent.mentions, (QList<Protocol::Mention>{Protocol::Mention{3, 11, QStringLiteral("anna")},
                                                           Protocol::Mention{19, 4, QStringLiteral("bob")}}));
        QTRY_COMPARE(window.messages()->count(), 1);
        const int mentionsRole = Qt::UserRole + 26, mentionedRole = Qt::UserRole + 27;
        QCOMPARE(window.messages()->item(0)->data(mentionsRole).toList().size(), 2);

        // Theirs, mentioning me: marked in the list until read; notified even when muted.
        window.openConversation(store.noteToSelf()); // elsewhere: it stays unread
        const ChatStore::Message theirs = store.addMessage(trip, QStringLiteral("@You come?"), QStringLiteral("anna"));
        store.setMentions(theirs.id, {Protocol::Mention{0, 4, QStringLiteral("anna")}}, true);
        auto row = [&] {
            for (int r = 0; r < window.conversations()->count(); ++r)
                if (window.conversations()->item(r)->data(Qt::UserRole + 1).toLongLong() == trip)
                    return window.conversations()->item(r);
            return static_cast<QListWidgetItem *>(nullptr);
        };
        QVERIFY(row() && row()->data(mentionedRole).toBool());
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.openConversation(trip);
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
            window.openConversation(store.noteToSelf());
        }
        store.setMutedUntil(trip, ChatStore::MutedForever);
        QSignalSpy notified(&window, &MainWindow::notified);
        emit messenger.messageReceived(trip, QStringLiteral("anna"), QStringLiteral("psst"), false, false);
        QCOMPARE(notified.size(), 0);
        emit messenger.messageReceived(trip, QStringLiteral("anna"), QStringLiteral("@You come?"), false, true);
        QCOMPARE(notified.size(), 1);
        window.openConversation(trip);
        QVERIFY(!store.conversation(trip).mentioned);
    }

    // A click on a reply's quote goes to what it answers, outlined for a moment.
    void jumpToTheQuotedMessage()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/jump.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        MainWindow window(&store, &messenger);
        window.resize(700, 500);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const int highlightRole = Qt::UserRole + 21;
        const qint64 notes = store.noteToSelf();
        window.openConversation(notes);
        messenger.send(notes, QStringLiteral("Buy bread, milk and the long list of other things"));
        const qint64 original = store.messages(notes).last().id;
        QVERIFY(!store.message(original).uid.isEmpty()); // notes have ids too, for this
        for (int i = 0; i < 30; ++i)
            messenger.send(notes, QStringLiteral("filler %1").arg(i));
        messenger.send(notes, QStringLiteral("ok"), original);
        const qint64 reply = store.messages(notes).last().id;
        window.openConversation(notes);
        QListWidget *list = window.messages();
        QListWidgetItem *first = list->item(0);
        QVERIFY(!list->viewport()->rect().intersects(list->visualItemRect(first))); // scrolled away, at the bottom

        // The quote is the top of the newest bubble (mine: at the right).
        QListWidgetItem *last = list->item(list->count() - 1);
        QCOMPARE(last->data(Qt::UserRole + 1).toLongLong(), reply);
        const QRect r = list->visualItemRect(last);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, QPoint(r.right() - 40, r.top() + 4 + 10 + 8));
        QTRY_COMPARE(window.flashedMessage(), original);
        first = list->item(0);
        QVERIFY(first->data(highlightRole).toBool());
        QVERIFY(list->viewport()->rect().intersects(list->visualItemRect(first)));
        QTRY_COMPARE_WITH_TIMEOUT(window.flashedMessage(), 0, 3000);
        QVERIFY(!list->item(0)->data(highlightRole).toBool());

        // What it answered, deleted: nowhere to go.
        window.deleteMessage(original, false, false);
        QVERIFY(!window.jumpToQuote(reply));
        QVERIFY(window.noticeLabel()->isVisible());
        QCOMPARE(window.flashedMessage(), 0);
    }

    // Choosing messages and copying them.
    // Part of a message's text, chosen with the mouse, copied alone.
    void selectPartOfAMessage()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/selecttext.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("anna"), QStringLiteral("anna"));
        store.setAccepted(anna);
        const QString text = QStringLiteral("Hello wonderful world");
        const qint64 id = store.addMessage(anna, text, QStringLiteral("anna")).id;
        store.markRead(anna); // no "New messages" line above it
        MainWindow window(&store, &messenger);
        window.resize(1000, 640);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openConversation(anna);
        QTRY_COMPARE(window.messages()->count(), 1);
        QWidget *viewport = window.messages()->viewport();
        const QRect row = window.messages()->visualItemRect(window.messages()->item(0));
        // Theirs: the text starts 14 + 10 px in, 4 + 10 px down.
        const QFontMetrics fm(window.messages()->font());
        const int left = row.left() + 24;
        const int y = row.top() + 14 + fm.height() / 2;

        // A double-click on "wonderful": that word.
        QTest::mouseDClick(viewport, Qt::LeftButton, {}, QPoint(left + fm.horizontalAdvance(QStringLiteral("Hello wond")), y));
        QCOMPARE(window.selectedText(), QStringLiteral("wonderful"));
        QVERIFY(window.messages()->selectedItems().isEmpty()); // not the whole message
        window.messages()->setFocus();
        QTest::keyClick(window.messages(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("wonderful"));
        // A third click: the whole text.
        QTest::mouseClick(viewport, Qt::LeftButton, {}, QPoint(left + fm.horizontalAdvance(QStringLiteral("Hello wond")), y));
        QCOMPARE(window.selectedText(), text);
        QVERIFY(window.messages()->selectedItems().isEmpty());

        // Dragged from its start to the middle: "Hello won".
        auto drag = [&](const QPoint &from, const QPoint &to) {
            QTest::mousePress(viewport, Qt::LeftButton, {}, from);
            QMouseEvent move(QEvent::MouseMove, to, viewport->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(viewport, &move);
            QTest::mouseRelease(viewport, Qt::LeftButton, {}, to);
        };
        drag(QPoint(left + 1, y), QPoint(left + fm.horizontalAdvance(QStringLiteral("Hello won")), y));
        QCOMPARE(window.selectedText(), QStringLiteral("Hello won"));
        // Backwards, past the end of the bubble: from there to the end.
        drag(QPoint(left + fm.horizontalAdvance(QStringLiteral("Hello ")), y), QPoint(row.right() - 5, row.bottom()));
        QCOMPARE(window.selectedText(), QStringLiteral("wonderful world"));
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB"))
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        // Its menu copies that part.
        std::unique_ptr<QMenu> menu(window.messageMenu(id));
        QVERIFY(menu);
        QPoint at(left + 5, y);
        QTimer::singleShot(100, [] {
            if (auto *open = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                if (auto *copy = open->findChild<QAction *>(QStringLiteral("copySelectionAction")))
                    copy->trigger();
                open->close();
            }
        });
        QGuiApplication::clipboard()->clear();
        emit window.messages()->customContextMenuRequested(at);
        QTRY_COMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("wonderful world"));
        // A plain click lets go; Ctrl+C then copies the whole message, as before.
        QTest::mouseClick(viewport, Qt::LeftButton, {}, QPoint(left + 3, y));
        QVERIFY(window.selectedText().isEmpty());
        QTest::mouseClick(viewport, Qt::LeftButton, {}, QPoint(row.right() - 20, row.center().y())); // beside the text
        window.messages()->setFocus();
        QTest::keyClick(window.messages(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), text);
    }

    void copyMessages()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/copy.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const qint64 notes = store.noteToSelf();
        window.openConversation(notes);
        for (const char *text : {"first line", "second\nhas two lines", "third"})
            messenger.send(notes, QString::fromUtf8(text));
        QListWidget *list = window.messages();
        QCOMPARE(list->count(), 3);
        QCOMPARE(list->selectionMode(), QAbstractItemView::ExtendedSelection);

        // A plain click marks no row (as in Signal); Ctrl+C then copies the one clicked, as it is.
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(1)).center());
        QVERIFY(!list->item(1)->isSelected());
        QVERIFY(list->selectedItems().isEmpty());
        QGuiApplication::clipboard()->clear();
        QTest::keyClick(list, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("second\nhas two lines"));
        // Ctrl+click chooses it (tinted), and another; a plain click lets both go.
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::ControlModifier, list->visualItemRect(list->item(1)).center());
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::ControlModifier, list->visualItemRect(list->item(2)).center());
        QCOMPARE(list->selectedItems().size(), 2);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(0)).center());
        QVERIFY(list->selectedItems().isEmpty());
        list->setFocus();

        // All: one line each with the time and who.
        QTest::keyClick(list, Qt::Key_A, Qt::ControlModifier);
        const QStringList lines = window.selectedMessagesText().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 4);
        QVERIFY(lines.at(0).startsWith(QLatin1Char('[')));
        QVERIFY(lines.at(0).endsWith(QStringLiteral("] You: first line")));
        QCOMPARE(lines.at(2), QStringLiteral("has two lines"));
        QVERIFY(lines.at(3).endsWith(QStringLiteral("You: third")));
        // A message arriving rebuilds the list: the choice stays.
        messenger.send(notes, QStringLiteral("fourth"));
        QCOMPARE(list->count(), 4);
        QCOMPARE(list->selectedItems().size(), 3);
        QVERIFY(!list->item(3)->isSelected());
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB"))
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));

        // Typing goes on in the field; Esc lets go.
        QTest::keyClicks(list, QStringLiteral("hi"));
        QVERIFY(window.composeField()->hasFocus());
        QCOMPARE(window.composeField()->toPlainText(), QStringLiteral("hi"));
        list->setFocus();
        QTest::keyClick(list, Qt::Key_Escape);
        QVERIFY(list->selectedItems().isEmpty());
        // A deleted one has nothing to copy.
        list->item(0)->setSelected(true);
        window.deleteMessage(list->item(0)->data(Qt::UserRole + 1).toLongLong(), false, false);
        QCOMPARE(window.selectedMessagesText(), QString());
    }

    // Files and pictures wait above the field until sent: 📎, pasted, dropped.
    void pasteAndDropFiles()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/paste.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const qint64 notes = store.noteToSelf();
        window.openConversation(notes);
        QListWidget *list = window.messages();
        QPlainTextEdit *field = window.composeField();

        // A screenshot pasted: waiting, not sent.
        QImage shot(320, 200, QImage::Format_RGB32);
        shot.fill(QColor(200, 80, 30));
        QGuiApplication::clipboard()->setImage(shot);
        field->setFocus();
        QTest::keyClick(field, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(window.pendingAttachments(), 1);
        QVERIFY(window.attachBar()->isVisible());
        QVERIFY(field->toPlainText().isEmpty());
        QCOMPARE(list->count(), 0);
        // Text pasted is text, and an office program's text with a picture of it too.
        QGuiApplication::clipboard()->setText(QStringLiteral("look"));
        QTest::keyClick(field, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(field->toPlainText(), QStringLiteral("look"));
        auto *both = new QMimeData;
        both->setText(QStringLiteral(" at this"));
        both->setImageData(shot);
        QGuiApplication::clipboard()->setMimeData(both);
        QTest::keyClick(field, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(field->toPlainText(), QStringLiteral("look at this"));
        QCOMPARE(window.pendingAttachments(), 1);

        // Enter: the picture, with the text, as a PNG of just its pixels.
        QTest::keyClick(field, Qt::Key_Return);
        QCOMPARE(window.pendingAttachments(), 0);
        QVERIFY(!window.attachBar()->isVisible());
        QVERIFY(field->toPlainText().isEmpty());
        QTRY_COMPARE(list->count(), 1);
        const ChatStore::Message sent = store.messages(notes).last();
        QCOMPARE(sent.body, QStringLiteral("look at this"));
        const QList<ChatStore::StoredAttachment> files = store.attachments(sent.id);
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.first().type, QStringLiteral("image/png"));
        QVERIFY(files.first().name.startsWith(QStringLiteral("Pasted image ")));
        QImage back;
        QVERIFY(back.loadFromData(messenger.attachmentData(files.first().id).value_or(QByteArray())));
        QCOMPARE(back.size(), shot.size());

        // Two files dropped on the conversation; one taken back with its ✕.
        const QString a = TestSupport::home() + QStringLiteral("/a.txt");
        const QString b = TestSupport::home() + QStringLiteral("/b.txt");
        for (const QString &path : {a, b}) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(path.toUtf8());
        }
        QMimeData dropped;
        dropped.setUrls({QUrl::fromLocalFile(a), QUrl::fromLocalFile(b)});
        const QPoint middle = list->viewport()->rect().center();
        QDragEnterEvent enter(middle, Qt::CopyAction, &dropped, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(middle, Qt::CopyAction, &dropped, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(window.pendingAttachments(), 2);
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            QVERIFY(window.addAttachmentData(QStringLiteral("shot.png"), [&] {
                QByteArray png;
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                shot.save(&buffer, "PNG");
                return png;
            }()));
            QTest::keyClicks(field, QStringLiteral("the notes and the picture"));
            QTest::qWait(100);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
            window.removeAttachment(2);
            field->clear();
        }
        const auto removes = window.attachBar()->findChildren<QToolButton *>(QStringLiteral("removeAttachmentButton"));
        QCOMPARE(removes.size(), 2);
        removes.first()->click();
        QCOMPARE(window.pendingAttachments(), 1);
        QTRY_COMPARE(window.attachBar()->findChildren<QToolButton *>(QStringLiteral("removeAttachmentButton")).size(), 1);
        QVERIFY(window.findChild<QPushButton *>(QStringLiteral("sendButton"))->isEnabled()); // nothing typed: still something to send
        QTest::keyClick(field, Qt::Key_Return);
        QTRY_COMPARE(list->count(), 2);
        QCOMPARE(store.attachments(store.messages(notes).last().id).first().name, QStringLiteral("b.txt"));

        // A folder: not sent, said so; text dropped is text.
        QMimeData folder;
        folder.setUrls({QUrl::fromLocalFile(TestSupport::home())});
        QVERIFY(window.takeAttachments(&folder));
        QCOMPARE(window.pendingAttachments(), 0);
        QVERIFY(window.noticeLabel()->isVisible());
        QMimeData text;
        text.setText(QStringLiteral("words"));
        QVERIFY(!window.takeAttachments(&text));
        // Kept for this conversation only.
        QVERIFY(window.addAttachment(a));
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("Anna"), QStringLiteral("anna"));
        store.setAccepted(anna);
        window.openConversation(anna);
        QCOMPARE(window.pendingAttachments(), 0);
    }

    // Mute, archive, pin: the list, the menus, notifications and the count.
    void muteArchivePin()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/mute.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        const QDateTime t0 = QDateTime::currentDateTimeUtc().addSecs(-60);
        QHash<QString, qint64> id;
        int i = 0;
        for (const QString &name : {QStringLiteral("anna"), QStringLiteral("bob"), QStringLiteral("carl")}) {
            id[name] = store.addConversation(QStringLiteral("direct"), name, name);
            store.setAccepted(id[name]);
            store.addMessage(id[name], QStringLiteral("hello"), name, t0.addSecs(++i)); // carl the newest
        }
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.openConversation(store.noteToSelf());
        QListWidget *list = window.conversations();
        auto names = [&] {
            QStringList out;
            for (int r = 0; r < list->count(); ++r) {
                const QString peer = list->item(r)->data(Qt::UserRole + 12).toString(); // PeerRole
                if (!peer.isEmpty())
                    out << peer; // Note to Self has none
            }
            return out.join(QLatin1Char(' '));
        };
        const int mutedRole = Qt::UserRole + 23, pinnedRole = Qt::UserRole + 24;
        QCOMPARE(names(), QStringLiteral("carl bob anna"));
        QCOMPARE(window.unreadCount(), 3);

        // Pin from the list's menu: on top, marked.
        std::unique_ptr<QMenu> menu(window.conversationMenu(id["anna"]));
        menu->findChild<QAction *>(QStringLiteral("pinAction"))->trigger();
        QVERIFY2(names().startsWith(QStringLiteral("anna carl bob")), qPrintable(names()));
        QVERIFY(list->item(0)->data(pinnedRole).toBool());

        // Mute bob for an hour: marked, no notification, not counted.
        menu.reset(window.conversationMenu(id["bob"]));
        auto *mute = menu->findChild<QMenu *>(QStringLiteral("muteMenu"));
        QVERIFY(mute);
        QCOMPARE(mute->actions().size(), 5);
        mute->actions().first()->trigger();
        QVERIFY(store.conversation(id["bob"]).isMuted());
        QVERIFY(store.conversation(id["bob"]).mutedUntil > QDateTime::currentMSecsSinceEpoch() + 3500 * 1000);
        QCOMPARE(window.unreadCount(), 2);
        QSignalSpy notified(&window, &MainWindow::notified);
        emit messenger.messageReceived(id["bob"], QStringLiteral("bob"), QStringLiteral("psst"), false, false);
        emit messenger.messageReceived(id["carl"], QStringLiteral("carl"), QStringLiteral("hey"), false, false);
        QCOMPARE(notified.size(), 1);
        QCOMPARE(notified.at(0).at(0).toLongLong(), id["carl"]);
        for (int r = 0; r < list->count(); ++r)
            QCOMPARE(list->item(r)->data(mutedRole).toBool(), list->item(r)->data(Qt::UserRole + 12).toString() == QLatin1String("bob"));
        // Its menu now says Unmute, with until when.
        menu.reset(window.conversationMenu(id["bob"]));
        QAction *unmute = menu->findChild<QAction *>(QStringLiteral("unmuteAction"));
        QVERIFY(unmute);
        unmute->trigger();
        QVERIFY(!store.conversation(id["bob"]).isMuted());
        QCOMPARE(window.unreadCount(), 3);
        // A mute that runs out shows so by itself.
        store.setMutedUntil(id["bob"], QDateTime::currentMSecsSinceEpoch() + 300);
        QTRY_VERIFY_WITH_TIMEOUT(!store.conversation(id["bob"]).isMuted(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (int r = 0; r < list->count(); ++r)
                if (list->item(r)->data(mutedRole).toBool())
                    return false;
            return true;
        }(), 3000);
        // Note to Self has nothing to mute.
        menu.reset(window.conversationMenu(store.noteToSelf()));
        QVERIFY(!menu->findChild<QMenu *>(QStringLiteral("muteMenu")));

        // Archive carl: out of the list, under "Archived Chats (1)".
        window.archiveConversation(id["carl"], true);
        QVERIFY(!names().contains(QStringLiteral("carl")));
        QVERIFY(window.archivedButton()->isVisible());
        QCOMPARE(window.archivedButton()->text(), QStringLiteral("Archived Chats (1)"));
        window.archivedButton()->click();
        QVERIFY(window.showingArchived());
        QCOMPARE(names(), QStringLiteral("carl"));
        window.archivedButton()->click();
        QVERIFY(!window.showingArchived());
        // Their next message brings it back; muted, it would stay.
        store.addMessage(id["carl"], QStringLiteral("are you there?"), QStringLiteral("carl"));
        QVERIFY(names().contains(QStringLiteral("carl")));
        QVERIFY(!window.archivedButton()->isVisible());

        // Four pins at most.
        for (const QString &name : {QStringLiteral("bob"), QStringLiteral("carl")})
            QVERIFY(window.pinConversation(id[name], true));
        QVERIFY(window.pinConversation(store.noteToSelf(), true));
        const qint64 dora = store.addConversation(QStringLiteral("direct"), QStringLiteral("dora"), QStringLiteral("dora"));
        store.setAccepted(dora);
        QVERIFY(!window.pinConversation(dora, true));
        QVERIFY(window.noticeLabel()->isVisible());
        QVERIFY(names().startsWith(QStringLiteral("anna bob carl")));
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.pinConversation(store.noteToSelf(), false);
            window.pinConversation(id["carl"], false);
            window.muteConversation(id["bob"], -1);
            store.addMessage(id["bob"], QStringLiteral("muted but still here"), QStringLiteral("bob"));
            window.archiveConversation(dora, true);
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
    }

    // "New messages" above the first unread one, the place kept while reading
    // further up, ↓ back to the newest.
    void whereYouLeftOff()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/left-off.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("Anna"), QStringLiteral("anna"));
        store.setAccepted(anna);
        const QDateTime t0 = QDateTime::currentDateTimeUtc().addSecs(-3600);
        for (int i = 0; i < 30; ++i)
            store.addMessage(anna, QStringLiteral("old %1").arg(i), QStringLiteral("anna"), t0.addSecs(i));
        store.markRead(anna);
        for (int i = 0; i < 30; ++i)
            store.addMessage(anna, QStringLiteral("new %1").arg(i), QStringLiteral("anna"), t0.addSecs(100 + i));
        const qint64 firstNew = store.firstUnseen(anna);
        QCOMPARE(store.message(firstNew).body, QStringLiteral("new 0"));

        MainWindow window(&store, &messenger);
        window.resize(800, 500);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const int dividerRole = Qt::UserRole + 25;
        window.openConversation(anna);
        QListWidget *list = window.messages();
        QCOMPARE(list->count(), 60); // the line is part of its message's row
        QCOMPARE(window.unreadMarker(), firstNew);
        QListWidgetItem *first = list->item(30);
        QVERIFY(first->data(dividerRole).toBool());
        QVERIFY(!list->item(29)->data(dividerRole).toBool());
        // Opened there: the line at the top, read now; ↓ shown.
        QCOMPARE(list->visualItemRect(first).top(), 0);
        QCOMPARE(store.firstUnseen(anna), 0);
        QTRY_VERIFY(window.toBottomButton()->isVisible());

        // Reading further up, a message comes: the place stays; ↓ counts it.
        QScrollBar *bar = list->verticalScrollBar();
        bar->setValue(bar->value() - 100);
        const int place = bar->value();
        store.addMessage(anna, QStringLiteral("are you there?"), QStringLiteral("anna"));
        QCOMPARE(bar->value(), place);
        QCOMPARE(window.toBottomButton()->text(), QStringLiteral("↓ 1"));
        // ↓: the newest; it goes.
        window.toBottomButton()->click();
        QCOMPARE(bar->value(), bar->maximum());
        QVERIFY(!window.toBottomButton()->isVisible());
        // At the newest, a new one keeps it there.
        store.addMessage(anna, QStringLiteral("hello?"), QStringLiteral("anna"));
        QCOMPARE(bar->value(), bar->maximum());
        QVERIFY(!window.toBottomButton()->isVisible());

        // Writing: the line goes, as in Signal.
        QTest::keyClicks(window.composeField(), QStringLiteral("here!"));
        QTest::keyClick(window.composeField(), Qt::Key_Return);
        QCOMPARE(window.unreadMarker(), 0);
        for (int i = 0; i < list->count(); ++i)
            QVERIFY(!list->item(i)->data(dividerRole).toBool());
        QCOMPARE(bar->value(), bar->maximum());
        // Opened again with nothing unread: no line, the newest.
        window.openConversation(store.noteToSelf());
        window.openConversation(anna);
        QCOMPARE(window.unreadMarker(), 0);
        QCOMPARE(bar->value(), bar->maximum());
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            for (int i = 0; i < 3; ++i)
                store.addMessage(anna, QStringLiteral("one more %1").arg(i), QStringLiteral("anna"));
            window.openConversation(store.noteToSelf());
            window.openConversation(anna);
            bar->setValue(bar->value() - 40);
            store.addMessage(anna, QStringLiteral("and another"), QStringLiteral("anna"));
            QTest::qWait(100);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
    }

    void findInAConversation()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/finding.sqlite"));
        MainWindow window(&store);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const int highlightRole = Qt::UserRole + 21;
        const qint64 anna = store.addConversation(QStringLiteral("direct"), QStringLiteral("anna"), QStringLiteral("anna"));
        const QDateTime t = QDateTime::currentDateTimeUtc().addDays(-10);
        // One long ago (more than the list shows at first), then 600 others.
        const qint64 first = store.addMessage(anna, QStringLiteral("The wifi password is argo123"), QStringLiteral("anna"), t).id;
        for (int i = 0; i < 600; ++i)
            store.addMessage(anna, QStringLiteral("message %1").arg(i), i % 2 ? QString() : QStringLiteral("anna"), t.addSecs(60 + i));
        const qint64 recent = store.addMessage(anna, QStringLiteral("what was the WiFi password again?"), QString(), t.addSecs(1000)).id;
        window.openConversation(anna);
        QCOMPARE(window.messages()->count(), 500);

        QTest::keyClick(&window, Qt::Key_F, Qt::ControlModifier);
        QVERIFY(window.findBar()->isVisible());
        QVERIFY(window.findField()->hasFocus());
        QTest::keyClicks(window.findField(), QStringLiteral("wifi"));
        QCOMPARE(window.findCountLabel()->text(), QStringLiteral("2 of 2")); // the newest first
        QCOMPARE(window.currentFindMatch(), recent);
        auto highlighted = [&] {
            for (int i = 0; i < window.messages()->count(); ++i)
                if (window.messages()->item(i)->data(highlightRole).toBool())
                    return window.messages()->item(i)->data(Qt::UserRole + 1).toLongLong();
            return qint64(0);
        };
        QCOMPARE(highlighted(), recent);
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.resize(1000, 640);
            QTest::qWait(200);
            window.findStep(0); // again, at this size
            QTest::qWait(100);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
        // Enter: older; the list grows to hold it.
        QTest::keyClick(window.findField(), Qt::Key_Return);
        QCOMPARE(window.findCountLabel()->text(), QStringLiteral("1 of 2"));
        QCOMPARE(window.currentFindMatch(), first);
        QCOMPARE(highlighted(), first);
        QCOMPARE(window.messages()->count(), 602);
        // Shift+Enter: newer, round again.
        QTest::keyClick(window.findField(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(window.currentFindMatch(), recent);
        QTest::keyClick(window.findField(), Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(window.currentFindMatch(), first);

        window.findField()->setText(QStringLiteral("nothing like this"));
        QCOMPARE(window.findCountLabel()->text(), QStringLiteral("No results"));
        QCOMPARE(highlighted(), 0);
        // Esc: closed, nothing marked.
        window.findField()->setText(QStringLiteral("wifi"));
        QTest::keyClick(window.findField(), Qt::Key_Escape);
        QVERIFY(!window.findBar()->isVisible());
        QCOMPARE(window.currentFindMatch(), 0);
        QCOMPARE(highlighted(), 0);
        // Another conversation closes it, and shows 500 again.
        window.showFind();
        window.findField()->setText(QStringLiteral("argo123"));
        window.openConversation(store.noteToSelf());
        QVERIFY(!window.findBar()->isVisible());
        window.openConversation(anna);
        QCOMPARE(window.messages()->count(), 500);
    }

    void voiceMessageBubble()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/voice.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const int fileRole = Qt::UserRole + 9;
        const int voiceRole = Qt::UserRole + 22;
        QVERIFY(window.findChild<QToolButton *>(QStringLiteral("voiceButton")));
        // What a 3 s recording gives, into Note to Self.
        window.sendVoice(QByteArray(3 * 16000 * 2, '\0'));
        QCOMPARE(window.messages()->count(), 1);
        QListWidgetItem *bubble = window.messages()->item(0);
        QVERIFY(bubble->data(voiceRole).toBool());
        QCOMPARE(bubble->data(fileRole).toString(), QStringLiteral("▶  Voice message · 0:03"));
        const qint64 message = store.messages(store.noteToSelf()).last().id;
        std::unique_ptr<QMenu> menu(window.messageMenu(message));
        QAction *save = menu->findChild<QAction *>(QStringLiteral("saveAttachmentAction"));
        QVERIFY(save);
        QCOMPARE(save->text(), QStringLiteral("Save Voice Message…"));
        // Too short: not sent.
        window.sendVoice(QByteArray(10, '\0'));
        QCOMPARE(window.messages()->count(), 1);
    }

    void othersMessagesAreUnreadUntilOpened()
    {
        ChatStore store(TestSupport::home() + QStringLiteral("/unread.sqlite"));
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
        ServerStore serverStore(TestSupport::home() + QStringLiteral("/server.sqlite"));
        ChatServer server(&serverStore);
        QVERIFY(server.listen());
        const QString url = QStringLiteral("ws://127.0.0.1:%1").arg(server.port());

        ChatStore store(TestSupport::home() + QStringLiteral("/alice.sqlite"));
        KeyStore keys(&store);
        Messenger messenger(&store, &keys);
        MainWindow window(&store, &messenger);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.connectionLabel()->text().contains("sign up"));

        // Signing up through the dialog.
        SignUpDialog dialog(&messenger, &window);
        QVERIFY(!dialog.signUpButton()->isEnabled());
        messenger.useServer(QUrl(url));
        dialog.usernameField()->setText(QStringLiteral("Alice"));
        QVERIFY(dialog.signUpButton()->isEnabled());
        dialog.signUpButton()->click();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 5000);
        QTRY_VERIFY(window.connectionLabel()->text().contains("@alice"));
        QVERIFY(window.connectionLabel()->text().contains("connected"));

        ChatStore bobStore(TestSupport::home() + QStringLiteral("/bob.sqlite"));
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
        ServerStore serverStore(TestSupport::home() + QStringLiteral("/server2.sqlite"));
        ChatServer server(&serverStore);
        QVERIFY(server.listen());
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(server.port()));
        auto signUp = [&](Messenger &m, const QString &name) {
            std::optional<bool> ok;
            m.signUp(url, name, [&](bool done, const QString &) { ok = done; });
            return QTest::qWaitFor([&] { return ok.has_value(); }, 5000) && *ok;
        };

        ChatStore store(TestSupport::home() + QStringLiteral("/me.sqlite"));
        KeyStore keys(&store);
        Messenger me(&store, &keys);
        QVERIFY(signUp(me, QStringLiteral("myself")));
        MainWindow window(&store, &me);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        ChatStore annaStore(TestSupport::home() + QStringLiteral("/anna.sqlite"));
        KeyStore annaKeys(&annaStore);
        Messenger anna(&annaStore, &annaKeys);
        QVERIFY(signUp(anna, QStringLiteral("anna")));
        ChatStore spamStore(TestSupport::home() + QStringLiteral("/spam.sqlite"));
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

    void filesInTheWindow()
    {
        ServerStore serverStore(TestSupport::home() + QStringLiteral("/server4/server.sqlite"));
        ChatServer server(&serverStore);
        QVERIFY(server.listen());
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(server.port()));
        auto signUp = [&](Messenger &m, const QString &name) {
            std::optional<bool> ok;
            m.signUp(url, name, [&](bool done, const QString &) { ok = done; });
            return QTest::qWaitFor([&] { return ok.has_value(); }, 5000) && *ok;
        };
        ChatStore store(TestSupport::home() + QStringLiteral("/me4/chat.sqlite"));
        KeyStore keys(&store);
        Messenger me(&store, &keys);
        QVERIFY(signUp(me, QStringLiteral("myself")));
        ChatStore annaStore(TestSupport::home() + QStringLiteral("/anna4/chat.sqlite"));
        KeyStore annaKeys(&annaStore);
        Messenger anna(&annaStore, &annaKeys);
        QVERIFY(signUp(anna, QStringLiteral("anna")));
        MainWindow window(&store, &me);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // I write first (so Anna's answers are no request), with a file and a caption.
        window.startChatWith(QStringLiteral("anna"));
        QTRY_VERIFY(store.directConversation(QStringLiteral("anna")) > 0);
        const qint64 toAnna = store.directConversation(QStringLiteral("anna"));
        QTRY_COMPARE(window.currentConversation(), toAnna);
        const QString notes = TestSupport::home() + QStringLiteral("/notes.txt");
        {
            QFile f(notes);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("the notes");
        }
        QTest::keyClicks(window.composeField(), QStringLiteral("my notes"));
        window.attachFile(notes);
        QVERIFY(window.composeField()->toPlainText().isEmpty());
        QTRY_COMPARE(window.messages()->count(), 1);
        QCOMPARE(window.messages()->item(0)->text(), QStringLiteral("my notes"));
        QVERIFY(window.messages()->item(0)->data(Qt::UserRole + 9).toString().contains(QStringLiteral("notes.txt")));
        QTRY_VERIFY_WITH_TIMEOUT(annaStore.directConversation(QStringLiteral("myself")) > 0, 5000);
        const qint64 fromMe = annaStore.directConversation(QStringLiteral("myself"));
        anna.acceptRequest(fromMe);

        // Anna sends a picture: it shows in the bubble once it is here.
        QImage picture(400, 300, QImage::Format_RGB32);
        picture.fill(QColor(30, 120, 200));
        const QString photo = TestSupport::home() + QStringLiteral("/photo.png");
        QVERIFY(picture.save(photo));
        QVERIFY(anna.sendFile(fromMe, photo, QString()) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(window.messages()->count(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.messages()->item(1)->data(Qt::UserRole + 8).value<QPixmap>().isNull(), 5000);
        const QPixmap preview = window.messages()->item(1)->data(Qt::UserRole + 8).value<QPixmap>();
        QCOMPARE(preview.width(), 260); // scaled to fit, the shape kept
        QCOMPARE(preview.height(), 195);
        // Saved: the very file.
        const qint64 attachment = window.messages()->item(1)->data(Qt::UserRole + 10).toLongLong();
        const QString saved = TestSupport::home() + QStringLiteral("/saved.png");
        QVERIFY(window.saveAttachment(attachment, saved));
        QFile original(photo), copy(saved);
        QVERIFY(original.open(QIODevice::ReadOnly) && copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), original.readAll());

        // A click shows it in its own window, at its own size (it fits), once.
        QListWidgetItem *pictureItem = window.messages()->item(1);
        const QRect row = window.messages()->visualItemRect(pictureItem);
        QTest::mouseClick(window.messages()->viewport(), Qt::LeftButton, {}, QPoint(row.left() + 60, row.center().y()));
        QTRY_COMPARE(window.imageViewers().size(), 1);
        ImageViewer *viewer = window.imageViewers().first();
        QCOMPARE(viewer->attachment(), attachment);
        QVERIFY(viewer->isWindow());
        QVERIFY(viewer->isFitted());
        QCOMPARE(viewer->zoom(), 1.0);
        QCOMPARE(window.viewImage(attachment), viewer);
        QCOMPARE(window.imageViewers().size(), 1);
        QTest::keyClick(viewer, Qt::Key_Plus);
        QVERIFY(!viewer->isFitted());
        QCOMPARE(viewer->zoom(), 1.25);
        viewer->resize(240, 200); // smaller than the picture: fitted, it shrinks
        QTest::keyClick(viewer, Qt::Key_0);
        QTRY_VERIFY(viewer->zoom() < 1.0);
        QTest::keyClick(viewer, Qt::Key_1);
        QCOMPARE(viewer->zoom(), 1.0);
        QTest::keyClick(viewer, Qt::Key_Escape);
        QTRY_VERIFY(window.imageViewers().isEmpty());
        // A message deleted takes its window with it.
        QVERIFY(window.viewImage(attachment));
        // Locking hides it too.
        QVERIFY(window.viewImage(attachment));
        QVERIFY(window.appLock()->set(QString(), QStringLiteral("open sesame"), 0));
        window.lockNow();
        QVERIFY(window.isLocked());
        QTRY_VERIFY(window.imageViewers().isEmpty());
        QVERIFY(window.unlock(QStringLiteral("open sesame")));
        store.deleteMessage(store.attachment(attachment).message);
        QTRY_VERIFY(window.imageViewers().isEmpty());
        QVERIFY(!window.viewImage(attachment));
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
    }

    void profilesInTheWindow()
    {
        // Pictures made ready to send: square, 256×256, small.
        QImage wide(800, 400, QImage::Format_RGB32);
        wide.fill(QColor(200, 80, 40));
        QImage made;
        QVERIFY(made.loadFromData(ProfileDialog::preparePicture(wide)));
        QCOMPARE(made.size(), QSize(256, 256));
        QImage noise(1000, 1000, QImage::Format_RGB32);
        for (int y = 0; y < noise.height(); ++y)
            for (int x = 0; x < noise.width(); ++x)
                noise.setPixel(x, y, qRgb(std::rand() % 256, std::rand() % 256, std::rand() % 256));
        const QByteArray heavy = ProfileDialog::preparePicture(noise);
        QVERIFY(heavy.size() <= Messenger::MaxAvatarBytes);
        // Even noise fits: 256×256 is at most about 197 KB as PNG.
        QImage back;
        QVERIFY(back.loadFromData(heavy));
        QCOMPARE(back.size(), QSize(256, 256));
        QVERIFY(ProfileDialog::preparePicture(QImage()).isEmpty());

        ServerStore serverStore(TestSupport::home() + QStringLiteral("/server5/server.sqlite"));
        ChatServer server(&serverStore);
        QVERIFY(server.listen());
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(server.port()));
        auto signUp = [&](Messenger &m, const QString &name) {
            std::optional<bool> ok;
            m.signUp(url, name, [&](bool done, const QString &) { ok = done; });
            return QTest::qWaitFor([&] { return ok.has_value(); }, 5000) && *ok;
        };
        ChatStore store(TestSupport::home() + QStringLiteral("/me5/chat.sqlite"));
        KeyStore keys(&store);
        Messenger me(&store, &keys);
        QVERIFY(signUp(me, QStringLiteral("myself")));
        ChatStore annaStore(TestSupport::home() + QStringLiteral("/anna5/chat.sqlite"));
        KeyStore annaKeys(&annaStore);
        Messenger anna(&annaStore, &annaKeys);
        QVERIFY(signUp(anna, QStringLiteral("anna")));
        MainWindow window(&store, &me);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // My profile, through the dialog.
        const QString picturePath = TestSupport::home() + QStringLiteral("/me.png");
        QVERIFY(wide.save(picturePath));
        ProfileDialog dialog(&me, &window);
        dialog.nameField()->setText(QStringLiteral("Me Myself"));
        QVERIFY(dialog.setPicture(picturePath));
        QVERIFY(!dialog.pictureLabel()->pixmap().isNull());
        dialog.saveButton()->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(me.myProfile().name, QStringLiteral("Me Myself"));
        QVERIFY(!me.myProfile().avatar.isEmpty());

        // I write to Anna; she accepts, and her name and picture come to me.
        QImage annaPicture(300, 300, QImage::Format_RGB32);
        annaPicture.fill(QColor(40, 160, 90));
        QVERIFY(anna.setMyProfile(QStringLiteral("Anna K."), ProfileDialog::preparePicture(annaPicture)));
        window.startChatWith(QStringLiteral("anna"));
        QTRY_VERIFY(store.directConversation(QStringLiteral("anna")) > 0);
        // My profile reaches her as soon as I open the chat; a conversation only with a message.
        QTRY_COMPARE_WITH_TIMEOUT(anna.displayName(QStringLiteral("myself")), QStringLiteral("Me Myself"), 5000);
        QCOMPARE(annaStore.directConversation(QStringLiteral("myself")), 0);
        me.send(store.directConversation(QStringLiteral("anna")), QStringLiteral("Hi Anna"));
        QTRY_VERIFY_WITH_TIMEOUT(annaStore.directConversation(QStringLiteral("myself")) > 0, 5000);
        anna.acceptRequest(annaStore.directConversation(QStringLiteral("myself")));
        QTRY_COMPARE_WITH_TIMEOUT(window.conversations()->item(0)->text(), QStringLiteral("Anna K."), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.conversations()->item(0)->data(Qt::UserRole + 11).value<QPixmap>().isNull(), 5000);
        QCOMPARE(window.findChild<QLabel *>(QStringLiteral("chatTitle"))->text(), QStringLiteral("Anna K."));
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("chatSubtitle"))->text().contains(QStringLiteral("@anna")));
        // Anna has my picture too.
        QTRY_VERIFY_WITH_TIMEOUT(!anna.avatar(QStringLiteral("myself")).isEmpty(), 5000);

        // Search: by her name or her username.
        auto search = window.findChild<QLineEdit *>(QStringLiteral("searchField"));
        search->setText(QStringLiteral("K."));
        QVERIFY(!window.conversations()->item(0)->isHidden());
        search->setText(QStringLiteral("anna"));
        QVERIFY(!window.conversations()->item(0)->isHidden());
        search->setText(QStringLiteral("nobody"));
        QVERIFY(window.conversations()->item(0)->isHidden());
        search->clear();
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
    }

    void groupsInTheWindow()
    {
        ServerStore serverStore(TestSupport::home() + QStringLiteral("/server6/server.sqlite"));
        ChatServer server(&serverStore);
        QVERIFY(server.listen());
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(server.port()));
        auto signUp = [&](Messenger &m, const QString &name) {
            std::optional<bool> ok;
            m.signUp(url, name, [&](bool done, const QString &) { ok = done; });
            return QTest::qWaitFor([&] { return ok.has_value(); }, 5000) && *ok;
        };
        ChatStore store(TestSupport::home() + QStringLiteral("/me6/chat.sqlite"));
        KeyStore keys(&store);
        Messenger me(&store, &keys);
        QVERIFY(signUp(me, QStringLiteral("myself")));
        ChatStore annaStore(TestSupport::home() + QStringLiteral("/anna6/chat.sqlite"));
        KeyStore annaKeys(&annaStore);
        Messenger anna(&annaStore, &annaKeys);
        QVERIFY(signUp(anna, QStringLiteral("anna")));
        ChatStore benStore(TestSupport::home() + QStringLiteral("/ben6/chat.sqlite"));
        KeyStore benKeys(&benStore);
        Messenger ben(&benStore, &benKeys);
        QVERIFY(signUp(ben, QStringLiteral("ben")));
        MainWindow window(&store, &me);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy notified(&window, &MainWindow::notified);

        // A new group, through the dialog.
        GroupDialog create(&me, 0, &window);
        create.nameField()->setText(QStringLiteral("Book club"));
        create.membersField()->setPlainText(QStringLiteral("anna, @ben\n"));
        QCOMPARE(create.members(), (QStringList{QStringLiteral("anna"), QStringLiteral("ben")}));
        create.okButton()->click();
        QTRY_COMPARE_WITH_TIMEOUT(create.result(), int(QDialog::Accepted), 5000);
        const qint64 club = create.conversation();
        QVERIFY(club > 0);
        window.openConversation(club);
        QCOMPARE(window.findChild<QLabel *>(QStringLiteral("chatTitle"))->text(), QStringLiteral("Book club"));
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("chatSubtitle"))->text().contains(QStringLiteral("3 members")));

        // Anna (a stranger to me: a group request for her) accepts and writes.
        QTRY_VERIFY_WITH_TIMEOUT(annaStore.groupConversation(store.group(club).id) > 0, 5000);
        const qint64 annaClub = annaStore.groupConversation(store.group(club).id);
        anna.acceptRequest(annaClub);
        window.openConversation(store.noteToSelf()); // elsewhere, so it notifies
        anna.send(annaClub, QStringLiteral("I'm reading Ithaca"));
        QTRY_COMPARE_WITH_TIMEOUT(notified.size(), 1, 5000);
        QCOMPARE(notified.at(0).at(1).toString(), QStringLiteral("Book club · anna"));
        window.openConversation(club);
        QTRY_COMPARE(window.messages()->count(), 1);
        QCOMPARE(window.messages()->item(0)->data(Qt::UserRole + 13).toString(), QStringLiteral("anna"));

        // Members: mine to change; Anna's only to see.
        GroupDialog mine(&me, club, &window);
        QVERIFY(!mine.nameField()->isReadOnly());
        QVERIFY(mine.okButton()->isVisible() || !mine.okButton()->isHidden());
        GroupDialog hers(&anna, annaClub, &window);
        QVERIFY(hers.nameField()->isReadOnly());
        QVERIFY(hers.membersField()->isReadOnly());

        // Ben makes his own group with me: a request in my window, saying who.
        std::optional<qint64> benGroup;
        ben.createGroup(QStringLiteral("Ben's party"), {QStringLiteral("myself")},
                        [&](qint64 c, const QString &) { benGroup = c; });
        QTRY_VERIFY_WITH_TIMEOUT(benGroup.has_value() && *benGroup > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(store.groupConversation(benStore.group(*benGroup).id) > 0, 5000);
        const qint64 party = store.groupConversation(benStore.group(*benGroup).id);
        window.openConversation(party);
        QVERIFY(window.requestBar()->isVisible());
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("requestText"))->text().contains(QStringLiteral("added you to")));

        // I leave the book club: read only.
        window.openConversation(club);
        QVERIFY(window.composer()->isVisible());
        window.leaveGroup(false);
        QVERIFY(store.conversation(club).left);
        QVERIFY(!window.composer()->isVisible());
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("leftNote"))->isVisible());
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
    }

    void linkingInTheWindow()
    {
        ServerStore serverStore(TestSupport::home() + QStringLiteral("/server7/server.sqlite"));
        ChatServer server(&serverStore);
        QVERIFY(server.listen());
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(server.port()));
        ChatStore aliceStore(TestSupport::home() + QStringLiteral("/alice7/chat.sqlite"));
        KeyStore aliceKeys(&aliceStore);
        Messenger alice(&aliceStore, &aliceKeys);
        std::optional<bool> signedUp;
        alice.signUp(url, QStringLiteral("alice"), [&](bool ok, const QString &) { signedUp = ok; });
        QTRY_VERIFY(signedUp.has_value() && *signedUp);

        // The laptop: Link to My Account… shows a code.
        ChatStore laptopStore(TestSupport::home() + QStringLiteral("/laptop7/chat.sqlite"));
        KeyStore laptopKeys(&laptopStore);
        Messenger laptop(&laptopStore, &laptopKeys);
        laptop.useServer(url);
        SignUpDialog dialog(&laptop);
        dialog.show();
        dialog.linkButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.codeField()->text().isEmpty(), 5000);
        QVERIFY(dialog.codeField()->isVisible());
        QVERIFY(dialog.messageLabel()->text().contains(QStringLiteral("Linked Devices")));

        // Alice's computer takes the code (history on, as it starts); the laptop is in,
        // and her notes follow it.
        aliceStore.addMessage(aliceStore.noteToSelf(), QStringLiteral("a note from before"), QString(),
                              QDateTime::currentDateTimeUtc(), Protocol::newMessageId());
        DevicesDialog devices(&alice);
        QVERIFY(devices.historyCheck()->isChecked());
        QTRY_COMPARE_WITH_TIMEOUT(devices.list()->count(), 1, 5000);
        devices.linkWithCode(dialog.codeField()->text());
        QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 5000);
        QCOMPARE(laptop.username(), QStringLiteral("alice"));
        QCOMPARE(laptop.device(), 2u);
        QTRY_COMPARE_WITH_TIMEOUT(laptopStore.messages(laptopStore.noteToSelf()).size(), 1, 10000);
        QCOMPARE(laptopStore.messages(laptopStore.noteToSelf()).first().body, QStringLiteral("a note from before"));
        QTRY_VERIFY_WITH_TIMEOUT(devices.messageLabel()->text().contains(QStringLiteral("history was sent")), 10000);

        // Both, this one marked; only the first computer removes the others.
        DevicesDialog onLaptop(&laptop);
        QTRY_COMPARE_WITH_TIMEOUT(onLaptop.list()->count(), 2, 5000);
        QVERIFY(onLaptop.list()->item(1)->text().contains(QStringLiteral("this one")));
        onLaptop.list()->setCurrentRow(0);
        QVERIFY(!onLaptop.removeButton()->isEnabled()); // the first stays
        onLaptop.list()->setCurrentRow(1);
        QVERIFY(onLaptop.removeButton()->isEnabled()); // itself
        DevicesDialog onFirst(&alice);
        QTRY_COMPARE_WITH_TIMEOUT(onFirst.list()->count(), 2, 5000);
        onFirst.list()->setCurrentRow(1);
        QVERIFY(onFirst.removeButton()->isEnabled());
        onFirst.removeSelected(false);
        QTRY_COMPARE_WITH_TIMEOUT(onFirst.list()->count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(laptop.state() != Messenger::State::Online, 5000);

        // A cancelled link leaves nothing behind.
        ChatStore thirdStore(TestSupport::home() + QStringLiteral("/third7/chat.sqlite"));
        KeyStore thirdKeys(&thirdStore);
        Messenger third(&thirdStore, &thirdKeys);
        third.useServer(url);
        SignUpDialog cancelled(&third);
        cancelled.linkButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(!cancelled.codeField()->text().isEmpty(), 5000);
        cancelled.reject();
        QCOMPARE(third.state(), Messenger::State::NoAccount);
        QVERIFY(!thirdKeys.account());
    }

    void disappearingInTheWindow()
    {
        ServerStore serverStore(TestSupport::home() + QStringLiteral("/server8/server.sqlite"));
        ChatServer server(&serverStore);
        QVERIFY(server.listen());
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(server.port()));
        auto signUp = [&](Messenger &m, const QString &name) {
            std::optional<bool> ok;
            m.signUp(url, name, [&](bool done, const QString &) { ok = done; });
            return QTest::qWaitFor([&] { return ok.has_value(); }, 5000) && *ok;
        };
        ChatStore store(TestSupport::home() + QStringLiteral("/me8/chat.sqlite"));
        KeyStore keys(&store);
        Messenger me(&store, &keys);
        QVERIFY(signUp(me, QStringLiteral("myself")));
        ChatStore annaStore(TestSupport::home() + QStringLiteral("/anna8/chat.sqlite"));
        KeyStore annaKeys(&annaStore);
        Messenger anna(&annaStore, &annaKeys);
        QVERIFY(signUp(anna, QStringLiteral("anna")));
        MainWindow window(&store, &me);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.startChatWith(QStringLiteral("anna"));
        QTRY_VERIFY(store.directConversation(QStringLiteral("anna")) > 0);
        const qint64 toAnna = store.directConversation(QStringLiteral("anna"));
        QTRY_COMPARE(window.currentConversation(), toAnna);
        me.send(toAnna, QStringLiteral("hello"));
        QTRY_VERIFY_WITH_TIMEOUT(annaStore.directConversation(QStringLiteral("myself")) > 0, 5000);
        anna.acceptRequest(annaStore.directConversation(QStringLiteral("myself")));

        // Conversation › Disappearing Messages › 1 hour.
        QAction *hour = nullptr;
        for (QAction *a : window.timerMenu()->actions())
            if (a->data().toLongLong() == 3600)
                hour = a;
        QVERIFY(hour);
        QVERIFY(window.timerMenu()->isEnabled());
        hour->trigger();
        QVERIFY(hour->isChecked());
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("chatSubtitle"))->text().contains(QStringLiteral("⏱ 1 hour")));
        QTRY_COMPARE(window.messages()->count(), 2);
        QVERIFY(window.messages()->item(1)->data(Qt::UserRole + 14).toBool()); // a note
        QCOMPARE(window.messages()->item(1)->text(), QStringLiteral("You set disappearing messages to 1 hour."));
        QTRY_COMPARE_WITH_TIMEOUT(annaStore.conversation(annaStore.directConversation(QStringLiteral("myself"))).expireSeconds,
                                  3600, 5000);
        // What I write now carries the ⏱.
        QTest::keyClicks(window.composeField(), QStringLiteral("this goes"));
        QTest::keyClick(window.composeField(), Qt::Key_Return);
        QTRY_COMPARE(window.messages()->count(), 3);
        QVERIFY(window.messages()->item(2)->data(Qt::UserRole + 15).toBool());
        QVERIFY(!window.messages()->item(0)->data(Qt::UserRole + 15).toBool()); // the first did not
        if (qEnvironmentVariableIsSet("CHAT_TEST_GRAB")) {
            window.resize(1000, 640);
            QTest::qWait(200);
            window.grab().save(qEnvironmentVariable("CHAT_TEST_GRAB"));
        }
        // Off again.
        window.setDisappearing(0);
        QVERIFY(!window.findChild<QLabel *>(QStringLiteral("chatSubtitle"))->text().contains(QStringLiteral("⏱")));
    }

    void notificationContent()
    {
        using Notifications::Content;
        auto text = Notifications::make(Content::NameAndMessage, QStringLiteral("anna"),
                                        QStringLiteral("See you\n at  8"), false);
        QCOMPARE(text->title, QStringLiteral("anna"));
        QCOMPARE(text->body, QStringLiteral("See you at 8"));
        QCOMPARE(Notifications::make(Content::NameAndMessage, QStringLiteral("anna"), QString(300, 'x'), false)->body.size(),
                 200);
        text = Notifications::make(Content::NameOnly, QStringLiteral("anna"), QStringLiteral("secret"), true);
        QCOMPARE(text->title, QStringLiteral("Message request from anna"));
        QVERIFY(!text->body.contains(QStringLiteral("secret")));
        text = Notifications::make(Content::Nothing, QStringLiteral("anna"), QStringLiteral("secret"), false);
        QVERIFY(!text->title.contains(QStringLiteral("anna")) && !text->body.contains(QStringLiteral("secret")));
        QVERIFY(!Notifications::make(Content::Off, QStringLiteral("anna"), QStringLiteral("secret"), false));
        // Kept in the settings; the default is name and message.
        QCOMPARE(Notifications::saved(), Content::NameAndMessage);
        Notifications::save(Content::NameOnly);
        QCOMPARE(Notifications::saved(), Content::NameOnly);
        Notifications::save(Content::NameAndMessage);
    }

    void notificationsAndUnread()
    {
        ServerStore serverStore(TestSupport::home() + QStringLiteral("/server3.sqlite"));
        ChatServer server(&serverStore);
        QVERIFY(server.listen());
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(server.port()));
        auto signUp = [&](Messenger &m, const QString &name) {
            std::optional<bool> ok;
            m.signUp(url, name, [&](bool done, const QString &) { ok = done; });
            return QTest::qWaitFor([&] { return ok.has_value(); }, 5000) && *ok;
        };
        ChatStore store(TestSupport::home() + QStringLiteral("/me3.sqlite"));
        KeyStore keys(&store);
        Messenger me(&store, &keys);
        QVERIFY(signUp(me, QStringLiteral("myself")));
        MainWindow window(&store, &me);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy notified(&window, &MainWindow::notified);

        ChatStore annaStore(TestSupport::home() + QStringLiteral("/anna3.sqlite"));
        KeyStore annaKeys(&annaStore);
        Messenger anna(&annaStore, &annaKeys);
        QVERIFY(signUp(anna, QStringLiteral("anna")));
        std::optional<qint64> toMe;
        anna.startConversation(QStringLiteral("myself"), [&](qint64 id, const QString &) { toMe = id; });
        QTRY_VERIFY(toMe.has_value());

        // A stranger's first message: a request.
        anna.send(*toMe, QStringLiteral("Hi, it's Anna"));
        QTRY_COMPARE_WITH_TIMEOUT(notified.size(), 1, 5000);
        const qint64 fromAnna = store.directConversation(QStringLiteral("anna"));
        QCOMPARE(notified.at(0).at(0).toLongLong(), fromAnna);
        QCOMPARE(notified.at(0).at(1).toString(), QStringLiteral("Message request from anna"));
        QCOMPARE(notified.at(0).at(2).toString(), QStringLiteral("Hi, it's Anna"));
        QCOMPARE(window.unreadCount(), 1);

        // Accepted elsewhere in the window; her next ones, by name, and counted.
        me.acceptRequest(fromAnna);
        anna.send(*toMe, QStringLiteral("Are you there?"));
        anna.send(*toMe, QStringLiteral("Hello?"));
        QTRY_COMPARE_WITH_TIMEOUT(notified.size(), 3, 5000);
        QCOMPARE(notified.at(1).at(1).toString(), QStringLiteral("anna"));
        QCOMPARE(notified.at(2).at(2).toString(), QStringLiteral("Hello?"));
        QTRY_COMPARE(window.unreadCount(), 2);

        // Less said, or nothing.
        Notifications::save(Notifications::Content::NameOnly);
        anna.send(*toMe, QStringLiteral("a secret"));
        QTRY_COMPARE_WITH_TIMEOUT(notified.size(), 4, 5000);
        QVERIFY(!notified.at(3).at(2).toString().contains(QStringLiteral("secret")));
        Notifications::save(Notifications::Content::Off);
        anna.send(*toMe, QStringLiteral("unheard"));
        QTRY_COMPARE_WITH_TIMEOUT(store.messages(fromAnna).size(), 5, 5000);
        QTest::qWait(100);
        QCOMPARE(notified.size(), 4);
        Notifications::save(Notifications::Content::NameAndMessage);

        // Opened, and the window in front: read, and no notification for what comes.
        window.openConversation(fromAnna);
        QTRY_COMPARE(window.unreadCount(), 0);
        window.activateWindow();
        if (QTest::qWaitForWindowActive(&window)) {
            anna.send(*toMe, QStringLiteral("you're here"));
            QTRY_COMPARE_WITH_TIMEOUT(store.messages(fromAnna).size(), 6, 5000);
            QTest::qWait(100);
            QCOMPARE(notified.size(), 4);
        } else {
            qWarning("The window never became active here: \"no notification in front\" not checked.");
        }
    }
};

CHAT_TEST_MAIN(TestMainWindow, QApplication)
#include "tst_mainwindow.moc"
