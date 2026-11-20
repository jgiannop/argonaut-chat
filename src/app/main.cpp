#include "ChatStore.h"
#include "KeyStore.h"
#include "MainWindow.h"
#include "Messenger.h"
#include "Theme.h"

#include <QApplication>
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

    ChatStore store;
    if (!store.isOpen())
        qWarning("The chat store couldn't be opened: %s", qPrintable(store.error()));
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
