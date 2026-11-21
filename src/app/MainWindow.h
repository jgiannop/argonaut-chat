#pragma once

#include "ChatStore.h"
#include "Theme.h"

#include <QHash>
#include <QMainWindow>
#include <QPixmap>

class Messenger;
class TrayIcon;
class QLabel;
class QListWidget;
class QMenu;
class QToolButton;
class QPlainTextEdit;
class QPushButton;

// Signal's layout: the conversations at the left (newest first, with the
// last message and the unread count), the chosen one at the right, its
// messages as bubbles (mine at the right, in the accent colour), and the
// field to write in under them (Enter sends, Shift+Enter is a new line).
//
// With a Messenger, conversations with people go through the server,
// encrypted; without one (or before signing up) only Note to Self works.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(ChatStore *store, Messenger *messenger = nullptr, QWidget *parent = nullptr);

    // Tests reach the parts.
    QListWidget *conversations() const { return m_conversations; }
    QListWidget *messages() const { return m_messages; }
    QPlainTextEdit *composeField() const { return m_compose; }
    qint64 currentConversation() const { return m_current; }
    QLabel *connectionLabel() const { return m_connection; }
    QLabel *noticeLabel() const { return m_notice; }
    QToolButton *safetyNumberButton() const { return m_safety; }
    void openConversation(qint64 id);
    QWidget *requestBar() const { return m_requestBar; }
    QWidget *composer() const { return m_composer; }
    // The open message request (the bar's buttons; `ask`: confirm first).
    void acceptRequest();
    void deleteRequest(bool ask);
    void blockRequest(bool ask);
    // The open group: left (`ask`: confirm first).
    void leaveGroup(bool ask);
    // The open conversation's disappearing messages (0: off).
    void setDisappearing(qint64 seconds);
    QMenu *timerMenu() const { return m_timerMenu; }
    // A file into the open conversation, with what is typed as its caption (the 📎 button).
    void attachFile(const QString &path);
    // An attachment, decrypted, into `path` (a click on it asks where).
    bool saveAttachment(qint64 attachment, const QString &path);
    // Finds them on the server and opens the conversation (New Chat).
    void startChatWith(const QString &username);
    void showSignUp();

    TrayIcon *tray() const { return m_tray; }
    // The tray's number: unread messages, a request counting as one.
    int unreadCount() const;
    // Ends the program (File › Quit); closing the window only hides it while there is a tray.
    void quit();
    void showAndRaise();
    // Replies (as Signal's): a message's menu has Reply; the bar above the
    // field shows what the next message answers, until it goes or ✕ / Esc.
    void replyTo(qint64 message);
    void cancelReply();
    qint64 replyingTo() const { return m_replyTo; }
    // Edit (mine, as Signal's): the text back in the field, the bar says so;
    // Enter saves, Esc or ✕ leaves it as it was. cancelReply() ends it too.
    void startEdit(qint64 message);
    qint64 editing() const { return m_editing; }
    QWidget *replyBar() const { return m_replyBar; }
    QLabel *replyLabel() const { return m_replyLabel; }
    // Delete for Me / for Everyone (`ask`: confirm first).
    void deleteMessage(qint64 message, bool forEveryone, bool ask);
    // A message's right-click menu (the caller owns it); none for a note.
    QMenu *messageMenu(qint64 message);

signals:
    // A notification was shown (tests: the offscreen platform has no tray).
    void notified(qint64 conversation, const QString &title, const QString &body);

protected:
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildMenus();
    void reloadConversations();
    void reloadMessages();
    void send();
    void applyTheme();
    void updateConnection();
    void updateHeader();
    void showNotice(const QString &text);
    void newChat();
    void showSafetyNumber();
    void markCurrentRead();
    QPixmap previewFor(const ChatStore::StoredAttachment &attachment);
    QPixmap avatarFor(const QString &username);
    QString fileLine(const ChatStore::StoredAttachment &attachment, bool shownAsImage) const;
    // "You", or their name: who wrote what a reply answers.
    QString quoteAuthorName(const QString &author) const;
    void notify(qint64 conversation, const QString &from, const QString &text, bool request);
    void updateUnread();

    ChatStore *m_store;
    Messenger *m_messenger;
    QLabel *m_connection = nullptr;
    QLabel *m_notice = nullptr;
    QToolButton *m_safety = nullptr;
    QAction *m_signUpAction = nullptr;
    QAction *m_newChatAction = nullptr;
    QAction *m_safetyAction = nullptr;
    QAction *m_blockedAction = nullptr;
    TrayIcon *m_tray = nullptr;
    QToolButton *m_attach = nullptr;
    QHash<qint64, QPixmap> m_previews; // attachment -> its picture, decrypted in memory
    QHash<qint64, int> m_progress;     // attachment -> percent up or down
    QHash<QString, QPixmap> m_avatars; // username -> their picture, small
    QAction *m_profileAction = nullptr;
    QAction *m_newGroupAction = nullptr;
    QAction *m_devicesAction = nullptr;
    QMenu *m_timerMenu = nullptr;
    QAction *m_membersAction = nullptr;
    QAction *m_leaveAction = nullptr;
    QLabel *m_leftNote = nullptr;
    qint64 m_lastNotified = 0;
    bool m_quitting = false;
    QWidget *m_composer = nullptr;
    QWidget *m_replyBar = nullptr;
    QLabel *m_replyLabel = nullptr;
    qint64 m_replyTo = 0;
    qint64 m_editing = 0;
    QWidget *m_requestBar = nullptr;
    QLabel *m_requestText = nullptr;
    QPushButton *m_acceptButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_blockButton = nullptr;
    QListWidget *m_conversations = nullptr;
    QListWidget *m_messages = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QPlainTextEdit *m_compose = nullptr;
    QPushButton *m_send = nullptr;
    qint64 m_current = 0;
    QString m_themeId;
    Theme m_theme;
};
