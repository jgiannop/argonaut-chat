#include "MainWindow.h"

#include "Dialogs.h"
#include "Messenger.h"
#include "Notifications.h"
#include "TrayIcon.h"
#include "StyleSheet.h"

#include <QAction>
#include <QCloseEvent>
#include <QSettings>
#include <QActionGroup>
#include <QApplication>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenuBar>
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
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme->accent);
        p->drawEllipse(avatar);
        QFont letter = option.font;
        letter.setBold(true);
        letter.setPixelSize(17);
        p->setFont(letter);
        p->setPen(Qt::white);
        p->drawText(avatar, Qt::AlignCenter, title.left(1).toUpper());

        const int unread = index.data(UnreadRole).toInt();
        const QRect text(avatar.right() + 12, r.top() + 10, r.right() - avatar.right() - 22, r.height() - 20);
        QFont name = option.font;
        name.setBold(true);
        const QFontMetrics nameMetrics(name);
        const QFontMetrics plain(option.font);
        const QString time = index.data(TimeRole).toString();
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
            p->fillPath(pill, m_theme->accent);
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

// A message as a bubble: mine at the right in the accent colour, theirs at
// the left; a picture or a file line on top when it has an attachment, then
// the text, then the time (and for mine, whether it was sent).
class BubbleDelegate : public QStyledItemDelegate
{
public:
    BubbleDelegate(const Theme *theme, QObject *parent)
        : QStyledItemDelegate(parent)
        , m_theme(theme)
    {
    }

    static constexpr int Pad = 10;
    static constexpr int Gap = 6;

    struct Layout
    {
        QRect image; // relative to the bubble's top left
        QRect file;
        QRect text;
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
        int width = smallMetrics.horizontalAdvance(footer);
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
            const QRect t = fm.boundingRect(QRect(0, 0, most, 100000), Qt::TextWordWrap, text);
            l.text = QRect(Pad, y, t.width(), t.height());
            y += t.height();
            width = qMax(width, t.width());
        }
        l.width = width + 2 * Pad;
        l.height = y + smallMetrics.height() + Pad + 2;
        return l;
    }

    static QString footerFor(const QModelIndex &index, bool *read)
    {
        QString footer = index.data(TimeRole).toString();
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

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        bool read = false;
        return {option.rect.width(), layout(option, index, footerFor(index, &read)).height + 8};
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const bool mine = index.data(OutgoingRole).toBool();
        bool read = false;
        const QString footer = footerFor(index, &read);
        const Layout l = layout(option, index, footer);
        QFont small = option.font;
        small.setPointSizeF(option.font.pointSizeF() * 0.85);
        const QFontMetrics smallMetrics(small);
        const QRect bubble(mine ? option.rect.right() - 14 - l.width : option.rect.left() + 14, option.rect.top() + 4,
                           l.width, l.height);
        QPainterPath path;
        path.addRoundedRect(bubble, 16, 16);
        p->fillPath(path, mine ? m_theme->accent : m_theme->pathBg);
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
        const QString file = index.data(FileRole).toString();
        if (!file.isEmpty()) {
            QFont bold = option.font;
            bold.setBold(true);
            p->setFont(bold);
            p->drawText(l.file.translated(origin), Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(bold).elidedText(file, Qt::ElideMiddle, l.file.width()));
            p->setFont(option.font);
        }
        if (!l.text.isNull())
            p->drawText(l.text.translated(origin), Qt::TextWordWrap, index.data().toString());
        p->setFont(small);
        // Read: the ticks in full white, as Signal fills them in.
        p->setPen(mine ? QColor(255, 255, 255, read ? 255 : 190) : m_theme->dimFg);
        p->drawText(QRect(bubble.left() + Pad, bubble.bottom() - Pad - smallMetrics.height(), bubble.width() - 2 * Pad,
                          smallMetrics.height()),
                    Qt::AlignRight | Qt::AlignVCenter, footer);
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
    auto *search = new QLineEdit(sidebar);
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
    sideLayout->addWidget(search);
    sideLayout->addWidget(m_conversations, 1);
    connect(search, &QLineEdit::textChanged, this, [this](const QString &text) {
        for (int i = 0; i < m_conversations->count(); ++i) {
            QListWidgetItem *item = m_conversations->item(i);
            item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive));
        }
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
    m_messages->setItemDelegate(new BubbleDelegate(&m_theme, m_messages));
    m_messages->setSelectionMode(QAbstractItemView::NoSelection);
    m_messages->setFocusPolicy(Qt::NoFocus);
    m_messages->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_messages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messages->setResizeMode(QListView::Adjust);
    m_messages->setWordWrap(true);
    auto *composer = new QWidget(chat);
    composer->setObjectName(QStringLiteral("composer"));
    auto *composerLayout = new QHBoxLayout(composer);
    composerLayout->setContentsMargins(12, 10, 12, 10);
    m_compose = new QPlainTextEdit(composer);
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
    m_attach->setToolTip(tr("Send a file (up to 25 MB), encrypted; what you have typed goes with it"));
    m_attach->setCursor(Qt::PointingHandCursor);
    m_attach->setEnabled(m_messenger != nullptr);
    connect(m_attach, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Send a File"));
        if (!path.isEmpty())
            attachFile(path);
    });
    composerLayout->addWidget(m_attach);
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

    chatLayout->addWidget(header);
    chatLayout->addWidget(m_messages, 1);
    chatLayout->addWidget(composer);
    chatLayout->addWidget(m_requestBar);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(sidebar);
    split->addWidget(chat);
    split->setStretchFactor(1, 1);
    split->setSizes({300, 700});
    setCentralWidget(split);

    connect(m_send, &QPushButton::clicked, this, &MainWindow::send);
    connect(m_compose, &QPlainTextEdit::textChanged, this, [this] {
        m_send->setEnabled(m_current != 0 && !m_compose->toPlainText().trimmed().isEmpty());
        // The field grows with what is written, up to six lines.
        const int lines = qBound(1, int(m_compose->document()->size().height()), 6);
        m_compose->setFixedHeight(qMax(40, lines * m_compose->fontMetrics().lineSpacing() + 18));
    });
    connect(m_conversations, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item)
            openConversation(item->data(IdRole).toLongLong());
    });
    connect(m_store, &ChatStore::conversationsChanged, this, &MainWindow::reloadConversations);
    connect(m_store, &ChatStore::conversationsChanged, this, &MainWindow::updateUnread);
    connect(m_store, &ChatStore::messagesChanged, this, [this](qint64 conversation) {
        if (conversation != m_current)
            return;
        reloadMessages();
        // Arrived while it is open in front of them: seen.
        if (isActiveWindow())
            QTimer::singleShot(0, this, &MainWindow::markCurrentRead);
    });
    connect(m_messages, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const qint64 id = item->data(AttachmentRole).toLongLong();
        const ChatStore::StoredAttachment a = m_store->attachment(id);
        if (id == 0 || a.file.isEmpty())
            return;
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
                    item->setData(FileRole, fileLine(m_store->attachment(id), !item->data(ImageRole).isNull()));
            }
        });
        connect(m_messenger, &Messenger::stateChanged, this, &MainWindow::updateConnection);
        connect(m_messenger, &Messenger::safetyNumberChanged, this, [this](const QString &username) {
            updateHeader();
            showNotice(tr("Your safety number with %1 has changed.").arg(username));
        });
        connect(m_messenger, &Messenger::problem, this, &MainWindow::showNotice);
        connect(m_messenger, &Messenger::messageReceived, this, &MainWindow::notify);
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
    m_safetyAction = conversation->addAction(tr("Safety Number…"), this, &MainWindow::showSafetyNumber);

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
    for (const ChatStore::Conversation &c : m_store->conversations()) {
        auto *item = new QListWidgetItem(c.title, m_conversations);
        item->setData(IdRole, c.id);
        item->setData(LastTextRole, c.lastText);
        item->setData(TimeRole, c.lastText.isEmpty() ? QString() : shortTime(c.lastActivity));
        item->setData(UnreadRole, c.id == m_current || c.request ? 0 : c.unread);
        item->setData(RequestRole, c.request);
        if (c.id == m_current)
            current = item;
    }
    if (current)
        m_conversations->setCurrentItem(current);
}

void MainWindow::openConversation(qint64 id)
{
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
    markCurrentRead();
    reloadMessages();
    m_compose->setFocus();
    m_send->setEnabled(!m_compose->toPlainText().trimmed().isEmpty());
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

void MainWindow::updateHeader()
{
    const ChatStore::Conversation c = m_store->conversation(m_current);
    const bool direct = c.kind == QLatin1String("direct");
    m_title->setText(c.title);
    if (c.kind == QLatin1String("self"))
        m_subtitle->setText(tr("Notes you keep for yourself, on this computer"));
    else if (direct && c.request)
        m_subtitle->setText(tr("@%1 · message request").arg(c.peer));
    else if (direct && m_messenger && m_messenger->safetyNumberHasChanged(c.peer))
        m_subtitle->setText(tr("⚠ Your safety number with %1 has changed: check it").arg(c.peer));
    else if (direct && m_messenger && m_messenger->isVerified(c.peer))
        m_subtitle->setText(tr("@%1 · ✓ verified").arg(c.peer));
    else
        m_subtitle->setText(direct ? QStringLiteral("@") + c.peer : c.peer);
    m_safety->setVisible(direct && m_messenger);
    if (m_safetyAction)
        m_safetyAction->setEnabled(direct && m_messenger);
    // A request: answer it before writing.
    const bool request = direct && c.request && m_messenger;
    m_composer->setVisible(!request);
    m_requestBar->setVisible(request);
    if (request)
        m_requestText->setText(tr("<b>%1</b> wants to chat with you. They won't know you have seen their messages "
                                  "until you accept.")
                                   .arg(c.peer.toHtmlEscaped()));
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
    if (!m_messenger || c.kind != QLatin1String("direct"))
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
    if (!m_messenger || c.kind != QLatin1String("direct"))
        return;
    if (ask && QMessageBox::question(this, tr("Block"),
                                     tr("Block %1? Their messages will be dropped, and they will not know. "
                                        "You can unblock them in File › Blocked People.")
                                         .arg(c.peer))
                   != QMessageBox::Yes)
        return;
    m_messenger->block(c.peer);
    openConversation(m_store->noteToSelf());
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
    m_messages->clear();
    for (const ChatStore::Message &m : m_store->messages(m_current)) {
        auto *item = new QListWidgetItem(m.body, m_messages);
        item->setData(IdRole, m.id);
        item->setData(OutgoingRole, m.outgoing());
        item->setData(TimeRole, QLocale().toString(m.sentAt.toLocalTime().time(), QLocale::ShortFormat));
        item->setData(StatusRole, m.status);
        const QList<ChatStore::StoredAttachment> attachments = m_store->attachments(m.id);
        if (!attachments.isEmpty()) {
            const ChatStore::StoredAttachment &a = attachments.first();
            item->setData(AttachmentRole, a.id);
            const QPixmap preview = previewFor(a);
            if (!preview.isNull())
                item->setData(ImageRole, preview);
            item->setData(FileRole, fileLine(a, !preview.isNull()));
            item->setToolTip(a.file.isEmpty() ? QString() : tr("Click to save %1").arg(a.name));
        }
    }
    m_messages->scrollToBottom();
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
    QString error;
    if (m_messenger->sendFile(m_current, path, m_compose->toPlainText().trimmed(), &error) == 0) {
        showNotice(error);
        return;
    }
    m_compose->clear();
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
    if (text.isEmpty() || m_current == 0)
        return;
    if (m_messenger)
        m_messenger->send(m_current, text);
    else
        m_store->addMessage(m_current, text);
    m_compose->clear();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Enter sends; Shift+Enter is a new line.
    if (watched == m_compose && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && !(key->modifiers() & Qt::ShiftModifier)) {
            send();
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
    const auto shown = Notifications::make(Notifications::saved(), from, text, request);
    if (!shown)
        return;
    m_lastNotified = conversation;
    m_tray->showMessage(shown->title, shown->body);
    emit notified(conversation, shown->title, shown->body);
}

int MainWindow::unreadCount() const
{
    int count = 0;
    for (const ChatStore::Conversation &c : m_store->conversations())
        count += c.request ? (c.unread > 0 ? 1 : 0) : c.unread;
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
