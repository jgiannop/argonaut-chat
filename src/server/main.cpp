#include "ChatServer.h"
#include "Crypto.h"
#include "ServerStore.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QHostAddress>

#include <cstdio>

// argonaut-chat-server: the relay. Plain WebSockets on this computer only by
// default; put a reverse proxy (Caddy, nginx) in front for TLS.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("argonaut-chat-server"));
    QCoreApplication::setApplicationVersion(QStringLiteral(CHAT_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Argonaut Chat's relay server."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption listenOption(QStringLiteral("listen"),
                                          QStringLiteral("The address to listen on (default 127.0.0.1)."),
                                          QStringLiteral("address"), QStringLiteral("127.0.0.1"));
    const QCommandLineOption portOption(QStringLiteral("port"), QStringLiteral("The port (default 8790)."),
                                        QStringLiteral("port"), QStringLiteral("8790"));
    const QCommandLineOption databaseOption(
        QStringLiteral("database"), QStringLiteral("The database file (default %1).").arg(ServerStore::defaultFile()),
        QStringLiteral("file"));
    const QCommandLineOption trustProxyOption(
        QStringLiteral("trust-proxy"),
        QStringLiteral("Behind a reverse proxy on this computer: take the client's address from X-Forwarded-For "
                       "(for the rate limits)."));
    parser.addOptions({listenOption, portOption, databaseOption, trustProxyOption});
    parser.process(app);

    const QHostAddress address(parser.value(listenOption));
    bool portOk = false;
    const uint port = parser.value(portOption).toUInt(&portOk);
    if (address.isNull() || !portOk || port == 0 || port > 65535) {
        std::fprintf(stderr, "argonaut-chat-server: a bad address or port\n");
        return 2;
    }
    if (!Crypto::init()) {
        std::fprintf(stderr, "argonaut-chat-server: libsodium did not start\n");
        return 1;
    }
    ServerStore store(parser.value(databaseOption));
    if (!store.isOpen()) {
        std::fprintf(stderr, "argonaut-chat-server: cannot open the database: %s\n", qPrintable(store.error()));
        return 1;
    }
    ChatServer server(&store);
    server.setTrustProxy(parser.isSet(trustProxyOption));
    if (!server.listen(address, quint16(port))) {
        std::fprintf(stderr, "argonaut-chat-server: cannot listen: %s\n", qPrintable(server.errorString()));
        return 1;
    }
    if (!address.isLoopback())
        std::fprintf(stderr, "argonaut-chat-server: listening beyond this computer without TLS; "
                             "a reverse proxy should do TLS in front\n");
    std::printf("argonaut-chat-server: listening on %s:%u\n", qPrintable(address.toString()), server.port());
    std::fflush(stdout);
    return app.exec();
}
