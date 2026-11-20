#include "Notifications.h"

#include <QCoreApplication>
#include <QSettings>

namespace Notifications {

namespace {

const QString kKey = QStringLiteral("notifications/content");

QString tr(const char *text)
{
    return QCoreApplication::translate("Notifications", text);
}

} // namespace

Content saved()
{
    const QString value = QSettings().value(kKey).toString();
    if (value == QLatin1String("name"))
        return Content::NameOnly;
    if (value == QLatin1String("nothing"))
        return Content::Nothing;
    if (value == QLatin1String("off"))
        return Content::Off;
    return Content::NameAndMessage;
}

void save(Content content)
{
    QString value = QStringLiteral("all");
    if (content == Content::NameOnly)
        value = QStringLiteral("name");
    else if (content == Content::Nothing)
        value = QStringLiteral("nothing");
    else if (content == Content::Off)
        value = QStringLiteral("off");
    QSettings().setValue(kKey, value);
}

std::optional<Text> make(Content content, const QString &from, const QString &message, bool request)
{
    switch (content) {
    case Content::Off:
        return std::nullopt;
    case Content::Nothing:
        return Text{tr("Argonaut Chat"), tr("New message")};
    case Content::NameOnly:
        return Text{request ? tr("Message request from %1").arg(from) : from, tr("New message")};
    case Content::NameAndMessage:
        break;
    }
    QString body = message.simplified();
    if (body.size() > 200)
        body = body.left(199) + QChar(0x2026);
    return Text{request ? tr("Message request from %1").arg(from) : from, body};
}

} // namespace Notifications
