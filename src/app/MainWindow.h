#pragma once

#include "ChatStore.h"
#include "Theme.h"

#include <QMainWindow>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

// Signal's layout: the conversations at the left (newest first, with the
// last message and the unread count), the chosen one at the right, its
// messages as bubbles (mine at the right, in the accent colour), and the
// field to write in under them (Enter sends, Shift+Enter is a new line).
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(ChatStore *store, QWidget *parent = nullptr);

    // Tests reach the parts.
    QListWidget *conversations() const { return m_conversations; }
    QListWidget *messages() const { return m_messages; }
    QPlainTextEdit *composeField() const { return m_compose; }
    qint64 currentConversation() const { return m_current; }
    void openConversation(qint64 id);

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildMenus();
    void reloadConversations();
    void reloadMessages();
    void send();
    void applyTheme();

    ChatStore *m_store;
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
