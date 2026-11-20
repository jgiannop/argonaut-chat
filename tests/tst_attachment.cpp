#include "Attachment.h"
#include "Crypto.h"
#include "Protocol.h"
#include "TestSupport.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace Attachment;

class TestAttachment : public QObject
{
    Q_OBJECT

private slots:
    void padding()
    {
        QCOMPARE(paddedSize(0), 541);
        QCOMPARE(paddedSize(541), 541);
        qint64 previous = 541;
        for (qint64 size : {542LL, 1000LL, 4096LL, 100000LL, 1000000LL, MaxFileBytes}) {
            const qint64 padded = paddedSize(size);
            QVERIFY(padded >= size);
            QVERIFY(padded <= size * 1.05 + 1); // at most 5 % more
            QVERIFY(padded >= previous);
            previous = padded;
        }
        // Files of nearby sizes look the same.
        QCOMPARE(paddedSize(99000), paddedSize(100100)); // one bucket: 95386..100155
        QCOMPARE(blobSize(1000), Crypto::AeadNonceSize + paddedSize(1000) + Crypto::AeadTagSize);
    }

    void roundTrip_data()
    {
        QTest::addColumn<int>("size");
        for (int size : {0, 1, 541, 542, 10000, 1 << 20})
            QTest::newRow(qPrintable(QString::number(size))) << size;
    }

    void roundTrip()
    {
        QFETCH(int, size);
        const QByteArray file = Crypto::randomBytes(size);
        const auto sealed = encrypt(file);
        QVERIFY(sealed);
        QCOMPARE(sealed->key.size(), KeySize);
        QCOMPARE(sealed->digest, Crypto::sha256(sealed->blob));
        QCOMPARE(sealed->blob.size(), blobSize(size));
        QCOMPARE(decrypt(sealed->blob, sealed->key, sealed->digest, size), std::optional<QByteArray>(file));
        // The same file twice: two different keys, two different blobs.
        const auto again = encrypt(file);
        QVERIFY(again->key != sealed->key);
        QVERIFY(again->blob != sealed->blob);
    }

    void changesAreCaught()
    {
        const QByteArray file("a holiday photo, pretend");
        const auto sealed = encrypt(file);
        QByteArray changed = sealed->blob;
        changed[40] = char(changed.at(40) ^ 1);
        // Changed on the way: the digest says so.
        QVERIFY(!decrypt(changed, sealed->key, sealed->digest, file.size()));
        // Even with a digest made to match, the encryption says so.
        QVERIFY(!decrypt(changed, sealed->key, Crypto::sha256(changed), file.size()));
        QVERIFY(!decrypt(sealed->blob, Crypto::randomBytes(32), sealed->digest, file.size()));
        QVERIFY(!decrypt(sealed->blob.left(sealed->blob.size() - 1), sealed->key, sealed->digest, file.size()));
        QVERIFY(!decrypt(sealed->blob, sealed->key, sealed->digest, file.size() + 1000));
        QVERIFY(!decrypt(sealed->blob, sealed->key, sealed->digest, -1));
        QVERIFY(!decrypt(sealed->blob, "short", sealed->digest, file.size()));
        // Too large to send.
        QVERIFY(!encrypt(QByteArray(int(MaxFileBytes) + 1, 'x')));
    }

    void names()
    {
        QCOMPARE(safeName("photo.jpg"), QString("photo.jpg"));
        QCOMPARE(safeName("../../etc/passwd"), QString("passwd"));
        QCOMPARE(safeName("C:\\Users\\x\\report.pdf"), QString("report.pdf"));
        QCOMPARE(safeName(QStringLiteral("invoice") + QChar(0x202E) + QStringLiteral("fdp.exe")), QString("invoicefdp.exe"));
        QCOMPARE(safeName(QString("a\nb\tc")), QString("abc"));
        QCOMPARE(safeName(""), QString("file"));
        QCOMPARE(safeName(".."), QString("file"));
        QCOMPARE(safeName("dir/"), QString("file"));
        QCOMPARE(safeName(QString(300, 'x')).size(), MaxNameLength);
        QCOMPARE(safeName(QString::fromUtf8("Φωτογραφία.png")), QString::fromUtf8("Φωτογραφία.png"));

        QVERIFY(isValidId("0123456789abcdef0123456789abcdef"));
        QVERIFY(!isValidId("0123456789ABCDEF0123456789ABCDEF"));
        QVERIFY(!isValidId("0123456789abcdef"));
        QVERIFY(!isValidId("../../../../etc/passwd/xxxxxxxxx"));
    }

    void pointers()
    {
        const Pointer pointer{"0123456789abcdef0123456789abcdef", Crypto::randomBytes(32), Crypto::randomBytes(32),
                              12345, "photo.jpg", "image/jpeg"};
        const QByteArray wire = QJsonDocument(toJson(pointer)).toJson();
        const auto back = fromJson(QJsonDocument::fromJson(wire).object());
        QVERIFY(back);
        QCOMPARE(back->id, pointer.id);
        QCOMPARE(back->key, pointer.key);
        QCOMPARE(back->digest, pointer.digest);
        QCOMPARE(back->size, pointer.size);
        QCOMPARE(back->name, pointer.name);
        QCOMPARE(back->type, pointer.type);

        // From strangers.
        for (const auto &[key, value] : QList<QPair<QString, QJsonValue>>{
                 {"id", "../etc"}, {"key", "short"}, {"digest", 5}, {"size", -1}, {"size", double(MaxFileBytes + 1)},
                 {"size", 1.5}, {"name", 7}, {"name", QString(300, 'x')}, {"type", "not a type"},
                 {"type", "Image/JPEG"}, {"type", ""}}) {
            QJsonObject json = toJson(pointer);
            json.insert(key, value);
            QVERIFY2(!fromJson(json), qPrintable(key + " " + value.toVariant().toString().left(20)));
        }
        QVERIFY(!fromJson(QJsonValue(5)));
        // A name with folders arrives as just the name.
        QJsonObject sneaky = toJson(pointer);
        sneaky.insert("name", "../../.bashrc");
        QCOMPARE(fromJson(sneaky)->name, QString(".bashrc"));
    }

    void inMessages()
    {
        Protocol::Content content = Protocol::Content::text("look at this");
        const Pointer pointer{"0123456789abcdef0123456789abcdef", Crypto::randomBytes(32), Crypto::randomBytes(32),
                              99, "a.png", "image/png"};
        content.attachments = {pointer, pointer};
        auto back = Protocol::decodeContent(Protocol::encodeContent(content));
        QVERIFY(back);
        QCOMPARE(back->body, QString("look at this"));
        QCOMPARE(back->attachments.size(), 2);
        QCOMPARE(back->attachments.first().key, pointer.key);
        // Without attachments, as before.
        QVERIFY(Protocol::decodeContent(Protocol::encodeContent(Protocol::Content::text("x")))->attachments.isEmpty());
        // Too many, or a bad one: not sent, not taken.
        content.attachments = QList<Pointer>(Protocol::MaxAttachments + 1, pointer);
        QVERIFY(Protocol::encodeContent(content).isEmpty());
        Pointer bad = pointer;
        bad.id = "nope";
        content.attachments = {bad};
        QVERIFY(Protocol::encodeContent(content).isEmpty());
        const QByteArray id = Protocol::newMessageId().toLatin1();
        QVERIFY(!Protocol::decodeContent(Protocol::pad(
            "{\"v\":1,\"t\":\"text\",\"id\":\"" + id + "\",\"sent\":1,\"body\":\"\",\"attachments\":[{\"id\":1}]}")));
        QVERIFY(!Protocol::decodeContent(Protocol::pad(
            "{\"v\":1,\"t\":\"text\",\"id\":\"" + id + "\",\"sent\":1,\"body\":\"\",\"attachments\":[]}")));
    }
};

CHAT_TEST_MAIN(TestAttachment, QCoreApplication)
#include "tst_attachment.moc"
