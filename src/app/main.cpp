#include "ChatStore.h"
#include "CipherSql.h"
#include "DatabaseKey.h"
#include "KeyStore.h"
#include "MainWindow.h"
#include "Messenger.h"
#include "Theme.h"

#include <QApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv)
{
    // Asked before anything starts (an install's check).
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--version") {
            std::printf("argonaut-chat %s\n", CHAT_VERSION);
            return 0;
        }
        if (arg == "--help") {
            std::printf("Usage: argonaut-chat\nA private messenger in C++ and Qt, from the makers of Argonaut.\n");
            return 0;
        }
    }
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Argonaut"));
    QApplication::setApplicationName(QStringLiteral("argonaut-chat"));
    QApplication::setApplicationDisplayName(QStringLiteral("Argonaut Chat"));
    QApplication::setApplicationVersion(QStringLiteral(CHAT_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("argonaut-chat"));
    QApplication::setStyle(QStringLiteral("Fusion"));
    applyThemeKey(savedThemeKey());

    // The history is encrypted; its key is in the system keyring. Without the
    // key nothing opens and nothing is changed.
    const QString file = ChatStore::defaultFile();
    const QFileInfo info(file);
    const bool encryptedHistory = info.exists() && info.size() > 0 && !CipherSql::isPlainSqlite(file);
    const DatabaseKey::Result key = DatabaseKey::load(!encryptedHistory);
    if (key.key.isEmpty()) {
        QMessageBox::critical(nullptr, QObject::tr("Argonaut Chat"), key.error);
        return 1;
    }
    ChatStore store(file, key.key);
    if (!store.isOpen()) {
        QMessageBox::critical(nullptr, QObject::tr("Argonaut Chat"),
                              QObject::tr("Your history could not be opened: %1").arg(store.error()));
        return 1;
    }
    KeyStore keys(&store);
    Messenger messenger(&store, &keys);
    MainWindow window(&store, &messenger);
    window.show();
    // The first time: sign up (or not now, for Note to Self alone).
    if (messenger.hasAccount())
        messenger.start();
    else
        QTimer::singleShot(0, &window, &MainWindow::showSignUp);
    return app.exec();
}
