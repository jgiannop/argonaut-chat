#include "ChatServer.h"
#include "ChatStore.h"
#include "Dialogs.h"
#include "KeyStore.h"
#include "MainWindow.h"
#include "Notifications.h"
#include "Messenger.h"
#include "ServerStore.h"
#include "TestSupport.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QToolButton>
#include <QListWidget>

#include <memory>
#include <QPlainTextEdit>
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
        dialog.serverField()->setText(url);
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
        SignUpDialog dialog(&laptop);
        dialog.show();
        dialog.serverField()->setText(url.toString());
        dialog.linkButton()->click();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.codeField()->text().isEmpty(), 5000);
        QVERIFY(dialog.codeField()->isVisible());
        QVERIFY(dialog.messageLabel()->text().contains(QStringLiteral("Linked Devices")));

        // Alice's computer takes the code; the laptop is in.
        DevicesDialog devices(&alice);
        QTRY_COMPARE_WITH_TIMEOUT(devices.list()->count(), 1, 5000);
        devices.linkWithCode(dialog.codeField()->text());
        QTRY_COMPARE_WITH_TIMEOUT(dialog.result(), int(QDialog::Accepted), 5000);
        QCOMPARE(laptop.username(), QStringLiteral("alice"));
        QCOMPARE(laptop.device(), 2u);

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
        SignUpDialog cancelled(&third);
        cancelled.serverField()->setText(url.toString());
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
