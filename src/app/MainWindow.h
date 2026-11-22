#pragma once

#include "AppLock.h"
#include "ChatStore.h"
#include "Theme.h"
#include "VoiceAudio.h"

#include <QHash>
#include <QMainWindow>
#include <QElapsedTimer>
#include <QTimer>
#include <QPixmap>

class Messenger;
class TrayIcon;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMimeData;
class QMenu;
class QToolButton;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QStyledItemDelegate;

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
    // The part of one message's text chosen with the mouse (empty: none).
    QString selectedText() const;
    void selectText(qint64 message, int start, int end);
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
    // A file into the open conversation at once, with what is typed as its caption.
    void attachFile(const QString &path);
    // Files waiting above the field until Enter/Send (📎, pasted, dropped on
    // the field or the messages; ✕ takes one back), each its own message, the
    // typed text with the last. false and a notice: a folder, too big, too many.
    bool addAttachment(const QString &path);
    bool addAttachmentData(const QString &name, const QByteArray &data); // a pasted picture
    // What a paste or drop carries: local files, or a picture without text (a
    // screenshot). false: nothing for us, it is text.
    bool takeAttachments(const QMimeData *data);
    int pendingAttachments() const { return int(m_pending.size()); }
    void removeAttachment(int index);
    QWidget *attachBar() const { return m_attachBar; }
    // An attachment, decrypted, into `path` (a click on it asks where).
    bool saveAttachment(qint64 attachment, const QString &path);
    // Finds them on the server and opens the conversation (New Chat).
    void startChatWith(const QString &username);
    void showSignUp();

    // The app lock (File › App Lock…, Lock Now Ctrl+L): the chats hidden
    // behind a passphrase field, at start, when asked, and after the idle
    // minutes; menus and shortcuts off meanwhile (Quit stays), nothing marked
    // read, notifications without names or text. Messages still arrive.
    bool isLocked() const { return m_locked; }
    void lockNow();
    bool unlock(const QString &passphrase); // false: the lock screen says why
    AppLock *appLock() { return &m_appLock; }
    QWidget *lockScreen() const { return m_lockScreen; }
    QAction *lockAction() const { return m_lockAction; }
    void setIdleLockMsForTests(int ms) { m_idleLockMsForTests = ms; restartIdleLock(); }

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
    QAction *typingIndicatorsAction() const { return m_typingAction; }
    // View › Read Receipts (QSettings privacy/readReceipts, on by default).
    QAction *readReceiptsAction() const { return m_readReceiptsAction; }
    // Find in Conversation (Ctrl+F): its matches, newest first in turn; Enter
    // goes to older ones, Shift+Enter newer, Esc closes.
    void showFind();
    void closeFind();
    void findStep(int delta); // -1 older, +1 newer
    qint64 currentFindMatch() const;
    QLineEdit *findField() const { return m_findField; }
    QLabel *findCountLabel() const { return m_findCount; }
    QWidget *findBar() const { return m_findBar; }
    // Voice messages: 🎤 records (the bar shows the time; Cancel, Send), a
    // click on one plays it (again: stops).
    void startRecording();
    void stopRecording(bool send);
    void sendVoice(const QByteArray &samples); // what a recording gives (tests too)
    void toggleVoice(qint64 attachment);
    VoicePlayer *voicePlayer() { return &m_player; }
    QWidget *replyBar() const { return m_replyBar; }
    QLabel *replyLabel() const { return m_replyLabel; }
    // Delete for Me / for Everyone (`ask`: confirm first).
    void deleteMessage(qint64 message, bool forEveryone, bool ask);
    // A message's right-click menu (the caller owns it); none for a note.
    QMenu *messageMenu(qint64 message);
    // Mute, archive, pin (as Signal's; kept on this computer only). Muted: no
    // notifications, not in the tray's count, a grey count. `seconds` -1:
    // always, 0: unmute. Archived: under "Archived Chats" until their next
    // message (unless muted). Pinned: on top, at most ChatStore::MaxPinned
    // (a notice when full).
    void muteConversation(qint64 conversation, qint64 seconds);
    void archiveConversation(qint64 conversation, bool archived);
    bool pinConversation(qint64 conversation, bool pinned);
    // A conversation's right-click menu in the list (the caller owns it).
    QMenu *conversationMenu(qint64 conversation);
    void showArchived(bool show);
    bool showingArchived() const { return m_showArchived; }
    QToolButton *archivedButton() const { return m_archivedButton; }
    // Where you left off: "New messages" above the first unread one, there on
    // opening; the place kept when the list changes while you read further up;
    // ↓ (with how many came since) back to the newest.
    qint64 unreadMarker() const { return m_unreadMarker; }
    QToolButton *toBottomButton() const { return m_toBottom; }
    void scrollToNewest();
    // A click on a reply's quote: the message it answers scrolled into view
    // and outlined for a moment (false: it is gone, deleted or never here).
    bool jumpToQuote(qint64 reply);
    qint64 flashedMessage() const { return m_flash; }
    // Copying (click, Ctrl/Shift+click, Ctrl+A; Ctrl+C): one message's text as
    // it is, several as "[date time] Name: text" lines, oldest first.
    QString selectedMessagesText() const;
    void copySelectedMessages();

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
    QString voiceLine(const ChatStore::StoredAttachment &attachment);
    void refreshVoiceLines();
    // "typing…", "Anna is typing…": empty when nobody is.
    QString typingText(qint64 conversation) const;
    // "You", or their name: who wrote what a reply answers.
    QString quoteAuthorName(const QString &author) const;
    void notify(qint64 conversation, const QString &from, const QString &text, bool request, bool mentionsMe = false);
    // @mentions while writing in a group: `mentionList` under the field shows
    // the members matching what follows "@"; one picked becomes "@Name".
    struct ComposeMention
    {
        QString username;
        QString label; // "@Name", as written in the field
    };
    QList<ComposeMention> m_mentions;
    QListWidget *m_mentionList = nullptr;
    int m_mentionStart = -1;     // where the "@" being completed is in the field
    int m_mentionDismissed = -1; // Esc on that "@": no list for it
    void updateMentionList();
    void pickMention();
    QString mentionLabel(const QString &username, const QStringList &members) const;
    // Where the picked ones are in `text` (the field's, trimmed).
    QList<Protocol::Mention> composedMentions(const QString &text) const;
    void updateUnread();

    ChatStore *m_store;
    Messenger *m_messenger;
    AppLock m_appLock;
    bool m_locked = false;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_lockScreen = nullptr;
    QLineEdit *m_lockField = nullptr;
    QLabel *m_lockMessage = nullptr;
    QAction *m_lockAction = nullptr;
    QAction *m_quitAction = nullptr;
    QTimer m_idleLock;
    int m_idleLockMsForTests = 0;
    void restartIdleLock();
    void updateLockAction();
    void showAppLockSettings();
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
    QAction *m_pinAction = nullptr;
    QMenu *m_muteMenu = nullptr;
    QAction *m_archiveAction = nullptr;
    void fillMuteMenu(QMenu *menu, qint64 conversation);
    void updateChatActions();
    void applySearch();
    QLineEdit *m_search = nullptr;
    QToolButton *m_archivedButton = nullptr;
    bool m_showArchived = false;
    QTimer m_muteTimer; // a mute running out: the list again
    qint64 m_unreadMarker = 0;   // the message "New messages" is above (this opening)
    bool m_markerFresh = false;  // not scrolled to yet
    bool m_stickToBottom = true; // the next reload ends at the newest (opening, sending)
    qint64 m_newestSeen = 0;     // the newest message when last at the bottom
    QToolButton *m_toBottom = nullptr;
    void aboutToSend();
    void updateToBottom();
    void placeToBottom();
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
    QTimer m_typingIdle; // "stopped" 5 s after the last key
    QAction *m_typingAction = nullptr;
    QAction *m_readReceiptsAction = nullptr;
    void runFind();
    void showFindMatch();
    static constexpr int DefaultMessageLimit = 500;
    int m_messageLimit = DefaultMessageLimit; // grows to show an older match
    struct PendingFile
    {
        QString path;    // a file on disk, read when sent
        QString name;
        QByteArray data; // or bytes in memory (pasted)
        qint64 size = 0;
        QPixmap thumbnail;
    };
    static constexpr int MaxPendingFiles = 10;
    QList<PendingFile> m_pending;
    QWidget *m_attachBar = nullptr;
    bool canAttach() const;
    void refreshAttachBar();
    void updateSendButton();
    qint64 m_flash = 0;          // the message a quote's click went to, outlined for a moment
    QTimer m_flashTimer;
    bool m_quoteClicked = false; // that click is not the bubble's own (play, save)
    QListWidgetItem *messageItem(qint64 message) const;
    QString copyText(const ChatStore::Message &m) const;
    // Part of a message's text, chosen with the mouse (drag; a double-click
    // takes a word): copied by Ctrl+C or Copy in its menu before whole messages.
    struct TextSelection
    {
        qint64 message = 0;
        int anchor = 0;
        int cursor = 0;
    };
    TextSelection m_textSelection;
    bool m_selectingText = false;
    QElapsedTimer m_doubleClicked; // for a triple-click
    QPoint m_doubleClickPos;
    QStyledItemDelegate *m_bubbles = nullptr;
    int textPositionAt(QListWidgetItem *item, const QPoint &pos, bool *onText) const;
    void clearTextSelection();
    QWidget *m_findBar = nullptr;
    QLineEdit *m_findField = nullptr;
    QLabel *m_findCount = nullptr;
    QList<qint64> m_findResults;
    int m_findIndex = -1;
    VoiceRecorder m_recorder;
    VoicePlayer m_player;
    QToolButton *m_voiceButton = nullptr;
    QWidget *m_recordBar = nullptr;
    QLabel *m_recordLabel = nullptr;
    QHash<qint64, qint64> m_voiceDurations; // attachment → ms (−1: not playable)
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
