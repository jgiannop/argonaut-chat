#pragma once

#include <QString>

#include <optional>

// What a notification shows, as in Signal: a notification can be read on a
// locked screen or by whoever sits next to you, so how much it says is the
// user's choice (View › Notifications), kept in the settings.
namespace Notifications {

enum class Content { NameAndMessage, NameOnly, Nothing, Off };

Content saved();
void save(Content content);

struct Text
{
    QString title;
    QString body;
};

// Nothing when notifications are off.
std::optional<Text> make(Content content, const QString &from, const QString &message, bool request);

} // namespace Notifications
