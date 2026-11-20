#pragma once

#include <QDialog>

class Messenger;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// The first start: which server, and the username people will find you by.
// The keys are made on this computer; only their public halves go to the
// server. Cancel to use Note to Self alone.
class SignUpDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SignUpDialog(Messenger *messenger, QWidget *parent = nullptr);

    // Tests reach the parts.
    QLineEdit *serverField() const { return m_server; }
    QLineEdit *usernameField() const { return m_username; }
    QPushButton *signUpButton() const { return m_signUp; }
    QLabel *messageLabel() const { return m_message; }

private:
    void signUp();
    void updateButton();

    Messenger *m_messenger;
    QLineEdit *m_server = nullptr;
    QLineEdit *m_username = nullptr;
    QLabel *m_message = nullptr;
    QPushButton *m_signUp = nullptr;
    bool m_busy = false;
};

// The safety number with someone, to compare in person or on a call: the
// same 60 digits on both sides mean nobody sits in between.
class SafetyNumberDialog : public QDialog
{
    Q_OBJECT

public:
    SafetyNumberDialog(Messenger *messenger, const QString &username, QWidget *parent = nullptr);

    QLabel *numberLabel() const { return m_number; }
    QPushButton *verifyButton() const { return m_verify; }

private:
    void update();

    Messenger *m_messenger;
    QString m_username;
    QLabel *m_warning = nullptr;
    QLabel *m_number = nullptr;
    QLabel *m_state = nullptr;
    QPushButton *m_verify = nullptr;
};

// The people whose messages are dropped, to unblock.
class BlockedDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BlockedDialog(Messenger *messenger, QWidget *parent = nullptr);

    QListWidget *list() const { return m_list; }
    QPushButton *unblockButton() const { return m_unblock; }

private:
    void reload();

    Messenger *m_messenger;
    QListWidget *m_list = nullptr;
    QPushButton *m_unblock = nullptr;
};
