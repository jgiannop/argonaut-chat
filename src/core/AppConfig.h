#pragma once

#include <QString>
#include <QUrl>

// The application's own settings: fixed when it is built, not for people to
// change (as Signal's app talks only to Signal's servers). Changing one means
// a new release.
namespace AppConfig {

// The server every copy of the app signs up, links and connects through. A
// new address here reaches existing accounts too: the app moves them to it
// when it starts (Messenger::useServer), so nobody signs up again.
inline constexpr const char ServerUrl[] = "wss://188-34-165-32.sslip.io";

inline QUrl serverUrl()
{
    return QUrl(QString::fromLatin1(ServerUrl));
}

} // namespace AppConfig
