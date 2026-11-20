#include "DatabaseKey.h"

#include "CipherSql.h"
#include "Crypto.h"

#include <QCoreApplication>
#include <QEventLoop>

#include <qt6keychain/keychain.h>

namespace {

const QString kService = QStringLiteral("Argonaut Chat");
const QString kEntry = QStringLiteral("database-key");

QString tr(const char *text)
{
    return QCoreApplication::translate("DatabaseKey", text);
}

void run(QKeychain::Job &job)
{
    job.setAutoDelete(false);
    job.setInsecureFallback(false); // never a plain file instead of the keyring
    job.setKey(kEntry);
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
}

} // namespace

namespace DatabaseKey {

Result load(bool mayCreate)
{
    Result result;
    QKeychain::ReadPasswordJob read(kService);
    run(read);
    if (read.error() == QKeychain::NoError) {
        result.key = read.binaryData();
        if (result.key.size() != CipherSql::KeySize) {
            result.key.clear();
            result.error = tr("The key of your history in the system keyring is damaged.");
        }
        return result;
    }
    if (read.error() != QKeychain::EntryNotFound) {
        result.error = tr("The system keyring could not be reached (%1). Argonaut Chat keeps the key of your "
                          "history there.")
                           .arg(read.errorString());
        return result;
    }
    if (!mayCreate) {
        result.error = tr("The key of your history is missing from the system keyring, so the history cannot "
                          "be opened. Nothing has been changed.");
        return result;
    }
    const QByteArray key = Crypto::randomBytes(CipherSql::KeySize);
    QKeychain::WritePasswordJob write(kService);
    write.setBinaryData(key);
    run(write);
    if (write.error() != QKeychain::NoError) {
        result.error = tr("The key of your history could not be kept in the system keyring (%1).")
                           .arg(write.errorString());
        return result;
    }
    result.key = key;
    return result;
}

} // namespace DatabaseKey
