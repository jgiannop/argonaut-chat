#pragma once

#include <QDialog>

class AppLock;
class QCheckBox;
class Messenger;
class QComboBox;
class QDialogButtonBox;
class QImage;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
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
    QLineEdit *usernameField() const { return m_username; }
    QPushButton *signUpButton() const { return m_signUp; }
    QPushButton *linkButton() const { return m_link; }
    QLabel *messageLabel() const { return m_message; }
    // While linking: the code to type on the other computer.
    QLineEdit *codeField() const { return m_code; }
    // Not Now, or closed: a link under way is cancelled.
    void reject() override;

private:
    void signUp();
    void link();
    void updateButton();

    Messenger *m_messenger;
    QLineEdit *m_username = nullptr;
    QLabel *m_message = nullptr;
    QPushButton *m_signUp = nullptr;
    QPushButton *m_link = nullptr;
    QWidget *m_codeRow = nullptr;
    QLineEdit *m_code = nullptr;
    bool m_busy = false;
    bool m_linking = false;
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

// My profile: a display name and a picture. They go only to the people I
// write to or accept, inside encrypted messages; the server never has them.
class ProfileDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProfileDialog(Messenger *messenger, QWidget *parent = nullptr);

    QLineEdit *nameField() const { return m_name; }
    QLabel *pictureLabel() const { return m_picture; }
    QPushButton *saveButton() const { return m_save; }
    // A picture from a file (Choose Picture…), made ready to send.
    bool setPicture(const QString &path, QString *error = nullptr);
    void removePicture();
    QByteArray picture() const { return m_avatar; }
    // A square from the middle, 256×256, PNG (JPEG if that is too large).
    static QByteArray preparePicture(const QImage &image);

private:
    void showPicture();
    void save();

    Messenger *m_messenger;
    QLineEdit *m_name = nullptr;
    QLabel *m_picture = nullptr;
    QLabel *m_message = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_remove = nullptr;
    QByteArray m_avatar;
};

// A new group (conversation 0), or a group's name and members: changed by
// its admin, only shown to the others.
class GroupDialog : public QDialog
{
    Q_OBJECT

public:
    GroupDialog(Messenger *messenger, qint64 conversation, QWidget *parent = nullptr);

    QLineEdit *nameField() const { return m_name; }
    QPlainTextEdit *membersField() const { return m_members; }
    QPushButton *okButton() const { return m_ok; }
    qint64 conversation() const { return m_conversation; } // the new one, once made
    // The usernames in the field: separated by commas, spaces or lines.
    QStringList members() const;

private:
    void apply();

    Messenger *m_messenger;
    qint64 m_conversation;
    QLineEdit *m_name = nullptr;
    QPlainTextEdit *m_members = nullptr;
    QLabel *m_message = nullptr;
    QPushButton *m_ok = nullptr;
};

// The account's computers: this one marked; link a new one by the code it
// shows, or remove one (the first computer removes the others; any removes itself).
class DevicesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DevicesDialog(Messenger *messenger, QWidget *parent = nullptr);

    QListWidget *list() const { return m_list; }
    QPushButton *removeButton() const { return m_remove; }
    QLabel *messageLabel() const { return m_message; }
    QCheckBox *historyCheck() const { return m_history; }
    // Link New Device…'s work, after the code is typed.
    void linkWithCode(const QString &code);
    void removeSelected(bool ask);

private:
    void reload();
    void updateButtons();

    Messenger *m_messenger;
    QListWidget *m_list = nullptr;
    QPushButton *m_remove = nullptr;
    QLabel *m_message = nullptr;
    QCheckBox *m_history = nullptr;
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

// File › App Lock…: a passphrase (twice) and the idle time to turn it on;
// once on, the current passphrase to change either or to turn it off.
class AppLockDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AppLockDialog(AppLock *lock, QWidget *parent = nullptr);

    QLineEdit *currentField() const { return m_current; }
    QLineEdit *newField() const { return m_new; }
    QLineEdit *repeatField() const { return m_repeat; }
    QComboBox *idleBox() const { return m_idle; }
    QLabel *errorLabel() const { return m_error; }
    QPushButton *okButton() const { return m_ok; }
    QPushButton *turnOffButton() const { return m_turnOff; }

private:
    void save();

    AppLock *m_lock;
    QLineEdit *m_current = nullptr;
    QLineEdit *m_new = nullptr;
    QLineEdit *m_repeat = nullptr;
    QComboBox *m_idle = nullptr;
    QLabel *m_error = nullptr;
    QPushButton *m_ok = nullptr;
    QPushButton *m_turnOff = nullptr;
};
