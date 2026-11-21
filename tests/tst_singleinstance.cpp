#include "SingleInstance.h"
#include "TestSupport.h"

#include <QApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTest>

class TestSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void secondStartShowsTheFirst()
    {
        const QString file = TestSupport::home() + QStringLiteral("/data/chat.sqlite");
        SingleInstance first(file);
        QVERIFY(first.start());
        QSignalSpy shown(&first, &SingleInstance::activated);

        SingleInstance second(file);
        QVERIFY(!second.start()); // it would end here
        QVERIFY(shown.wait(5000));

        // And again, later on.
        SingleInstance third(file);
        QVERIFY(!third.start());
        QVERIFY(shown.wait(5000));
        QCOMPARE(shown.size(), 2);
    }

    void otherDataFoldersDontMeet()
    {
        SingleInstance alice(TestSupport::home() + QStringLiteral("/alice/chat.sqlite"));
        SingleInstance bob(TestSupport::home() + QStringLiteral("/bob/chat.sqlite"));
        QVERIFY(alice.start());
        QVERIFY(bob.start());
        QVERIFY(alice.socketName() != bob.socketName());
    }

    void freeOnceItEnds()
    {
        const QString file = TestSupport::home() + QStringLiteral("/again/chat.sqlite");
        {
            SingleInstance first(file);
            QVERIFY(first.start());
        }
        QVERIFY(!QFile::exists(file + QStringLiteral(".lock")));
        SingleInstance next(file);
        QVERIFY(next.start());
    }
};

CHAT_TEST_MAIN(TestSingleInstance, QApplication)
#include "tst_singleinstance.moc"
