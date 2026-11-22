#include "MainWindow.h"

#include "Dialogs.h"
#include "Messenger.h"
#include "Notifications.h"
#include "TrayIcon.h"
#include "Voice.h"
#include "StyleSheet.h"

#include <QAction>
#include <QCloseEvent>
#include <QSet>
#include <QSettings>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenuBar>
#include <QMimeData>
#include <QImageReader>
#include <QBuffer>
#include <QFileInfo>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>

namespace {

enum Role {
    IdRole = Qt::UserRole + 1,
    OutgoingRole,
    TimeRole,
    StatusRole,
    LastTextRole,
    UnreadRole,
    RequestRole,
    ImageRole,      // a picture's preview (QPixmap)
    FileRole,       // the attachment's line: name, size, state
    AttachmentRole, // the attachment's id, to save it
    AvatarRole,     // a conversation's picture (QPixmap), if they sent one
    PeerRole,       // a conversation's username (search finds it too)
    AuthorRole,     // in a group, who wrote it (theirs only)
    SystemRole,     // a note in the conversation, not a message
    ExpireRole,     // a disappearing message
    QuoteAuthorRole, // a reply: who wrote what it answers (as shown: "You", a name)
    QuoteTextRole,   // a reply: the start of what it answers
    ReactionsRole,   // its reactions as shown: "👍 2  ❤️"
    DeletedRole,     // deleted for everyone: only a mark is left
    EditedRole,      // edited after it was sent
    HighlightRole,   // the search's current match
    VoiceRole,       // a voice message's attachment (plays on a click)
    MutedRole,       // a conversation's notifications are off
    PinnedRole,      // a conversation pinned on top
    DividerRole,     // the first unread one: "New messages" drawn above it
};

QString shortTime(const QDateTime &when)
{
    const QDateTime local = when.toLocalTime();
    const QLocale locale;
    if (local.date() == QDate::currentDate())
        return locale.toString(local.time(), QLocale::ShortFormat);
    if (local.date().daysTo(QDate::currentDate()) < 7)
        return locale.toString(local.date(), QStringLiteral("ddd"));
    return locale.toString(local.date(), QStringLiteral("d MMM"));
}

// A conversation in the list: a round picture with its first letter, its
// name, the last message under it, the time and the unread count at the right.
class ConversationDelegate : public QStyledItemDelegate
{
public:
    ConversationDelegate(const Theme *theme, QObject *parent)
        : QStyledItemDelegate(parent)
        , m_theme(theme)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override { return {option.rect.width(), 64}; }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect.adjusted(2, 2, -2, -2);
        if (option.state & (QStyle::State_Selected | QStyle::State_MouseOver)) {
            QPainterPath bg;
            bg.addRoundedRect(r, 8, 8);
            p->fillPath(bg, (option.state & QStyle::State_Selected) ? m_theme->selected : m_theme->hover);
        }
        const QString title = index.data().toString();
        const QRect avatar(r.left() + 8, r.center().y() - 20, 40, 40);
        const QPixmap picture = index.data(AvatarRole).value<QPixmap>();
        if (!picture.isNull()) {
            // Their picture, in a circle.
            QPainterPath circle;
            circle.addEllipse(avatar);
            p->setClipPath(circle);
            p->drawPixmap(avatar, picture);
            p->setClipping(false);
        } else {
            p->setPen(Qt::NoPen);
            p->setBrush(m_theme->accent);
            p->drawEllipse(avatar);
            QFont letter = option.font;
            letter.setBold(true);
            letter.setPixelSize(17);
            p->setFont(letter);
            p->setPen(Qt::white);
            p->drawText(avatar, Qt::AlignCenter, title.left(1).toUpper());
        }

        const int unread = index.data(UnreadRole).toInt();
        const QRect text(avatar.right() + 12, r.top() + 10, r.right() - avatar.right() - 22, r.height() - 20);
        QFont name = option.font;
        name.setBold(true);
        const QFontMetrics nameMetrics(name);
        const QFontMetrics plain(option.font);
        // Pinned, muted: small marks before the time.
        const bool muted = index.data(MutedRole).toBool();
        QString time = index.data(TimeRole).toString();
        if (muted)
            time = QStringLiteral("🔕 ") + time;
        if (index.data(PinnedRole).toBool())
            time = QStringLiteral("📌 ") + time;
        time = time.trimmed();
        const int timeWidth = plain.horizontalAdvance(time) + 8;
        p->setFont(name);
        p->setPen(m_theme->fg);
        p->drawText(QRect(text.left(), text.top(), text.width() - timeWidth, nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    nameMetrics.elidedText(title, Qt::ElideRight, text.width() - timeWidth));
        p->setFont(option.font);
        p->setPen(m_theme->dimFg);
        p->drawText(QRect(text.left(), text.top(), text.width(), nameMetrics.height()), Qt::AlignRight | Qt::AlignVCenter, time);
        const QString last = index.data(LastTextRole).toString().simplified();
        int lastWidth = text.width();
        // A message request: marked, and no count (it is not "unread" until accepted).
        const bool request = index.data(RequestRole).toBool();
        if (unread > 0 || request) {
            const QString count = request ? QCoreApplication::translate("MainWindow", "Request") : QString::number(unread);
            const int w = qMax(20, plain.horizontalAdvance(count) + 10);
            const QRect badge(text.right() - w, text.bottom() - 19, w, 20);
            QPainterPath pill;
            pill.addRoundedRect(badge, 10, 10);
            p->fillPath(pill, muted && !request ? m_theme->dimFg : m_theme->accent); // muted: grey
            p->setPen(Qt::white);
            p->drawText(badge, Qt::AlignCenter, count);
            lastWidth -= w + 6;
        }
        p->setPen(m_theme->dimFg);
        p->drawText(QRect(text.left(), text.bottom() - plain.height(), lastWidth, plain.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    plain.elidedText(last, Qt::ElideRight, lastWidth));
        p->restore();
    }

private:
    const Theme *m_theme;
};

// The local files a paste or drop carries.
QStringList localFiles(const QMimeData *data)
{
    QStringList out;
    if (data && data->hasUrls()) {
        for (const QUrl &url : data->urls()) {
            if (url.isLocalFile())
                out << url.toLocalFile();
        }
    }
    return out;
}

// Files, or a picture with no text (a screenshot; an office program's copied
// text carries a picture of itself too: that stays text).
bool carriesAttachments(const QMimeData *data)
{
    return data && (!localFiles(data).isEmpty() || (data->hasImage() && !data->hasText()));
}

// The message field: files or a picture pasted or dropped on it go to
// `take` (true: taken); anything else is text, as usual.
class ComposeField : public QPlainTextEdit
{
public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<bool(const QMimeData *)> take;

protected:
    bool canInsertFromMimeData(const QMimeData *source) const override
    {
        return carriesAttachments(source) || QPlainTextEdit::canInsertFromMimeData(source);
    }
    void insertFromMimeData(const QMimeData *source) override
    {
        if (!(take && carriesAttachments(source) && take(source)))
            QPlainTextEdit::insertFromMimeData(source);
    }
};

// A message as a bubble: mine at the right in the accent colour, theirs at
// the left; a picture or a file line on top when it has an attachment, then
// the text, then the time (and for mine, whether it was sent). Selected
// ones (to copy) on a tinted band; a click on a reply's quote calls
// `quoteClicked`.
class BubbleDelegate : public QStyledItemDelegate
{
public:
    std::function<void(const QModelIndex &)> quoteClicked;

    BubbleDelegate(const Theme *theme, QObject *parent)
        : QStyledItemDelegate(parent)
        , m_theme(theme)
    {
    }

    static constexpr int Pad = 10;
    static constexpr int Gap = 6;
    static constexpr int QuotePad = 6;
    // A deleted message's mark in italics.
    static QFont textFont(const QStyleOptionViewItem &option, const QModelIndex &index)
    {
        QFont font = option.font;
        font.setItalic(index.data(DeletedRole).toBool());
        return font;
    }
    // The reactions' pill: overlapping the bubble's bottom edge by 6 px, as Signal's.
    static int reactionHeight(const QStyleOptionViewItem &option) { return QFontMetrics(option.font).height() + 8; }
    static constexpr int QuoteBar = 3;

    struct Layout
    {
        QRect image; // relative to the bubble's top left
        QRect file;
        QRect text;
        QRect author;
        QRect quote; // a reply's: what it answers
        int width = 0;
        int height = 0;
    };

    Layout layout(const QStyleOptionViewItem &option, const QModelIndex &index, const QString &footer) const
    {
        const int most = qMax(120, option.rect.width() * 7 / 10) - 2 * Pad;
        const QFontMetrics fm(option.font);
        QFont small = option.font;
        small.setPointSizeF(option.font.pointSizeF() * 0.85);
        const QFontMetrics smallMetrics(small);
        Layout l;
        int y = Pad;
        const QString author = index.data(AuthorRole).toString();
        if (!author.isEmpty()) {
            QFont bold = option.font;
            bold.setBold(true);
            const QFontMetrics boldMetrics(bold);
            l.author = QRect(Pad, y, qMin(most, boldMetrics.horizontalAdvance(author) + 2), boldMetrics.height());
            y += boldMetrics.height() + 2;
        }
        int width = smallMetrics.horizontalAdvance(footer);
        const QString quoteAuthor = index.data(QuoteAuthorRole).toString();
        if (!quoteAuthor.isEmpty()) {
            // Its author on one line, the start of its text on the next.
            QFont smallBold = small;
            smallBold.setBold(true);
            const int inner = qMax(QFontMetrics(smallBold).horizontalAdvance(quoteAuthor),
                                   smallMetrics.horizontalAdvance(index.data(QuoteTextRole).toString()));
            const int w = qMin(most, qMax(140, inner + 2 * QuotePad + QuoteBar));
            l.quote = QRect(Pad, y, w, QFontMetrics(smallBold).height() + smallMetrics.height() + 2 * QuotePad);
            y += l.quote.height() + Gap;
            width = qMax(width, w);
        }
        const QPixmap image = index.data(ImageRole).value<QPixmap>();
        if (!image.isNull()) {
            l.image = QRect(Pad, y, image.width(), image.height());
            y += image.height() + Gap;
            width = qMax(width, image.width());
        }
        const QString file = index.data(FileRole).toString();
        if (!file.isEmpty()) {
            QFont bold = option.font; // as it is drawn
            bold.setBold(true);
            const QFontMetrics boldMetrics(bold);
            const int w = qMin(most, boldMetrics.horizontalAdvance(file) + 2);
            l.file = QRect(Pad, y, w, boldMetrics.height());
            y += boldMetrics.height() + Gap;
            width = qMax(width, w);
        }
        const QString text = index.data().toString();
        if (!text.isEmpty()) {
            const QRect t = QFontMetrics(textFont(option, index)).boundingRect(QRect(0, 0, most, 100000), Qt::TextWordWrap, text);
            l.text = QRect(Pad, y, t.width(), t.height());
            y += t.height();
            width = qMax(width, t.width());
        }
        width = qMax(width, l.author.width());
        l.width = width + 2 * Pad;
        l.height = y + smallMetrics.height() + Pad + 2;
        return l;
    }

    static QString footerFor(const QModelIndex &index, bool *read)
    {
        QString footer = index.data(TimeRole).toString();
        if (index.data(EditedRole).toBool())
            footer = QCoreApplication::translate("MainWindow", "edited") + QStringLiteral(" · ") + footer;
        if (index.data(ExpireRole).toBool())
            footer = QStringLiteral("⏱ ") + footer;
        *read = false;
        if (index.data(OutgoingRole).toBool()) {
            const QString status = index.data(StatusRole).toString();
            *read = status == QLatin1String("read");
            if (status == QLatin1String("read") || status == QLatin1String("delivered"))
                footer += QStringLiteral("  ✓✓");
            else if (status == QLatin1String("sent"))
                footer += QStringLiteral("  ✓");
            else if (status == QLatin1String("sending"))
                footer += QStringLiteral("  …");
            else if (status == QLatin1String("failed"))
                footer += QStringLiteral("  ⚠ ") + QCoreApplication::translate("MainWindow", "Not sent");
        }
        return footer;
    }

    // The first unread one: "New messages" across the top of its row.
    static int dividerHeight(const QStyleOptionViewItem &option, const QModelIndex &index)
    {
        return index.data(DividerRole).toBool() ? QFontMetrics(option.font).height() + 16 : 0;
    }
    // The row without that line: where the message itself goes.
    static QStyleOptionViewItem below(const QStyleOptionViewItem &option, const QModelIndex &index)
    {
        QStyleOptionViewItem o = option;
        o.rect.setTop(o.rect.top() + dividerHeight(option, index));
        return o;
    }
    void paintDivider(QPainter *p, const QStyleOptionViewItem &option) const
    {
        QFont bold = option.font;
        bold.setBold(true);
        bold.setPointSizeF(option.font.pointSizeF() * 0.85);
        const QFontMetrics fm(bold);
        const QString text = QCoreApplication::translate("MainWindow", "New messages");
        const int y = option.rect.top() + (QFontMetrics(option.font).height() + 16) / 2;
        const int w = fm.horizontalAdvance(text) + 24;
        const QRect label(option.rect.center().x() - w / 2, y - fm.height() / 2, w, fm.height());
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(m_theme->accent, 1));
        p->drawLine(option.rect.left() + 14, y, label.left(), y);
        p->drawLine(label.right(), y, option.rect.right() - 14, y);
        p->setFont(bold);
        p->drawText(label, Qt::AlignCenter, text);
        p->restore();
    }

    static QRect bubbleRect(const QStyleOptionViewItem &option, const QModelIndex &index, const Layout &l)
    {
        const bool mine = index.data(OutgoingRole).toBool();
        return {mine ? option.rect.right() - 14 - l.width : option.rect.left() + 14, option.rect.top() + 4, l.width, l.height};
    }

    // Where a reply's quote is in the view (empty: none).
    QRect quoteRect(const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        if (index.data(SystemRole).toBool() || index.data(QuoteAuthorRole).toString().isEmpty())
            return {};
        bool read = false;
        const QStyleOptionViewItem o = below(option, index);
        const Layout l = layout(o, index, footerFor(index, &read));
        return l.quote.translated(bubbleRect(o, index, l).topLeft());
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease && quoteClicked) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && !(mouse->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
                && quoteRect(option, index).contains(mouse->position().toPoint())) {
                quoteClicked(index);
                return true;
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const int line = dividerHeight(option, index);
        if (index.data(SystemRole).toBool())
            return {option.rect.width(), line + QFontMetrics(option.font).height() + 22};
        bool read = false;
        const int reactions = index.data(ReactionsRole).toString().isEmpty() ? 0 : reactionHeight(option) - 6;
        return {option.rect.width(), line + layout(option, index, footerFor(index, &read)).height + 8 + reactions};
    }

    void paint(QPainter *p, const QStyleOptionViewItem &whole, const QModelIndex &index) const override
    {
        if (dividerHeight(whole, index) > 0)
            paintDivider(p, whole);
        const QStyleOptionViewItem option = below(whole, index);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (index.data(SystemRole).toBool()) {
            // A note: small, in the middle, no bubble of anyone's.
            const QFontMetrics fm(option.font);
            const QString text = index.data().toString();
            const int w = qMin(option.rect.width() - 40, fm.horizontalAdvance(text) + 24);
            const QRect pill(option.rect.center().x() - w / 2, option.rect.top() + 6, w, fm.height() + 10);
            QPainterPath path;
            path.addRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
            p->fillPath(path, m_theme->pathBg);
            p->setPen(m_theme->dimFg);
            p->drawText(pill, Qt::AlignCenter, fm.elidedText(text, Qt::ElideRight, w - 16));
            p->restore();
            return;
        }
        const bool mine = index.data(OutgoingRole).toBool();
        bool read = false;
        const QString footer = footerFor(index, &read);
        const Layout l = layout(option, index, footer);
        QFont small = option.font;
        small.setPointSizeF(option.font.pointSizeF() * 0.85);
        const QFontMetrics smallMetrics(small);
        const QRect bubble = bubbleRect(option, index, l);
        if (option.state & QStyle::State_Selected)
            p->fillRect(option.rect, m_theme->itemSelected); // chosen to copy
        QPainterPath path;
        path.addRoundedRect(bubble, 16, 16);
        p->fillPath(path, mine ? m_theme->accent : m_theme->pathBg);
        if (index.data(HighlightRole).toBool()) {
            // The search's match: a ring around it.
            p->setPen(QPen(mine ? m_theme->fg : m_theme->accent, 2));
            p->drawPath(path);
        }
        const QPoint origin = bubble.topLeft();
        const QPixmap image = index.data(ImageRole).value<QPixmap>();
        if (!image.isNull()) {
            QPainterPath rounded;
            rounded.addRoundedRect(l.image.translated(origin), 10, 10);
            p->setClipPath(rounded);
            p->drawPixmap(l.image.translated(origin), image);
            p->setClipping(false);
        }
        p->setPen(mine ? QColor(Qt::white) : m_theme->fg);
        p->setFont(option.font);
        if (!l.author.isNull()) {
            QFont bold = option.font;
            bold.setBold(true);
            p->setFont(bold);
            p->setPen(m_theme->accent);
            p->drawText(l.author.translated(origin), Qt::AlignLeft | Qt::AlignVCenter, index.data(AuthorRole).toString());
            p->setPen(mine ? QColor(Qt::white) : m_theme->fg);
            p->setFont(option.font);
        }
        if (!l.quote.isNull()) {
            // A reply: what it answers, in a box with a bar at its left, as Signal shows it.
            const QRect box = l.quote.translated(origin);
            QPainterPath boxPath;
            boxPath.addRoundedRect(box, 8, 8);
            p->fillPath(boxPath, mine ? QColor(255, 255, 255, 45) : m_theme->window);
            p->save();
            p->setClipPath(boxPath);
            p->fillRect(QRect(box.left(), box.top(), QuoteBar, box.height()), mine ? QColor(Qt::white) : m_theme->accent);
            p->restore();
            QFont smallBold = small;
            smallBold.setBold(true);
            const QFontMetrics boldMetrics(smallBold);
            const QRect inner = box.adjusted(QuoteBar + QuotePad, QuotePad, -QuotePad, -QuotePad);
            p->setFont(smallBold);
            p->setPen(mine ? QColor(Qt::white) : m_theme->accent);
            p->drawText(QRect(inner.left(), inner.top(), inner.width(), boldMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                        boldMetrics.elidedText(index.data(QuoteAuthorRole).toString(), Qt::ElideRight, inner.width()));
            p->setFont(small);
            p->setPen(mine ? QColor(255, 255, 255, 215) : m_theme->dimFg);
            p->drawText(QRect(inner.left(), inner.top() + boldMetrics.height(), inner.width(), smallMetrics.height()),
                        Qt::AlignLeft | Qt::AlignVCenter,
                        smallMetrics.elidedText(index.data(QuoteTextRole).toString(), Qt::ElideRight, inner.width()));
            p->setPen(mine ? QColor(Qt::white) : m_theme->fg);
            p->setFont(option.font);
        }
        const QString file = index.data(FileRole).toString();
        if (!file.isEmpty()) {
            QFont bold = option.font;
            bold.setBold(true);
            p->setFont(bold);
            p->drawText(l.file.translated(origin), Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(bold).elidedText(file, Qt::ElideMiddle, l.file.width()));
            p->setFont(option.font);
        }
        if (!l.text.isNull()) {
            const bool deleted = index.data(DeletedRole).toBool();
            p->setFont(textFont(option, index));
            if (deleted)
                p->setPen(mine ? QColor(255, 255, 255, 200) : m_theme->dimFg);
            p->drawText(l.text.translated(origin), Qt::TextWordWrap, index.data().toString());
            p->setFont(option.font);
        }
        p->setFont(small);
        // Read: the ticks in full white, as Signal fills them in.
        p->setPen(mine ? QColor(255, 255, 255, read ? 255 : 190) : m_theme->dimFg);
        p->drawText(QRect(bubble.left() + Pad, bubble.bottom() - Pad - smallMetrics.height(), bubble.width() - 2 * Pad,
                          smallMetrics.height()),
                    Qt::AlignRight | Qt::AlignVCenter, footer);
        const QString reactions = index.data(ReactionsRole).toString();
        if (!reactions.isEmpty()) {
            const QFontMetrics fm(option.font);
            const int h = reactionHeight(option);
            const int w = fm.horizontalAdvance(reactions) + 16;
            const QRect pill(mine ? bubble.right() - 12 - w : bubble.left() + 12, bubble.bottom() - 6, w, h);
            QPainterPath pillPath;
            pillPath.addRoundedRect(pill, h / 2.0, h / 2.0);
            p->fillPath(pillPath, m_theme->window);
            p->setPen(QPen(m_theme->border, 1));
            p->drawPath(pillPath);
            p->setPen(m_theme->fg);
            p->setFont(option.font);
            p->drawText(pill, Qt::AlignCenter, reactions);
        }
        p->restore();
    }

private:
    const Theme *m_theme;
};

} // namespace

MainWindow::MainWindow(ChatStore *store, Messenger *messenger, QWidget *parent)
    : QMainWindow(parent)
    , m_store(store)
    , m_messenger(messenger)
    , m_theme(currentTheme())
{
    setWindowTitle(tr("Argonaut Chat"));
    setWindowIcon(applicationIcon());
    resize(1000, 700);

    // The conversations.
    auto *sidebar = new QWidget(this);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);
    auto *sideTitle = new QLabel(tr("Chats"), sidebar);
    sideTitle->setObjectName(QStringLiteral("sidebarTitle"));
    auto *search = m_search = new QLineEdit(sidebar);
    search->setObjectName(QStringLiteral("searchField"));
    search->setPlaceholderText(tr("Search"));
    search->setClearButtonEnabled(true);
    m_conversations = new QListWidget(sidebar);
    m_conversations->setObjectName(QStringLiteral("conversationList"));
    m_conversations->setItemDelegate(new ConversationDelegate(&m_theme, m_conversations));
    m_conversations->setMouseTracking(true);
    m_conversations->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_connection = new QLabel(sidebar);
    m_connection->setObjectName(QStringLiteral("connectionState"));
    m_notice = new QLabel(sidebar);
    m_notice->setObjectName(QStringLiteral("notice"));
    m_notice->setWordWrap(true);
    m_notice->hide();
    sideLayout->addWidget(sideTitle);
    sideLayout->addWidget(m_connection);
    sideLayout->addWidget(m_notice);
    // "Archived Chats (3)" when there are some; in there, the way back.
    m_archivedButton = new QToolButton(sidebar);
    m_archivedButton->setObjectName(QStringLiteral("archivedButton"));
    m_archivedButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_archivedButton->setAutoRaise(true);
    m_archivedButton->setCursor(Qt::PointingHandCursor);
    m_archivedButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_archivedButton->hide();
    connect(m_archivedButton, &QToolButton::clicked, this, [this] { showArchived(!m_showArchived); });
    sideLayout->addWidget(search);
    sideLayout->addWidget(m_archivedButton);
    sideLayout->addWidget(m_conversations, 1);
    connect(search, &QLineEdit::textChanged, this, &MainWindow::applySearch);
    m_conversations->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_conversations, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_conversations->itemAt(pos);
        if (!item)
            return;
        std::unique_ptr<QMenu> menu(conversationMenu(item->data(IdRole).toLongLong()));
        if (menu && !menu->isEmpty())
            menu->exec(m_conversations->viewport()->mapToGlobal(pos));
    });
    m_muteTimer.setSingleShot(true);
    connect(&m_muteTimer, &QTimer::timeout, this, [this] {
        reloadConversations();
        updateUnread();
    });

    // The chosen conversation.
    auto *chat = new QWidget(this);
    chat->setObjectName(QStringLiteral("chatPane"));
    auto *chatLayout = new QVBoxLayout(chat);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(0);
    auto *header = new QWidget(chat);
    header->setObjectName(QStringLiteral("chatHeader"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 10, 16, 10);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    m_title = new QLabel(header);
    m_title->setObjectName(QStringLiteral("chatTitle"));
    m_subtitle = new QLabel(header);
    m_subtitle->setObjectName(QStringLiteral("chatSubtitle"));
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);
    headerLayout->addLayout(titles, 1);
    m_safety = new QToolButton(header);
    m_safety->setObjectName(QStringLiteral("safetyNumberButton"));
    m_safety->setText(tr("Safety Number"));
    m_safety->setToolTip(tr("Check that your messages reach only them"));
    m_safety->setCursor(Qt::PointingHandCursor);
    headerLayout->addWidget(m_safety);
    connect(m_safety, &QToolButton::clicked, this, &MainWindow::showSafetyNumber);
    m_messages = new QListWidget(chat);
    m_messages->setObjectName(QStringLiteral("messageList"));
    auto *bubbles = new BubbleDelegate(&m_theme, m_messages);
    m_messages->setItemDelegate(bubbles);
    // Chosen with a click (Ctrl/Shift for more, Ctrl+A all) to copy with Ctrl+C.
    m_messages->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_messages->setFocusPolicy(Qt::ClickFocus);
    m_messages->installEventFilter(this);
    m_messages->viewport()->setAcceptDrops(true); // files dropped on the conversation
    m_messages->viewport()->installEventFilter(this);
    bubbles->quoteClicked = [this](const QModelIndex &index) {
        // After the click is over: going there rebuilds the list under it.
        m_quoteClicked = true;
        const qint64 reply = index.data(IdRole).toLongLong();
        QTimer::singleShot(0, this, [this, reply] {
            m_quoteClicked = false;
            jumpToQuote(reply);
        });
    };
    m_flashTimer.setSingleShot(true);
    m_flashTimer.setInterval(1500);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        QListWidgetItem *item = messageItem(m_flash);
        if (item && m_flash != currentFindMatch())
            item->setData(HighlightRole, false);
        m_flash = 0;
    });
    m_messages->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_messages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messages->setResizeMode(QListView::Adjust);
    m_messages->setWordWrap(true);
    m_messages->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_messages, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_messages->itemAt(pos);
        if (!item)
            return;
        // On one of several chosen: copy them all.
        if (item->isSelected() && m_messages->selectedItems().size() > 1) {
            QMenu menu;
            QAction *copy = menu.addAction(tr("Copy %n Messages", nullptr, int(m_messages->selectedItems().size())), this,
                                           &MainWindow::copySelectedMessages);
            copy->setShortcut(QKeySequence::Copy); // shown; the list's own key does it
            copy->setObjectName(QStringLiteral("copyMessagesAction"));
            menu.exec(m_messages->viewport()->mapToGlobal(pos));
            return;
        }
        std::unique_ptr<QMenu> menu(messageMenu(item->data(IdRole).toLongLong()));
        if (menu && !menu->isEmpty())
            menu->exec(m_messages->viewport()->mapToGlobal(pos));
    });
    // ↓ at the bottom right of the messages, while further up.
    m_toBottom = new QToolButton(m_messages);
    m_toBottom->setObjectName(QStringLiteral("toBottomButton"));
    m_toBottom->setText(QStringLiteral("↓"));
    m_toBottom->setToolTip(tr("Go to the newest message"));
    m_toBottom->setCursor(Qt::PointingHandCursor);
    m_toBottom->hide();
    connect(m_toBottom, &QToolButton::clicked, this, &MainWindow::scrollToNewest);
    connect(m_messages->verticalScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::updateToBottom);
    connect(m_messages->verticalScrollBar(), &QScrollBar::rangeChanged, this, &MainWindow::updateToBottom);
    auto *composer = new QWidget(chat);
    composer->setObjectName(QStringLiteral("composer"));
    auto *composerOuter = new QVBoxLayout(composer);
    composerOuter->setContentsMargins(12, 8, 12, 10);
    composerOuter->setSpacing(6);
    // Above the field while replying: what the reply answers, and ✕ to stop.
    m_replyBar = new QWidget(composer);
    m_replyBar->setObjectName(QStringLiteral("replyBar"));
    m_replyBar->setAttribute(Qt::WA_StyledBackground);
    auto *replyLayout = new QHBoxLayout(m_replyBar);
    replyLayout->setContentsMargins(10, 6, 6, 6);
    m_replyLabel = new QLabel(m_replyBar);
    m_replyLabel->setObjectName(QStringLiteral("replyLabel"));
    m_replyLabel->setTextFormat(Qt::PlainText);
    auto *cancelReply = new QToolButton(m_replyBar);
    cancelReply->setObjectName(QStringLiteral("cancelReplyButton"));
    cancelReply->setText(QStringLiteral("✕"));
    cancelReply->setToolTip(tr("Don't reply (Esc)"));
    cancelReply->setAutoRaise(true);
    connect(cancelReply, &QToolButton::clicked, this, &MainWindow::cancelReply);
    replyLayout->addWidget(m_replyLabel, 1);
    replyLayout->addWidget(cancelReply);
    m_replyBar->hide();
    composerOuter->addWidget(m_replyBar);
    // Above the field: the files that go with the next message.
    m_attachBar = new QWidget(composer);
    m_attachBar->setObjectName(QStringLiteral("attachBar"));
    m_attachBar->setAttribute(Qt::WA_StyledBackground);
    auto *attachLayout = new QHBoxLayout(m_attachBar);
    attachLayout->setContentsMargins(6, 6, 6, 6);
    attachLayout->setSpacing(6);
    m_attachBar->hide();
    composerOuter->addWidget(m_attachBar);
    auto *composerLayout = new QHBoxLayout;
    composerLayout->setContentsMargins(0, 0, 0, 0);
    composerOuter->addLayout(composerLayout);
    auto *field = new ComposeField(composer);
    field->take = [this](const QMimeData *data) { return takeAttachments(data); };
    m_compose = field;
    m_compose->setObjectName(QStringLiteral("composeField"));
    m_compose->setPlaceholderText(tr("Message"));
    m_compose->setFixedHeight(40);
    m_compose->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_compose->installEventFilter(this);
    m_send = new QPushButton(tr("Send"), composer);
    m_send->setObjectName(QStringLiteral("sendButton"));
    m_send->setCursor(Qt::PointingHandCursor);
    m_send->setEnabled(false);
    m_attach = new QToolButton(composer);
    m_attach->setObjectName(QStringLiteral("attachButton"));
    m_attach->setText(QStringLiteral("📎"));
    m_attach->setToolTip(tr("Attach files (up to 25 MB each), encrypted; you can paste or drop them here too"));
    m_attach->setCursor(Qt::PointingHandCursor);
    m_attach->setEnabled(m_messenger != nullptr);
    connect(m_attach, &QToolButton::clicked, this, [this] {
        for (const QString &path : QFileDialog::getOpenFileNames(this, tr("Attach Files"))) {
            if (!addAttachment(path))
                break;
        }
    });
    m_voiceButton = new QToolButton(composer);
    m_voiceButton->setObjectName(QStringLiteral("voiceButton"));
    m_voiceButton->setText(QStringLiteral("🎤"));
    m_voiceButton->setToolTip(tr("Record a voice message (up to %1 minutes), encrypted").arg(Voice::MaxSeconds / 60));
    m_voiceButton->setCursor(Qt::PointingHandCursor);
    m_voiceButton->setEnabled(m_messenger != nullptr);
    connect(m_voiceButton, &QToolButton::clicked, this, &MainWindow::startRecording);
    // While recording: in place of the reply bar, the time, Cancel and Send.
    m_recordBar = new QWidget(composer);
    m_recordBar->setObjectName(QStringLiteral("recordBar"));
    m_recordBar->setAttribute(Qt::WA_StyledBackground);
    auto *recordLayout = new QHBoxLayout(m_recordBar);
    recordLayout->setContentsMargins(10, 4, 6, 4);
    m_recordLabel = new QLabel(m_recordBar);
    m_recordLabel->setObjectName(QStringLiteral("recordLabel"));
    auto *cancelRecording = new QPushButton(tr("Cancel"), m_recordBar);
    cancelRecording->setObjectName(QStringLiteral("cancelRecordingButton"));
    auto *sendRecording = new QPushButton(tr("Send"), m_recordBar);
    sendRecording->setObjectName(QStringLiteral("sendRecordingButton"));
    recordLayout->addWidget(m_recordLabel, 1);
    recordLayout->addWidget(cancelRecording);
    recordLayout->addWidget(sendRecording);
    m_recordBar->hide();
    composerOuter->insertWidget(0, m_recordBar);
    connect(cancelRecording, &QPushButton::clicked, this, [this] { stopRecording(false); });
    connect(sendRecording, &QPushButton::clicked, this, [this] { stopRecording(true); });
    connect(&m_recorder, &VoiceRecorder::tick, this, [this](qint64 ms) {
        m_recordLabel->setText(tr("● Recording %1").arg(Voice::formatDuration(ms)));
    });
    connect(&m_recorder, &VoiceRecorder::limitReached, this, [this] { stopRecording(true); });
    connect(&m_player, &VoicePlayer::progress, this, [this](qint64, qint64, qint64) { refreshVoiceLines(); });
    composerLayout->addWidget(m_attach);
    composerLayout->addWidget(m_voiceButton);
    composerLayout->addWidget(m_compose, 1);
    composerLayout->addWidget(m_send);
    // In place of the composer for a message request.
    m_requestBar = new QWidget(chat);
    m_requestBar->setObjectName(QStringLiteral("requestBar"));
    auto *requestLayout = new QVBoxLayout(m_requestBar);
    requestLayout->setContentsMargins(16, 12, 16, 12);
    m_requestText = new QLabel(m_requestBar);
    m_requestText->setObjectName(QStringLiteral("requestText"));
    m_requestText->setWordWrap(true);
    m_requestText->setAlignment(Qt::AlignCenter);
    auto *requestButtons = new QHBoxLayout;
    requestButtons->addStretch();
    m_blockButton = new QPushButton(tr("Block"), m_requestBar);
    m_blockButton->setObjectName(QStringLiteral("blockButton"));
    m_deleteButton = new QPushButton(tr("Delete"), m_requestBar);
    m_deleteButton->setObjectName(QStringLiteral("deleteButton"));
    m_acceptButton = new QPushButton(tr("Accept"), m_requestBar);
    m_acceptButton->setObjectName(QStringLiteral("acceptButton"));
    for (QPushButton *button : {m_blockButton, m_deleteButton, m_acceptButton}) {
        button->setCursor(Qt::PointingHandCursor);
        requestButtons->addWidget(button);
    }
    requestButtons->addStretch();
    requestLayout->addWidget(m_requestText);
    requestLayout->addLayout(requestButtons);
    m_requestBar->hide();
    connect(m_acceptButton, &QPushButton::clicked, this, &MainWindow::acceptRequest);
    connect(m_deleteButton, &QPushButton::clicked, this, [this] { deleteRequest(true); });
    connect(m_blockButton, &QPushButton::clicked, this, [this] { blockRequest(true); });
    m_composer = composer;

    // Find in Conversation (Ctrl+F): a bar under the header.
    m_findBar = new QWidget(chat);
    m_findBar->setObjectName(QStringLiteral("findBar"));
    m_findBar->setAttribute(Qt::WA_StyledBackground);
    auto *findLayout = new QHBoxLayout(m_findBar);
    findLayout->setContentsMargins(12, 6, 8, 6);
    m_findField = new QLineEdit(m_findBar);
    m_findField->setObjectName(QStringLiteral("findField"));
    m_findField->setPlaceholderText(tr("Find in this conversation"));
    m_findField->setClearButtonEnabled(true);
    m_findField->installEventFilter(this);
    m_findCount = new QLabel(m_findBar);
    m_findCount->setObjectName(QStringLiteral("findCount"));
    auto *older = new QToolButton(m_findBar);
    older->setText(QStringLiteral("↑"));
    older->setToolTip(tr("Older (Enter)"));
    older->setAutoRaise(true);
    auto *newer = new QToolButton(m_findBar);
    newer->setText(QStringLiteral("↓"));
    newer->setToolTip(tr("Newer (Shift+Enter)"));
    newer->setAutoRaise(true);
    auto *closeFindButton = new QToolButton(m_findBar);
    closeFindButton->setText(QStringLiteral("✕"));
    closeFindButton->setToolTip(tr("Close (Esc)"));
    closeFindButton->setAutoRaise(true);
    findLayout->addWidget(m_findField, 1);
    findLayout->addWidget(m_findCount);
    findLayout->addWidget(older);
    findLayout->addWidget(newer);
    findLayout->addWidget(closeFindButton);
    m_findBar->hide();
    connect(m_findField, &QLineEdit::textChanged, this, &MainWindow::runFind);
    connect(older, &QToolButton::clicked, this, [this] { findStep(-1); });
    connect(newer, &QToolButton::clicked, this, [this] { findStep(1); });
    connect(closeFindButton, &QToolButton::clicked, this, &MainWindow::closeFind);

    chatLayout->addWidget(header);
    chatLayout->addWidget(m_findBar);
    chatLayout->addWidget(m_messages, 1);
    chatLayout->addWidget(composer);
    chatLayout->addWidget(m_requestBar);
    m_leftNote = new QLabel(tr("You are no longer in this group."), chat);
    m_leftNote->setObjectName(QStringLiteral("leftNote"));
    m_leftNote->setAlignment(Qt::AlignCenter);
    m_leftNote->hide();
    chatLayout->addWidget(m_leftNote);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(sidebar);
    split->addWidget(chat);
    split->setStretchFactor(1, 1);
    split->setSizes({300, 700});
    setCentralWidget(split);

    connect(m_send, &QPushButton::clicked, this, &MainWindow::send);
    connect(m_compose, &QPlainTextEdit::textChanged, this, [this] {
        updateSendButton();
        // The field grows with what is written, up to six lines.
        const int lines = qBound(1, int(m_compose->document()->size().height()), 6);
        m_compose->setFixedHeight(qMax(40, lines * m_compose->fontMetrics().lineSpacing() + 18));
        // Typing: said while there is something new written, and for 5 s after the last key.
        if (m_messenger && m_current != 0 && m_editing == 0) {
            const bool typing = !m_compose->toPlainText().trimmed().isEmpty();
            m_messenger->setTyping(m_current, typing);
            if (typing)
                m_typingIdle.start();
        }
    });
    m_typingIdle.setSingleShot(true);
    m_typingIdle.setInterval(5000);
    connect(&m_typingIdle, &QTimer::timeout, this, [this] {
        if (m_messenger && m_current != 0)
            m_messenger->setTyping(m_current, false);
    });
    connect(m_conversations, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item)
            openConversation(item->data(IdRole).toLongLong());
    });
    connect(m_store, &ChatStore::conversationsChanged, this, &MainWindow::reloadConversations);
    connect(m_store, &ChatStore::conversationsChanged, this, &MainWindow::updateUnread);
    connect(m_store, &ChatStore::conversationsChanged, this, [this] {
        if (m_current != 0)
            updateHeader();
    });
    connect(m_store, &ChatStore::profileChanged, this, [this](const QString &username) {
        m_avatars.remove(username);
        reloadConversations();
        updateHeader();
    });
    connect(m_store, &ChatStore::messagesChanged, this, [this](qint64 conversation) {
        if (conversation != m_current)
            return;
        reloadMessages();
        // Arrived while it is open in front of them: seen.
        if (isActiveWindow())
            QTimer::singleShot(0, this, &MainWindow::markCurrentRead);
    });
    connect(m_messages, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (m_quoteClicked || QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
            return; // going to the quoted one, or choosing several
        const qint64 id = item->data(AttachmentRole).toLongLong();
        const ChatStore::StoredAttachment a = m_store->attachment(id);
        if (id == 0 || a.file.isEmpty())
            return;
        if (a.voice) {
            toggleVoice(id); // saved from its menu
            return;
        }
        const QString folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QString path = QFileDialog::getSaveFileName(this, tr("Save"), folder + QLatin1Char('/') + a.name);
        if (!path.isEmpty())
            saveAttachment(id, path);
    });
    if (m_messenger) {
        connect(m_messenger, &Messenger::attachmentProgress, this, [this](qint64 id, qint64 done, qint64 total) {
            m_progress.insert(id, total > 0 ? int(done * 100 / total) : 0);
            for (int i = 0; i < m_messages->count(); ++i) {
                QListWidgetItem *item = m_messages->item(i);
                if (item->data(AttachmentRole).toLongLong() == id)
                    item->setData(FileRole, item->data(VoiceRole).toBool()
                                                ? voiceLine(m_store->attachment(id))
                                                : fileLine(m_store->attachment(id), !item->data(ImageRole).isNull()));
            }
        });
        connect(m_messenger, &Messenger::stateChanged, this, &MainWindow::updateConnection);
        connect(m_messenger, &Messenger::safetyNumberChanged, this, [this](const QString &username) {
            updateHeader();
            showNotice(tr("Your safety number with %1 has changed.").arg(username));
        });
        connect(m_messenger, &Messenger::problem, this, &MainWindow::showNotice);
        connect(m_messenger, &Messenger::messageReceived, this, &MainWindow::notify);
        connect(m_messenger, &Messenger::typingChanged, this, [this](qint64 conversation) {
            if (conversation == m_current)
                updateHeader();
            // In the list too, in place of the last message.
            for (int i = 0; i < m_conversations->count(); ++i) {
                QListWidgetItem *item = m_conversations->item(i);
                if (item->data(IdRole).toLongLong() != conversation)
                    continue;
                const QString typing = typingText(conversation);
                const ChatStore::Conversation c = m_store->conversation(conversation);
                item->setData(LastTextRole, !typing.isEmpty() ? typing
                                            : c.lastDeleted ? tr("This message was deleted") : c.lastText);
            }
        });
        connect(m_messenger, &Messenger::reactionReceived, this,
                [this](qint64 conversation, const QString &from, const QString &emoji, const QString &onText) {
                    notify(conversation, from, tr("Reacted %1 to “%2”").arg(emoji, onText), false);
                });
    }

    buildMenus();
    auto *trayMenu = new QMenu(this);
    trayMenu->addAction(tr("Open Argonaut Chat"), this, &MainWindow::showAndRaise);
    trayMenu->addSeparator();
    trayMenu->addAction(tr("Quit"), this, &MainWindow::quit);
    m_tray = new TrayIcon(trayMenu, this);
    connect(m_tray, &TrayIcon::activated, this, &MainWindow::showAndRaise);
    connect(m_tray, &TrayIcon::messageClicked, this, [this] {
        showAndRaise();
        if (m_lastNotified && m_store->conversation(m_lastNotified).id)
            openConversation(m_lastNotified);
    });
    updateConnection();
    applyTheme();
    m_store->noteToSelf(); // always there, as in Signal
    reloadConversations();
    updateUnread();
    if (m_conversations->count() > 0)
        m_conversations->setCurrentRow(0);
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("&File"));
    m_newChatAction = file->addAction(tr("New Chat…"), this, &MainWindow::newChat);
    m_newChatAction->setShortcut(QKeySequence::New);
    m_signUpAction = file->addAction(tr("Sign Up…"), this, &MainWindow::showSignUp);
    m_newGroupAction = file->addAction(tr("New Group…"), this, [this] {
        if (!m_messenger)
            return;
        GroupDialog dialog(m_messenger, 0, this);
        if (dialog.exec() == QDialog::Accepted && dialog.conversation())
            openConversation(dialog.conversation());
    });
    m_devicesAction = file->addAction(tr("Linked Devices…"), this, [this] {
        if (!m_messenger)
            return;
        DevicesDialog dialog(m_messenger, this);
        dialog.exec();
    });
    m_profileAction = file->addAction(tr("Profile…"), this, [this] {
        if (!m_messenger)
            return;
        ProfileDialog dialog(m_messenger, this);
        dialog.exec();
    });
    m_blockedAction = file->addAction(tr("Blocked People…"), this, [this] {
        if (!m_messenger)
            return;
        BlockedDialog dialog(m_messenger, this);
        dialog.exec();
    });
    file->addSeparator();
    QAction *quit = file->addAction(tr("Quit"), this, &MainWindow::quit);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *conversation = menuBar()->addMenu(tr("&Conversation"));
    QAction *find = conversation->addAction(tr("Find in Conversation…"), this, &MainWindow::showFind);
    find->setShortcut(QKeySequence::Find);
    conversation->addSeparator();
    m_safetyAction = conversation->addAction(tr("Safety Number…"), this, &MainWindow::showSafetyNumber);
    conversation->addSeparator();
    m_membersAction = conversation->addAction(tr("Members…"), this, [this] {
        if (!m_messenger)
            return;
        GroupDialog dialog(m_messenger, m_current, this);
        dialog.exec();
        updateHeader();
    });
    m_leaveAction = conversation->addAction(tr("Leave Group…"), this, [this] { leaveGroup(true); });
    conversation->addSeparator();
    m_pinAction = conversation->addAction(tr("Pin Chat"), this, [this] {
        pinConversation(m_current, m_store->conversation(m_current).pinned == 0);
    });
    m_muteMenu = conversation->addMenu(tr("Mute Notifications"));
    m_archiveAction = conversation->addAction(tr("Archive Chat"), this, [this] {
        archiveConversation(m_current, !m_store->conversation(m_current).archived);
    });
    connect(conversation, &QMenu::aboutToShow, this, &MainWindow::updateChatActions);
    updateChatActions();
    conversation->addSeparator();
    m_timerMenu = conversation->addMenu(tr("Disappearing Messages"));
    auto *timers = new QActionGroup(m_timerMenu);
    for (qint64 seconds : {0LL, 30LL, 300LL, 3600LL, 8 * 3600LL, 24 * 3600LL, 7 * 24 * 3600LL, 28 * 24 * 3600LL}) {
        QAction *a = m_timerMenu->addAction(seconds == 0 ? tr("Off") : Messenger::describeTimer(seconds));
        a->setCheckable(true);
        a->setData(seconds);
        timers->addAction(a);
        connect(a, &QAction::triggered, this, [this, seconds] { setDisappearing(seconds); });
    }

    QMenu *view = menuBar()->addMenu(tr("&View"));
    QMenu *notifications = view->addMenu(tr("Notifications"));
    auto *notificationGroup = new QActionGroup(notifications);
    const QList<QPair<QString, Notifications::Content>> choices{
        {tr("Name and Message"), Notifications::Content::NameAndMessage},
        {tr("Name Only"), Notifications::Content::NameOnly},
        {tr("No Name or Message"), Notifications::Content::Nothing},
        {tr("Off"), Notifications::Content::Off},
    };
    for (const auto &[label, content] : choices) {
        QAction *a = notifications->addAction(label);
        a->setCheckable(true);
        a->setChecked(Notifications::saved() == content);
        notificationGroup->addAction(a);
        connect(a, &QAction::triggered, a, [content = content] { Notifications::save(content); });
    }
    m_typingAction = view->addAction(tr("Typing Indicators"));
    m_typingAction->setObjectName(QStringLiteral("typingIndicatorsAction"));
    m_typingAction->setCheckable(true);
    m_typingAction->setToolTip(tr("Show when others are typing, and let them see when you are"));
    m_typingAction->setChecked(QSettings().value(QStringLiteral("privacy/typingIndicators"), true).toBool());
    if (m_messenger)
        m_messenger->setTypingIndicators(m_typingAction->isChecked());
    m_typingAction->setEnabled(m_messenger != nullptr);
    connect(m_typingAction, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("privacy/typingIndicators"), on);
        if (m_messenger)
            m_messenger->setTypingIndicators(on);
    });
    QMenu *themes = view->addMenu(tr("Theme"));
    connect(themes, &QMenu::aboutToShow, this, [themes] {
        themes->clear();
        auto *group = new QActionGroup(themes);
        const QString current = savedThemeKey();
        for (const ThemeChoice &choice : allThemes()) {
            QAction *a = themes->addAction(choice.name);
            a->setCheckable(true);
            a->setChecked(choice.key == current);
            group->addAction(a);
            connect(a, &QAction::triggered, a, [key = choice.key] { setThemeKey(key); });
        }
    });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("About Argonaut Chat"), this, [this] {
        QMessageBox::about(this, tr("About Argonaut Chat"),
                           tr("<b>Argonaut Chat</b> %1<br>A private messenger in Qt and C++, from the makers of Argonaut.<br>"
                              "License: GPL-3.0-or-later")
                               .arg(QStringLiteral(CHAT_VERSION)));
    });
}

void MainWindow::reloadConversations()
{
    const QSignalBlocker block(m_conversations);
    m_conversations->clear();
    QListWidgetItem *current = nullptr;
    const QList<ChatStore::Conversation> all = m_store->conversations();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int archived = 0;
    qint64 nextUnmute = 0;
    for (const ChatStore::Conversation &c : all) {
        archived += c.archived ? 1 : 0;
        if (c.isMuted(now) && c.mutedUntil != ChatStore::MutedForever && (nextUnmute == 0 || c.mutedUntil < nextUnmute))
            nextUnmute = c.mutedUntil;
    }
    if (m_showArchived && archived == 0)
        m_showArchived = false; // nothing left in there
    m_archivedButton->setText(m_showArchived ? tr("←  Archived Chats") : tr("Archived Chats (%1)").arg(archived));
    m_archivedButton->setToolTip(m_showArchived ? tr("Back to your chats") : QString());
    m_archivedButton->setVisible(m_showArchived || archived > 0);
    // When the next mute runs out, the list shows it (a day at most, then again).
    if (nextUnmute != 0)
        m_muteTimer.start(int(qBound<qint64>(1000, nextUnmute - now + 500, 24 * 3600 * 1000LL)));
    else
        m_muteTimer.stop();
    for (const ChatStore::Conversation &c : all) {
        if (c.archived != m_showArchived)
            continue;
        const bool direct = c.kind == QLatin1String("direct");
        auto *item = new QListWidgetItem(direct && m_messenger ? m_messenger->displayName(c.peer) : c.title,
                                         m_conversations);
        item->setData(IdRole, c.id);
        item->setData(PeerRole, c.peer);
        // A request's picture is not even fetched: nothing a stranger sends shows before accepting.
        if (direct && !c.request) {
            const QPixmap picture = avatarFor(c.peer);
            if (!picture.isNull())
                item->setData(AvatarRole, picture);
        }
        item->setData(LastTextRole, c.lastDeleted ? tr("This message was deleted") : c.lastText);
        item->setData(TimeRole, c.lastText.isEmpty() ? QString() : shortTime(c.lastActivity));
        item->setData(UnreadRole, c.id == m_current || c.request ? 0 : c.unread);
        item->setData(RequestRole, c.request);
        item->setData(MutedRole, c.isMuted(now));
        item->setData(PinnedRole, c.pinned > 0);
        if (c.id == m_current)
            current = item;
    }
    if (current)
        m_conversations->setCurrentItem(current);
    applySearch();
}

void MainWindow::applySearch()
{
    const QString text = m_search ? m_search->text() : QString();
    for (int i = 0; i < m_conversations->count(); ++i) {
        QListWidgetItem *item = m_conversations->item(i);
        item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive)
                        && !item->data(PeerRole).toString().contains(text, Qt::CaseInsensitive));
    }
}

void MainWindow::showArchived(bool show)
{
    m_showArchived = show;
    reloadConversations();
}

void MainWindow::muteConversation(qint64 conversation, qint64 seconds)
{
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.id == 0 || c.request || c.kind == QLatin1String("self"))
        return;
    m_store->setMutedUntil(conversation, seconds < 0   ? ChatStore::MutedForever
                                         : seconds == 0 ? 0
                                                        : QDateTime::currentMSecsSinceEpoch() + seconds * 1000);
}

void MainWindow::archiveConversation(qint64 conversation, bool archived)
{
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.id == 0 || c.request)
        return;
    m_store->setArchived(conversation, archived);
}

bool MainWindow::pinConversation(qint64 conversation, bool pinned)
{
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.id == 0 || c.request)
        return false;
    if (!m_store->setPinned(conversation, pinned)) {
        showNotice(tr("You can pin up to %1 chats.").arg(ChatStore::MaxPinned));
        return false;
    }
    return true;
}

void MainWindow::fillMuteMenu(QMenu *menu, qint64 conversation)
{
    menu->clear();
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.isMuted()) {
        if (c.mutedUntil != ChatStore::MutedForever) {
            const QDateTime until = QDateTime::fromMSecsSinceEpoch(c.mutedUntil).toLocalTime();
            menu->addAction(tr("Muted until %1").arg(QLocale().toString(until, QLocale::ShortFormat)))->setEnabled(false);
        }
        menu->addAction(tr("Unmute"), this, [this, conversation] { muteConversation(conversation, 0); })
            ->setObjectName(QStringLiteral("unmuteAction"));
        return;
    }
    const QList<QPair<QString, qint64>> choices{
        {tr("For 1 Hour"), 3600}, {tr("For 8 Hours"), 8 * 3600}, {tr("For 1 Day"), 24 * 3600},
        {tr("For 1 Week"), 7 * 24 * 3600}, {tr("Always"), -1},
    };
    for (const auto &[label, seconds] : choices) {
        QAction *a = menu->addAction(label, this, [this, conversation, seconds = seconds] { muteConversation(conversation, seconds); });
        a->setData(seconds);
    }
}

void MainWindow::updateChatActions()
{
    const ChatStore::Conversation c = m_store->conversation(m_current);
    const bool can = c.id != 0 && !c.request;
    m_pinAction->setText(c.pinned > 0 ? tr("Unpin Chat") : tr("Pin Chat"));
    m_pinAction->setEnabled(can);
    m_archiveAction->setText(c.archived ? tr("Unarchive Chat") : tr("Archive Chat"));
    m_archiveAction->setEnabled(can);
    m_muteMenu->setEnabled(can && c.kind != QLatin1String("self"));
    fillMuteMenu(m_muteMenu, m_current);
}

QMenu *MainWindow::conversationMenu(qint64 conversation)
{
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.id == 0 || c.request)
        return nullptr; // a request: answered first, in its bar
    auto *menu = new QMenu(this);
    menu->addAction(c.pinned > 0 ? tr("Unpin Chat") : tr("Pin Chat"), this,
                    [this, conversation, pin = c.pinned == 0] { pinConversation(conversation, pin); })
        ->setObjectName(QStringLiteral("pinAction"));
    if (c.kind != QLatin1String("self")) {
        QMenu *mute = menu->addMenu(tr("Mute Notifications"));
        mute->setObjectName(QStringLiteral("muteMenu"));
        fillMuteMenu(mute, conversation);
    }
    menu->addAction(c.archived ? tr("Unarchive Chat") : tr("Archive Chat"), this,
                    [this, conversation, archive = !c.archived] { archiveConversation(conversation, archive); })
        ->setObjectName(QStringLiteral("archiveAction"));
    return menu;
}

void MainWindow::openConversation(qint64 id)
{
    if (id != m_current) {
        cancelReply(); // a reply belongs to its conversation
        m_messageLimit = DefaultMessageLimit;
        m_flash = 0;
        m_flashTimer.stop();
        m_pending.clear(); // they were for that conversation
        refreshAttachBar();
        m_unreadMarker = 0;
        m_markerFresh = false;
        m_stickToBottom = true;
        m_newestSeen = 0;
        stopRecording(false); // a recording belongs to its conversation
        m_player.stop();
        if (m_findBar && m_findBar->isVisible()) {
            m_findResults.clear();
            m_findIndex = -1;
            m_findBar->hide();
        }
        if (m_messenger && m_current != 0)
            m_messenger->setTyping(m_current, false);
    }
    m_current = id;
    // The list shows which one is open, however it was opened.
    {
        const QSignalBlocker block(m_conversations);
        for (int i = 0; i < m_conversations->count(); ++i) {
            if (m_conversations->item(i)->data(IdRole).toLongLong() == id)
                m_conversations->setCurrentRow(i);
        }
    }
    updateHeader();
    reloadMessages(); // before reading them: "New messages" goes above the first unread
    markCurrentRead();
    m_compose->setFocus();
    updateSendButton();
}

void MainWindow::markCurrentRead()
{
    if (m_current == 0)
        return;
    if (m_messenger)
        m_messenger->markRead(m_current);
    else
        m_store->markRead(m_current);
}

void MainWindow::setDisappearing(qint64 seconds)
{
    if (!m_messenger || m_current == 0)
        return;
    m_messenger->setDisappearing(m_current, seconds);
    updateHeader();
}

void MainWindow::updateHeader()
{
    const ChatStore::Conversation c = m_store->conversation(m_current);
    const bool direct = c.kind == QLatin1String("direct");
    m_title->setText(direct && m_messenger ? m_messenger->displayName(c.peer) : c.title);
    const bool isGroup = c.kind == QLatin1String("group");
    if (c.kind == QLatin1String("self"))
        m_subtitle->setText(tr("Notes you keep for yourself, on this computer"));
    else if (isGroup && c.left)
        m_subtitle->setText(tr("You are no longer in this group"));
    else if (isGroup && m_messenger) {
        const ChatStore::Group g = m_messenger->group(c.id);
        QStringList names;
        for (const QString &member : g.members)
            names << (member == m_messenger->username() ? tr("you") : m_messenger->displayName(member));
        m_subtitle->setText(c.request ? tr("Group request from @%1").arg(g.admin)
                                      : (g.members.size() == 1 ? tr("1 member: %1")
                                                               : tr("%1 members: %2").arg(g.members.size()))
                                            .arg(names.join(QStringLiteral(", "))));
    }
    else if (direct && c.request)
        m_subtitle->setText(tr("@%1 · message request").arg(c.peer));
    else if (direct && m_messenger && m_messenger->safetyNumberHasChanged(c.peer))
        m_subtitle->setText(tr("⚠ Your safety number with %1 has changed: check it").arg(c.peer));
    else if (direct && m_messenger && m_messenger->isVerified(c.peer))
        m_subtitle->setText(tr("@%1 · ✓ verified").arg(c.peer));
    else
        m_subtitle->setText(direct ? QStringLiteral("@") + c.peer : c.peer);
    if (c.expireSeconds > 0)
        m_subtitle->setText(m_subtitle->text() + QStringLiteral(" · ⏱ ") + Messenger::describeTimer(c.expireSeconds));
    if (const QString typing = typingText(c.id); !typing.isEmpty())
        m_subtitle->setText(typing);
    if (m_timerMenu) {
        m_timerMenu->setEnabled(m_messenger && c.id != 0 && !c.left && !c.request);
        for (QAction *a : m_timerMenu->actions())
            a->setChecked(a->data().toLongLong() == c.expireSeconds);
    }
    m_safety->setVisible(direct && m_messenger);
    if (m_safetyAction)
        m_safetyAction->setEnabled(direct && m_messenger);
    if (m_membersAction)
        m_membersAction->setEnabled(isGroup && m_messenger && !c.left);
    if (m_leaveAction)
        m_leaveAction->setEnabled(isGroup && m_messenger && !c.left);
    // A request: answer it before writing. A group I left: read only.
    const bool request = (direct || isGroup) && c.request && m_messenger;
    m_composer->setVisible(!request && !c.left);
    m_requestBar->setVisible(request);
    m_leftNote->setVisible(c.left);
    if (request && isGroup) {
        const QString admin = m_messenger->group(c.id).admin;
        m_requestText->setText(tr("<b>%1</b> (@%2) added you to <b>%3</b>. They won't know you have seen its "
                                  "messages until you accept.")
                                   .arg(m_messenger->displayName(admin).toHtmlEscaped(), admin.toHtmlEscaped(),
                                        c.title.toHtmlEscaped()));
    } else if (request) {
        m_requestText->setText(tr("<b>%1</b> wants to chat with you. They won't know you have seen their messages "
                                  "until you accept.")
                                   .arg(c.peer.toHtmlEscaped()));
    }
}

void MainWindow::acceptRequest()
{
    if (!m_messenger || m_current == 0)
        return;
    m_messenger->acceptRequest(m_current);
    updateHeader();
    m_compose->setFocus();
}

void MainWindow::deleteRequest(bool ask)
{
    const ChatStore::Conversation c = m_store->conversation(m_current);
    if (!m_messenger || (c.kind != QLatin1String("direct") && c.kind != QLatin1String("group")))
        return;
    if (ask && QMessageBox::question(this, tr("Delete"),
                                     tr("Delete this conversation with %1? They can write to you again.").arg(c.peer))
                   != QMessageBox::Yes)
        return;
    m_messenger->deleteConversation(c.id);
    openConversation(m_store->noteToSelf());
}

void MainWindow::blockRequest(bool ask)
{
    const ChatStore::Conversation c = m_store->conversation(m_current);
    if (!m_messenger || (c.kind != QLatin1String("direct") && c.kind != QLatin1String("group")))
        return;
    // A group request: its admin is the one blocked, and the group goes.
    const QString who = c.kind == QLatin1String("group") ? m_messenger->group(c.id).admin : c.peer;
    if (ask && QMessageBox::question(this, tr("Block"),
                                     tr("Block %1? Their messages will be dropped, and they will not know. "
                                        "You can unblock them in File › Blocked People.")
                                         .arg(who))
                   != QMessageBox::Yes)
        return;
    m_messenger->block(who);
    if (c.kind == QLatin1String("group"))
        m_messenger->deleteConversation(c.id);
    openConversation(m_store->noteToSelf());
}

void MainWindow::leaveGroup(bool ask)
{
    const ChatStore::Conversation c = m_store->conversation(m_current);
    if (!m_messenger || c.kind != QLatin1String("group") || c.left)
        return;
    if (ask && QMessageBox::question(this, tr("Leave Group"),
                                     tr("Leave %1? You will no longer get its messages.").arg(c.title))
                   != QMessageBox::Yes)
        return;
    m_messenger->leaveGroup(c.id);
    updateHeader();
}

void MainWindow::updateConnection()
{
    const bool account = m_messenger && m_messenger->hasAccount();
    QString text;
    if (!m_messenger || !account) {
        text = tr("On this computer only: sign up to chat");
    } else {
        switch (m_messenger->state()) {
        case Messenger::State::Online:
            text = tr("@%1 · connected").arg(m_messenger->username());
            break;
        case Messenger::State::Connecting:
            text = tr("@%1 · connecting…").arg(m_messenger->username());
            break;
        default:
            text = tr("@%1 · offline: messages wait until the connection is back").arg(m_messenger->username());
            break;
        }
    }
    m_connection->setText(text);
    if (m_signUpAction)
        m_signUpAction->setVisible(m_messenger && !account);
    if (m_newChatAction)
        m_newChatAction->setEnabled(account);
    if (m_blockedAction)
        m_blockedAction->setEnabled(account);
    if (m_profileAction)
        m_profileAction->setEnabled(account);
    if (m_newGroupAction)
        m_newGroupAction->setEnabled(account);
    if (m_devicesAction)
        m_devicesAction->setEnabled(account);
}

void MainWindow::showNotice(const QString &text)
{
    m_notice->setText(text);
    m_notice->show();
    QTimer::singleShot(10000, m_notice, [notice = m_notice, text] {
        if (notice->text() == text)
            notice->hide();
    });
}

void MainWindow::showSignUp()
{
    if (!m_messenger || m_messenger->hasAccount())
        return;
    SignUpDialog dialog(m_messenger, this);
    if (dialog.exec() == QDialog::Accepted)
        updateConnection();
}

void MainWindow::newChat()
{
    if (!m_messenger || !m_messenger->hasAccount())
        return;
    bool ok = false;
    const QString username =
        QInputDialog::getText(this, tr("New Chat"), tr("Their username:"), QLineEdit::Normal, QString(), &ok);
    if (ok && !username.trimmed().isEmpty())
        startChatWith(username);
}

void MainWindow::startChatWith(const QString &username)
{
    if (!m_messenger)
        return;
    QPointer<MainWindow> self(this);
    m_messenger->startConversation(username, [self](qint64 conversation, const QString &error) {
        if (!self)
            return;
        if (conversation == 0) {
            self->showNotice(error);
            return;
        }
        self->reloadConversations();
        for (int i = 0; i < self->m_conversations->count(); ++i) {
            if (self->m_conversations->item(i)->data(IdRole).toLongLong() == conversation)
                self->m_conversations->setCurrentRow(i);
        }
        self->openConversation(conversation);
    });
}

void MainWindow::showSafetyNumber()
{
    const ChatStore::Conversation c = m_store->conversation(m_current);
    if (!m_messenger || c.kind != QLatin1String("direct"))
        return;
    SafetyNumberDialog dialog(m_messenger, c.peer, this);
    dialog.exec();
    updateHeader();
}

void MainWindow::reloadMessages()
{
    // What was chosen stays chosen (a message arriving rebuilds the list).
    QSet<qint64> chosen;
    for (const QListWidgetItem *item : m_messages->selectedItems())
        chosen.insert(item->data(IdRole).toLongLong());
    QListWidgetItem *flashItem = nullptr;
    // Where the reader is: at the newest, or further up at some message.
    QScrollBar *bar = m_messages->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;
    qint64 anchor = 0;
    int anchorTop = 0;
    if (QListWidgetItem *top = m_messages->itemAt(QPoint(m_messages->viewport()->width() / 2, 1))) {
        anchor = top->data(IdRole).toLongLong();
        anchorTop = m_messages->visualItemRect(top).top();
    }
    const bool stick = std::exchange(m_stickToBottom, false) || atBottom;
    if (m_unreadMarker == 0) {
        m_unreadMarker = m_store->firstUnseen(m_current);
        m_markerFresh = m_unreadMarker != 0;
    }
    QListWidgetItem *divider = nullptr;
    m_messages->clear();
    const bool group = m_store->conversation(m_current).kind == QLatin1String("group");
    const qint64 match = currentFindMatch();
    QListWidgetItem *matchItem = nullptr;
    for (const ChatStore::Message &m : m_store->messages(m_current, m_messageLimit)) {
        auto *item = new QListWidgetItem(m.deleted ? tr("This message was deleted") : m.body, m_messages);
        if (m.id == m_unreadMarker) {
            item->setData(DividerRole, true);
            divider = item;
        }
        item->setData(IdRole, m.id);
        item->setData(DeletedRole, m.deleted);
        item->setData(EditedRole, m.editedAt > 0 && !m.deleted);
        if (m.id == match) {
            item->setData(HighlightRole, true);
            matchItem = item;
        }
        if (m.id == m_flash) {
            item->setData(HighlightRole, true);
            flashItem = item;
        }
        if (chosen.contains(m.id))
            item->setSelected(true);
        item->setData(OutgoingRole, m.outgoing());
        item->setData(TimeRole, QLocale().toString(m.sentAt.toLocalTime().time(), QLocale::ShortFormat));
        item->setData(StatusRole, m.status);
        item->setData(SystemRole, m.system);
        item->setData(ExpireRole, m.expireSeconds > 0);
        if (group && !m.outgoing())
            item->setData(AuthorRole, m_messenger ? m_messenger->displayName(m.author) : m.author);
        if (m.isReply()) {
            item->setData(QuoteAuthorRole, quoteAuthorName(m.quoteAuthor));
            // Emptied when what it answered was deleted for everyone.
            item->setData(QuoteTextRole, m.quoteText.isEmpty() ? tr("Deleted message") : m.quoteText);
        }
        const QList<ChatStore::StoredAttachment> attachments = m_store->attachments(m.id);
        if (!attachments.isEmpty()) {
            const ChatStore::StoredAttachment &a = attachments.first();
            item->setData(AttachmentRole, a.id);
            if (a.voice) {
                item->setData(VoiceRole, true);
                item->setData(FileRole, voiceLine(a));
                item->setToolTip(a.file.isEmpty() ? QString() : tr("Click to play"));
            } else {
                const QPixmap preview = previewFor(a);
                if (!preview.isNull())
                    item->setData(ImageRole, preview);
                item->setData(FileRole, fileLine(a, !preview.isNull()));
                item->setToolTip(a.file.isEmpty() ? QString() : tr("Click to save %1").arg(a.name));
            }
        }
        const QList<ChatStore::Reaction> reactions = m_store->reactions(m.id);
        if (!reactions.isEmpty()) {
            // Each emoji once, in the order they came, with how many; who, in the tooltip.
            QStringList order;
            QHash<QString, QStringList> who;
            for (const ChatStore::Reaction &r : reactions) {
                if (!who.contains(r.emoji))
                    order << r.emoji;
                who[r.emoji] << (r.author.isEmpty() ? tr("You") : m_messenger ? m_messenger->displayName(r.author) : r.author);
            }
            QStringList shown;
            QStringList lines;
            for (const QString &emoji : order) {
                const int n = who.value(emoji).size();
                shown << (n > 1 ? QStringLiteral("%1 %2").arg(emoji).arg(n) : emoji);
                lines << QStringLiteral("%1 %2").arg(emoji, who.value(emoji).join(QStringLiteral(", ")));
            }
            item->setData(ReactionsRole, shown.join(QStringLiteral("  ")));
            const QString tip = lines.join(QLatin1Char('\n'));
            item->setToolTip(item->toolTip().isEmpty() ? tip : item->toolTip() + QLatin1Char('\n') + tip);
        }
    }
    // Just opened: what is there now is not "new" for ↓.
    if (m_newestSeen == 0) {
        for (int i = 0; i < m_messages->count(); ++i)
            m_newestSeen = qMax(m_newestSeen, m_messages->item(i)->data(IdRole).toLongLong());
    }
    // Gone to a quoted one, or searching: it stays in view. Just opened with
    // unread ones: "New messages" at the top. Reading further up: the same
    // place. Otherwise the newest.
    m_messages->doItemsLayout(); // the heights first, or it scrolls to the wrong place
    QListWidgetItem *kept = stick ? nullptr : messageItem(anchor);
    if (flashItem || matchItem) {
        m_messages->scrollToItem(flashItem ? flashItem : matchItem, QAbstractItemView::PositionAtCenter);
    } else if (m_markerFresh && divider) {
        m_markerFresh = false;
        m_messages->scrollToItem(divider, QAbstractItemView::PositionAtTop);
    } else if (kept) {
        m_messages->scrollToItem(kept, QAbstractItemView::PositionAtTop);
        bar->setValue(bar->value() - anchorTop);
    } else {
        m_messages->scrollToBottom();
    }
    updateToBottom();
}

void MainWindow::scrollToNewest()
{
    m_messages->scrollToBottom();
    updateToBottom();
}

void MainWindow::updateToBottom()
{
    const QScrollBar *bar = m_messages->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 40;
    int below = 0;
    if (atBottom) {
        for (int i = 0; i < m_messages->count(); ++i)
            m_newestSeen = qMax(m_newestSeen, m_messages->item(i)->data(IdRole).toLongLong());
    } else {
        // Theirs that came since the reader was last at the newest.
        for (int i = m_messages->count() - 1; i >= 0; --i) {
            const QListWidgetItem *item = m_messages->item(i);
            const qint64 id = item->data(IdRole).toLongLong();
            if (id != 0 && id > m_newestSeen && !item->data(OutgoingRole).toBool() && !item->data(SystemRole).toBool())
                ++below;
        }
    }
    m_toBottom->setText(below > 0 ? QStringLiteral("↓ %1").arg(below) : QStringLiteral("↓"));
    m_toBottom->setVisible(!atBottom && m_messages->count() > 0);
    placeToBottom();
}

void MainWindow::placeToBottom()
{
    m_toBottom->adjustSize();
    const QSize size(qMax(40, m_toBottom->sizeHint().width() + 12), 40);
    m_toBottom->resize(size);
    const QRect area = m_messages->viewport()->geometry();
    m_toBottom->move(area.right() - size.width() - 16, area.bottom() - size.height() - 12);
    m_toBottom->raise();
}

void MainWindow::aboutToSend()
{
    // Sending: what they wrote has been seen; the line goes and the newest
    // shows, as Signal does.
    markCurrentRead();
    m_unreadMarker = 0;
    m_markerFresh = false;
    m_stickToBottom = true;
}

QListWidgetItem *MainWindow::messageItem(qint64 message) const
{
    for (int i = 0; message != 0 && i < m_messages->count(); ++i) {
        if (m_messages->item(i)->data(IdRole).toLongLong() == message)
            return m_messages->item(i);
    }
    return nullptr;
}

bool MainWindow::jumpToQuote(qint64 reply)
{
    const ChatStore::Message m = m_store->message(reply);
    const qint64 target = m.conversation == m_current && m.isReply() ? m_store->messageByUid(m_current, m.quoteUid) : 0;
    if (target == 0 || m_store->message(target).deleted) {
        showNotice(tr("The message this answers is no longer here."));
        return false;
    }
    if (!messageItem(target))
        m_messageLimit = qMax(m_messageLimit, m_store->messageCount(m_current)); // older than the list holds
    m_flash = target;
    reloadMessages();
    m_flashTimer.start();
    return true;
}

QString MainWindow::copyText(const ChatStore::Message &m) const
{
    if (m.deleted || m.system)
        return {};
    const QList<ChatStore::StoredAttachment> files = m_store->attachments(m.id);
    QString file;
    if (!files.isEmpty())
        file = files.first().voice ? tr("🎤 Voice message") : QStringLiteral("📎 ") + files.first().name;
    if (m.body.isEmpty())
        return file;
    return file.isEmpty() ? m.body : file + QLatin1Char('\n') + m.body;
}

QString MainWindow::selectedMessagesText() const
{
    QList<QListWidgetItem *> items = m_messages->selectedItems();
    std::sort(items.begin(), items.end(),
              [this](QListWidgetItem *a, QListWidgetItem *b) { return m_messages->row(a) < m_messages->row(b); });
    QList<ChatStore::Message> chosen;
    for (const QListWidgetItem *item : items) {
        const ChatStore::Message m = m_store->message(item->data(IdRole).toLongLong());
        if (!copyText(m).isEmpty())
            chosen << m;
    }
    if (chosen.size() == 1)
        return copyText(chosen.first());
    QStringList lines;
    for (const ChatStore::Message &m : chosen) {
        const QString who = m.outgoing() ? tr("You") : m_messenger ? m_messenger->displayName(m.author) : m.author;
        lines << QStringLiteral("[%1] %2: %3")
                     .arg(QLocale().toString(m.sentAt.toLocalTime(), QLocale::ShortFormat), who, copyText(m));
    }
    return lines.join(QLatin1Char('\n'));
}

void MainWindow::copySelectedMessages()
{
    const QString text = selectedMessagesText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void MainWindow::showFind()
{
    if (m_current == 0)
        return;
    m_findBar->show();
    m_findField->setFocus();
    m_findField->selectAll();
    runFind();
}

void MainWindow::closeFind()
{
    const bool had = currentFindMatch() != 0;
    m_findResults.clear();
    m_findIndex = -1;
    m_findBar->hide();
    if (m_findField->hasFocus())
        m_compose->setFocus();
    if (had)
        reloadMessages();
}

qint64 MainWindow::currentFindMatch() const
{
    if (!m_findBar || m_findBar->isHidden() || m_findIndex < 0 || m_findIndex >= m_findResults.size())
        return 0;
    return m_findResults.at(m_findIndex);
}

void MainWindow::runFind()
{
    m_findResults = m_store->findMessages(m_current, m_findField->text());
    m_findIndex = int(m_findResults.size()) - 1; // the newest first, as you read upwards
    showFindMatch();
}

void MainWindow::findStep(int delta)
{
    if (m_findResults.isEmpty())
        return;
    const int n = int(m_findResults.size());
    m_findIndex = ((m_findIndex + delta) % n + n) % n;
    showFindMatch();
}

void MainWindow::showFindMatch()
{
    const int n = int(m_findResults.size());
    if (m_findField->text().simplified().isEmpty())
        m_findCount->clear();
    else if (n == 0)
        m_findCount->setText(tr("No results"));
    else
        m_findCount->setText(tr("%1 of %2").arg(m_findIndex + 1).arg(n));
    // An older one than the list holds: the list grows to it.
    const qint64 match = currentFindMatch();
    if (match != 0 && m_store->message(match).sentAt.isValid()) {
        bool shown = false;
        for (int i = 0; i < m_messages->count() && !shown; ++i)
            shown = m_messages->item(i)->data(IdRole).toLongLong() == match;
        if (!shown)
            m_messageLimit = qMax(m_messageLimit, m_store->messageCount(m_current));
    }
    reloadMessages();
}

QPixmap MainWindow::avatarFor(const QString &username)
{
    if (!m_messenger)
        return {};
    if (const auto cached = m_avatars.constFind(username); cached != m_avatars.constEnd())
        return *cached;
    QPixmap picture;
    QImage image;
    const QByteArray data = m_messenger->avatar(username);
    if (!data.isEmpty() && image.loadFromData(data))
        picture = QPixmap::fromImage(image.scaled(80, 80, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    m_avatars.insert(username, picture);
    return picture;
}

QPixmap MainWindow::previewFor(const ChatStore::StoredAttachment &a)
{
    if (!m_messenger || !a.type.startsWith(QLatin1String("image/")) || a.file.isEmpty())
        return {};
    if (const auto cached = m_previews.constFind(a.id); cached != m_previews.constEnd())
        return *cached;
    QPixmap preview;
    // Decrypted in memory only.
    if (const auto data = m_messenger->attachmentData(a.id)) {
        QImage image;
        if (image.loadFromData(*data))
            preview = QPixmap::fromImage(image.scaled(QSize(260, 260).boundedTo(image.size()), Qt::KeepAspectRatio,
                                                      Qt::SmoothTransformation));
    }
    m_previews.insert(a.id, preview);
    return preview;
}

QString MainWindow::voiceLine(const ChatStore::StoredAttachment &a)
{
    if (a.state == QLatin1String("upload") || a.state == QLatin1String("download") || a.state == QLatin1String("failed")) {
        QString line = fileLine(a, false);
        line.replace(QStringLiteral("📄 ") + a.name, tr("🎤 Voice message"));
        return line;
    }
    if (!m_voiceDurations.contains(a.id)) {
        // Its length, once: decrypted in memory and checked.
        qint64 ms = -1;
        if (m_messenger) {
            if (const auto data = m_messenger->attachmentData(a.id)) {
                if (const auto audio = Voice::fromWav(*data))
                    ms = audio->durationMs();
            }
        }
        m_voiceDurations.insert(a.id, ms);
    }
    const qint64 ms = m_voiceDurations.value(a.id);
    if (ms < 0)
        return tr("🎤 Voice message · cannot be played");
    if (m_player.playing() == a.id)
        return QStringLiteral("⏸  %1 / %2").arg(Voice::formatDuration(m_player.positionMs()), Voice::formatDuration(ms));
    return tr("▶  Voice message · %1").arg(Voice::formatDuration(ms));
}

void MainWindow::refreshVoiceLines()
{
    for (int i = 0; i < m_messages->count(); ++i) {
        QListWidgetItem *item = m_messages->item(i);
        if (item->data(VoiceRole).toBool())
            item->setData(FileRole, voiceLine(m_store->attachment(item->data(AttachmentRole).toLongLong())));
    }
}

void MainWindow::toggleVoice(qint64 attachment)
{
    if (m_player.playing() == attachment) {
        m_player.stop();
        return;
    }
    const auto data = m_messenger ? m_messenger->attachmentData(attachment) : std::nullopt;
    QString error;
    if (!data || !m_player.play(attachment, *data, &error))
        showNotice(error.isEmpty() ? tr("This voice message is not here yet.") : error);
    refreshVoiceLines();
}

void MainWindow::startRecording()
{
    if (!m_messenger || m_current == 0 || m_recorder.isRecording())
        return;
    QString error;
    if (!m_recorder.start(&error)) {
        showNotice(error);
        return;
    }
    m_player.stop();
    m_recordLabel->setText(tr("● Recording %1").arg(Voice::formatDuration(0)));
    m_recordBar->show();
    m_compose->setEnabled(false);
    m_voiceButton->setEnabled(false);
}

void MainWindow::stopRecording(bool send)
{
    if (!m_recorder.isRecording())
        return;
    const QByteArray samples = send ? m_recorder.stop() : QByteArray();
    m_recorder.cancel();
    m_recordBar->hide();
    m_compose->setEnabled(true);
    m_voiceButton->setEnabled(m_messenger != nullptr);
    m_compose->setFocus();
    if (send)
        sendVoice(samples);
}

void MainWindow::sendVoice(const QByteArray &samples)
{
    if (!m_messenger || m_current == 0)
        return;
    QString error;
    aboutToSend();
    if (m_messenger->sendVoice(m_current, samples, &error, m_replyTo) == 0) {
        showNotice(error);
        return;
    }
    cancelReply();
}

QString MainWindow::fileLine(const ChatStore::StoredAttachment &a, bool shownAsImage) const
{
    const QString name = QStringLiteral("📄 %1 · %2").arg(a.name, QLocale().formattedDataSize(a.size));
    const int percent = m_progress.value(a.id, -1);
    if (a.state == QLatin1String("upload"))
        return name + (percent >= 0 ? tr(" · Uploading %1%").arg(percent) : tr(" · Waiting to upload"));
    if (a.state == QLatin1String("download")) {
        if (m_store->conversation(m_current).request)
            return name + tr(" · Accept to download");
        return name + (percent >= 0 ? tr(" · Downloading %1%").arg(percent) : tr(" · Waiting to download"));
    }
    if (a.state == QLatin1String("failed"))
        return name + tr(" · Not available");
    return shownAsImage ? QString() : name;
}

void MainWindow::attachFile(const QString &path)
{
    if (!m_messenger || m_current == 0)
        return;
    if (m_editing != 0) {
        m_compose->clear();
        cancelReply(); // a file is a new message, not an edit
    }
    QString error;
    aboutToSend();
    if (m_messenger->sendFile(m_current, path, m_compose->toPlainText().trimmed(), &error, m_replyTo) == 0) {
        showNotice(error);
        return;
    }
    m_compose->clear();
    cancelReply();
}

bool MainWindow::canAttach() const
{
    return m_messenger && m_current != 0 && m_composer->isVisible() && m_compose->isEnabled() && !m_recorder.isRecording();
}

bool MainWindow::addAttachment(const QString &path)
{
    if (!canAttach())
        return false;
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) {
        showNotice(tr("%1 is a folder or cannot be read.").arg(info.fileName()));
        return false;
    }
    if (info.size() > Attachment::MaxFileBytes) {
        showNotice(tr("%1 is larger than %2 MB.").arg(info.fileName()).arg(Attachment::MaxFileBytes / (1024 * 1024)));
        return false;
    }
    if (m_pending.size() >= MaxPendingFiles) {
        showNotice(tr("At most %1 files at a time.").arg(MaxPendingFiles));
        return false;
    }
    if (m_editing != 0) {
        m_compose->clear();
        cancelReply(); // files are a new message, not an edit
    }
    PendingFile f;
    f.path = path;
    f.name = info.fileName();
    f.size = info.size();
    QImageReader reader(path);
    if (reader.canRead() && reader.size().isValid()) {
        reader.setScaledSize(reader.size().scaled(48, 48, Qt::KeepAspectRatio));
        f.thumbnail = QPixmap::fromImage(reader.read());
    }
    m_pending << f;
    refreshAttachBar();
    return true;
}

bool MainWindow::addAttachmentData(const QString &name, const QByteArray &data)
{
    if (!canAttach())
        return false;
    if (data.size() > Attachment::MaxFileBytes) {
        showNotice(tr("%1 is larger than %2 MB.").arg(name).arg(Attachment::MaxFileBytes / (1024 * 1024)));
        return false;
    }
    if (m_pending.size() >= MaxPendingFiles) {
        showNotice(tr("At most %1 files at a time.").arg(MaxPendingFiles));
        return false;
    }
    if (m_editing != 0) {
        m_compose->clear();
        cancelReply();
    }
    PendingFile f;
    f.name = name;
    f.data = data;
    f.size = data.size();
    QImage image;
    if (image.loadFromData(data))
        f.thumbnail = QPixmap::fromImage(image.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_pending << f;
    refreshAttachBar();
    return true;
}

bool MainWindow::takeAttachments(const QMimeData *data)
{
    if (!canAttach() || !carriesAttachments(data))
        return false;
    const QStringList files = localFiles(data);
    if (!files.isEmpty()) {
        for (const QString &path : files) {
            if (!addAttachment(path))
                break;
        }
        return true;
    }
    // A picture: as PNG (whatever it carried along, nothing but the pixels goes).
    const QImage image = qvariant_cast<QImage>(data->imageData());
    QByteArray png;
    QBuffer buffer(&png);
    if (image.isNull() || !buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return false;
    const QString name = tr("Pasted image %1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss")));
    addAttachmentData(name, png);
    return true;
}

void MainWindow::removeAttachment(int index)
{
    if (index < 0 || index >= m_pending.size())
        return;
    m_pending.removeAt(index);
    refreshAttachBar();
}

void MainWindow::refreshAttachBar()
{
    auto *layout = static_cast<QHBoxLayout *>(m_attachBar->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater(); // its ✕ may be the one being clicked
        }
        delete item;
    }
    for (int i = 0; i < m_pending.size(); ++i) {
        const PendingFile &f = m_pending.at(i);
        auto *chip = new QWidget(m_attachBar);
        chip->setObjectName(QStringLiteral("attachChip"));
        chip->setAttribute(Qt::WA_StyledBackground);
        chip->setToolTip(QStringLiteral("%1\n%2").arg(f.name, QLocale().formattedDataSize(f.size)));
        auto *chipLayout = new QHBoxLayout(chip);
        chipLayout->setContentsMargins(6, 4, 2, 4);
        chipLayout->setSpacing(4);
        auto *icon = new QLabel(chip);
        if (!f.thumbnail.isNull())
            icon->setPixmap(f.thumbnail);
        else
            icon->setText(QStringLiteral("📎"));
        chipLayout->addWidget(icon);
        if (f.thumbnail.isNull()) {
            auto *name = new QLabel(chip);
            name->setTextFormat(Qt::PlainText);
            name->setText(name->fontMetrics().elidedText(f.name, Qt::ElideMiddle, 160));
            chipLayout->addWidget(name);
        }
        auto *remove = new QToolButton(chip);
        remove->setObjectName(QStringLiteral("removeAttachmentButton"));
        remove->setText(QStringLiteral("✕"));
        remove->setToolTip(tr("Don't send this one"));
        remove->setAutoRaise(true);
        connect(remove, &QToolButton::clicked, this, [this, i] { removeAttachment(i); });
        chipLayout->addWidget(remove);
        layout->addWidget(chip);
    }
    layout->addStretch(1);
    m_attachBar->setVisible(!m_pending.isEmpty());
    updateSendButton();
}

void MainWindow::updateSendButton()
{
    m_send->setEnabled(m_current != 0 && (!m_compose->toPlainText().trimmed().isEmpty() || !m_pending.isEmpty()));
}

QString MainWindow::typingText(qint64 conversation) const
{
    if (!m_messenger)
        return {};
    const QStringList who = m_messenger->typingIn(conversation);
    if (who.isEmpty())
        return {};
    if (m_store->conversation(conversation).kind != QLatin1String("group"))
        return tr("typing…");
    if (who.size() == 1)
        return tr("%1 is typing…").arg(m_messenger->displayName(who.first()));
    if (who.size() == 2)
        return tr("%1 and %2 are typing…").arg(m_messenger->displayName(who.at(0)), m_messenger->displayName(who.at(1)));
    return tr("%1 people are typing…").arg(who.size());
}

QString MainWindow::quoteAuthorName(const QString &author) const
{
    const bool self = m_store->conversation(m_current).kind == QLatin1String("self");
    if (self || (m_messenger && author == m_messenger->username()) || author == QLatin1String("me"))
        return tr("You");
    return m_messenger ? m_messenger->displayName(author) : author;
}

QMenu *MainWindow::messageMenu(qint64 message)
{
    const ChatStore::Message m = m_store->message(message);
    const ChatStore::Conversation c = m_store->conversation(m.conversation);
    if (m.id == 0 || m.system)
        return nullptr;
    auto *menu = new QMenu(this);
    if (m.deleted) {
        // Only the mark is left: it can go from here.
        menu->addAction(tr("Delete for Me…"), this, [this, message] { deleteMessage(message, false, true); })
            ->setObjectName(QStringLiteral("deleteForMeAction"));
        return menu;
    }
    QAction *reply = menu->addAction(tr("Reply"), this, [this, message] { replyTo(message); });
    reply->setObjectName(QStringLiteral("replyAction"));
    // Not where nothing can be written: a request, a group I left.
    reply->setEnabled(m_messenger && !c.request && !c.left);
    if (m_messenger && m_messenger->canEdit(message)) {
        menu->addAction(tr("Edit"), this, [this, message] { startEdit(message); })
            ->setObjectName(QStringLiteral("editAction"));
    }
    // React: the quick ones, mine ticked (again: taken back).
    QMenu *react = menu->addMenu(tr("React"));
    react->setObjectName(QStringLiteral("reactMenu"));
    const bool canReact = m_messenger && !c.request && !c.left && (c.kind == QLatin1String("self") || !m.uid.isEmpty());
    react->setEnabled(canReact);
    const QString mine = m_store->myReaction(message);
    for (const QString &emoji : Messenger::quickReactions()) {
        QAction *a = react->addAction(emoji, this, [this, message, emoji, mine] {
            m_messenger->react(message, emoji == mine ? QString() : emoji);
        });
        a->setCheckable(true);
        a->setChecked(emoji == mine);
        a->setData(emoji);
    }
    if (!mine.isEmpty()) {
        QAction *remove = menu->addAction(tr("Remove Reaction"), this, [this, message] { m_messenger->react(message, QString()); });
        remove->setObjectName(QStringLiteral("removeReactionAction"));
        remove->setEnabled(canReact);
    }
    if (!m.body.isEmpty()) {
        menu->addAction(tr("Copy Text"), this, [body = m.body] { QGuiApplication::clipboard()->setText(body); });
    }
    const QList<ChatStore::StoredAttachment> files = m_store->attachments(message);
    if (!files.isEmpty() && !files.first().file.isEmpty()) {
        const ChatStore::StoredAttachment a = files.first();
        menu->addAction(a.voice ? tr("Save Voice Message…") : tr("Save As…"), this, [this, a] {
            const QString folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            const QString path = QFileDialog::getSaveFileName(this, tr("Save"), folder + QLatin1Char('/') + a.name);
            if (!path.isEmpty())
                saveAttachment(a.id, path);
        })->setObjectName(QStringLiteral("saveAttachmentAction"));
    }
    menu->addSeparator();
    menu->addAction(tr("Delete for Me…"), this, [this, message] { deleteMessage(message, false, true); })
        ->setObjectName(QStringLiteral("deleteForMeAction"));
    if (m_messenger && m_messenger->canDeleteForEveryone(message)) {
        menu->addAction(tr("Delete for Everyone…"), this, [this, message] { deleteMessage(message, true, true); })
            ->setObjectName(QStringLiteral("deleteForEveryoneAction"));
    }
    return menu;
}

void MainWindow::deleteMessage(qint64 message, bool forEveryone, bool ask)
{
    if (ask) {
        const QString question = forEveryone
            ? tr("Delete this message for everyone in the conversation? They will see that a message was deleted.")
            : tr("Delete this message from this computer? The others keep their copy.");
        if (QMessageBox::question(this, forEveryone ? tr("Delete for Everyone") : tr("Delete for Me"), question)
            != QMessageBox::Yes)
            return;
    }
    if (m_replyTo == message)
        cancelReply();
    if (forEveryone) {
        if (!m_messenger || !m_messenger->deleteForEveryone(message))
            showNotice(tr("This message can no longer be deleted for everyone."));
    } else if (m_messenger) {
        m_messenger->deleteForMe(message);
    } else {
        m_store->deleteMessage(message);
    }
}

void MainWindow::replyTo(qint64 message)
{
    if (!m_messenger)
        return;
    const ChatStore::Message m = m_store->message(message);
    const auto quote = m.conversation == m_current ? m_messenger->quoteOf(message) : std::nullopt;
    if (!quote)
        return;
    if (m_editing != 0) {
        m_compose->clear();
        m_editing = 0;
    }
    m_replyTo = message;
    const QString text = quote->text.isEmpty() ? tr("a message") : quote->text;
    m_replyLabel->setText(tr("Replying to %1: %2").arg(quoteAuthorName(quote->author), text));
    m_replyBar->show();
    m_compose->setFocus();
}

void MainWindow::startEdit(qint64 message)
{
    if (!m_messenger || !m_messenger->canEdit(message) || m_store->message(message).conversation != m_current)
        return;
    const ChatStore::Message m = m_store->message(message);
    m_replyTo = 0;
    m_editing = message;
    m_replyLabel->setText(tr("Editing: %1").arg(m.body.simplified().left(200)));
    m_replyBar->show();
    m_compose->setPlainText(m.body);
    m_compose->moveCursor(QTextCursor::End);
    m_compose->setFocus();
}

void MainWindow::cancelReply()
{
    if (m_editing != 0 && m_compose)
        m_compose->clear(); // the old text was only put there to change
    m_replyTo = 0;
    m_editing = 0;
    if (m_replyBar)
        m_replyBar->hide();
}

bool MainWindow::saveAttachment(qint64 attachment, const QString &path)
{
    QString error;
    if (!m_messenger || !m_messenger->saveAttachment(attachment, path, &error)) {
        showNotice(tr("Not saved: %1").arg(error));
        return false;
    }
    return true;
}

void MainWindow::send()
{
    const QString text = m_compose->toPlainText().trimmed();
    if (m_editing != 0) {
        // Saved: the new text (unchanged or impossible: nothing happens).
        if (m_messenger && !text.isEmpty())
            m_messenger->edit(m_editing, text);
        m_compose->clear();
        cancelReply();
        return;
    }
    if ((!text.isEmpty() || !m_pending.isEmpty()) && m_current != 0)
        aboutToSend();
    if (!m_pending.isEmpty() && m_messenger && m_current != 0) {
        // Each file its own message, the first answering what is replied to,
        // the text with the last. One that fails stays, with those after it.
        while (!m_pending.isEmpty()) {
            const PendingFile f = m_pending.first();
            const QString caption = m_pending.size() == 1 ? text : QString();
            QString error;
            const qint64 id = f.path.isEmpty() ? m_messenger->sendFileData(m_current, f.name, f.data, caption, &error, m_replyTo)
                                               : m_messenger->sendFile(m_current, f.path, caption, &error, m_replyTo);
            if (id == 0) {
                showNotice(tr("%1: %2").arg(f.name, error));
                refreshAttachBar();
                return;
            }
            m_pending.removeFirst();
            m_replyTo = 0;
        }
        refreshAttachBar();
        m_compose->clear();
        cancelReply();
        return;
    }
    if (text.isEmpty() || m_current == 0)
        return;
    if (m_messenger)
        m_messenger->send(m_current, text, m_replyTo);
    else
        m_store->addMessage(m_current, text);
    m_compose->clear();
    cancelReply();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Files or a picture dropped on the conversation: to send, as on the field.
    if (m_messages && watched == m_messages->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto *drag = static_cast<QDropEvent *>(event);
            if (canAttach() && carriesAttachments(drag->mimeData())) {
                drag->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto *drop = static_cast<QDropEvent *>(event);
            if (takeAttachments(drop->mimeData())) {
                drop->acceptProposedAction();
                m_compose->setFocus();
                return true;
            }
        }
    }
    // The messages: Ctrl+C copies the chosen ones, Esc lets go of them, and
    // typing goes on in the field.
    if (watched == m_messages && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &MainWindow::updateToBottom);
    if (watched == m_messages && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->matches(QKeySequence::Copy)) {
            copySelectedMessages();
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            m_messages->clearSelection();
            m_compose->setFocus();
            return true;
        }
        if (!key->text().isEmpty() && key->text().at(0).isPrint()
            && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && m_compose->isEnabled()
            && m_composer->isVisible()) {
            m_compose->setFocus();
            QCoreApplication::sendEvent(m_compose, event);
            return true;
        }
    }
    // In the find field: Enter older, Shift+Enter newer, Esc closes.
    if (watched == m_findField && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            findStep(key->modifiers() & Qt::ShiftModifier ? 1 : -1);
            return true;
        }
        if (key->key() == Qt::Key_Escape) {
            closeFind();
            return true;
        }
    }
    // Enter sends; Shift+Enter is a new line.
    if (watched == m_compose && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !(key->modifiers() & Qt::ShiftModifier)) {
            send();
            return true;
        }
        if (key->key() == Qt::Key_Escape && (m_replyTo != 0 || m_editing != 0)) {
            if (m_editing != 0)
                m_compose->clear(); // the old text was put there to change
            cancelReply();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::notify(qint64 conversation, const QString &from, const QString &text, bool request)
{
    // In front of them already: nothing to say.
    if (isActiveWindow() && !isMinimized() && conversation == m_current)
        return;
    // Their chosen name; for a request, with the username too (anyone can choose any name).
    if (m_store->conversation(conversation).isMuted())
        return; // muted: nothing at all
    const QString who = m_messenger ? m_messenger->displayName(from) : from;
    QString shownName = request && who != from ? QStringLiteral("%1 (@%2)").arg(who, from) : who;
    const ChatStore::Conversation c = m_store->conversation(conversation);
    if (c.kind == QLatin1String("group"))
        shownName = QStringLiteral("%1 · %2").arg(c.title, shownName);
    const auto shown = Notifications::make(Notifications::saved(), shownName, text, request);
    if (!shown)
        return;
    m_lastNotified = conversation;
    m_tray->showMessage(shown->title, shown->body);
    emit notified(conversation, shown->title, shown->body);
}

int MainWindow::unreadCount() const
{
    int count = 0;
    for (const ChatStore::Conversation &c : m_store->conversations()) {
        if (!c.isMuted())
            count += c.request ? (c.unread > 0 ? 1 : 0) : c.unread;
    }
    return count;
}

void MainWindow::updateUnread()
{
    if (m_tray)
        m_tray->setUnread(unreadCount());
}

void MainWindow::showAndRaise()
{
    show();
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
}

void MainWindow::quit()
{
    m_quitting = true;
    if (close())
        QApplication::quit();
    else
        m_quitting = false;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_tray && m_tray->isAvailable() && m_messenger && m_messenger->hasAccount() && !m_quitting) {
        // The window goes; the program stays in the tray and keeps receiving.
        QApplication::setQuitOnLastWindowClosed(false);
        hide();
        event->ignore();
        QSettings settings;
        if (!settings.value(QStringLiteral("window/toldAboutTray")).toBool()) {
            settings.setValue(QStringLiteral("window/toldAboutTray"), true);
            m_tray->showMessage(tr("Argonaut Chat is still running"),
                                tr("It keeps receiving messages in the tray. File › Quit ends it."));
        }
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    // A theme change arrives as PaletteChange (Qt 6), as in Argonaut Emails.
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        applyTheme();
    // Back in front of the open conversation: what came meanwhile is seen now.
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && m_current != 0)
        QTimer::singleShot(0, this, &MainWindow::markCurrentRead);
    QMainWindow::changeEvent(event);
}

void MainWindow::applyTheme()
{
    const Theme theme = currentTheme();
    if (theme.id == m_themeId)
        return;
    m_themeId = theme.id;
    m_theme = theme;
    setStyleSheet(styleSheetFor(theme));
    m_conversations->viewport()->update();
    m_messages->viewport()->update();
}
