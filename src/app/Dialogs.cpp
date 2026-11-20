#include "Dialogs.h"

#include "ChatStore.h"
#include "Messenger.h"
#include "Protocol.h"
#include "SafetyNumber.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPlainTextEdit>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QRegularExpression>
#include <QVBoxLayout>

SignUpDialog::SignUpDialog(Messenger *messenger, QWidget *parent)
    : QDialog(parent)
    , m_messenger(messenger)
{
    setWindowTitle(tr("Sign Up"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("<b>Welcome to Argonaut Chat.</b><br>Choose a username: people find you by it. "
                                "No phone number, no email. Your keys are made on this computer and never leave it; "
                                "the server only passes encrypted messages on."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    m_server = new QLineEdit(this);
    m_server->setObjectName(QStringLiteral("serverField"));
    m_server->setPlaceholderText(QStringLiteral("wss://chat.example.org"));
    m_username = new QLineEdit(this);
    m_username->setObjectName(QStringLiteral("usernameField"));
    m_username->setPlaceholderText(tr("3 to 32 letters, digits, _ or ."));
    m_username->setMaxLength(32);
    form->addRow(tr("Server:"), m_server);
    form->addRow(tr("Username:"), m_username);
    layout->addLayout(form);

    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    layout->addWidget(m_message);

    // Linking: the code, shown big enough to copy.
    m_codeRow = new QWidget(this);
    auto *codeLayout = new QHBoxLayout(m_codeRow);
    codeLayout->setContentsMargins(0, 0, 0, 0);
    m_code = new QLineEdit(m_codeRow);
    m_code->setObjectName(QStringLiteral("linkCode"));
    m_code->setReadOnly(true);
    m_code->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *copy = new QPushButton(tr("Copy"), m_codeRow);
    connect(copy, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setText(m_code->text()); });
    codeLayout->addWidget(m_code, 1);
    codeLayout->addWidget(copy);
    m_codeRow->hide();
    layout->addWidget(m_codeRow);

    auto *buttons = new QDialogButtonBox(this);
    m_signUp = buttons->addButton(tr("Sign Up"), QDialogButtonBox::AcceptRole);
    m_link = buttons->addButton(tr("Link to My Account…"), QDialogButtonBox::ActionRole);
    m_link->setToolTip(tr("This computer joins an account you already have on another computer"));
    connect(m_link, &QPushButton::clicked, this, &SignUpDialog::link);
    buttons->addButton(tr("Not Now"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &SignUpDialog::signUp);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_server, &QLineEdit::textChanged, this, &SignUpDialog::updateButton);
    connect(m_username, &QLineEdit::textChanged, this, &SignUpDialog::updateButton);
    updateButton();
    resize(440, sizeHint().height());
}

void SignUpDialog::updateButton()
{
    const QUrl url(m_server->text().trimmed());
    const bool serverOk = url.isValid() && !url.host().isEmpty()
                          && (url.scheme() == QLatin1String("wss") || url.scheme() == QLatin1String("ws"));
    const bool nameOk = !Protocol::normalizeUsername(m_username->text()).isEmpty();
    m_signUp->setEnabled(!m_busy && serverOk && nameOk);
    if (!m_busy)
        m_message->setText(!m_username->text().isEmpty() && !nameOk
                               ? tr("A username is 3 to 32 letters (a-z), digits, _ or ., and starts with a letter.")
                               : QString());
}

void SignUpDialog::link()
{
    const QUrl url(m_server->text().trimmed());
    if (!url.isValid() || url.host().isEmpty()) {
        m_message->setText(tr("First the server, as on your other computer."));
        return;
    }
    m_busy = true;
    m_linking = true;
    updateButton();
    m_link->setEnabled(false);
    m_server->setEnabled(false);
    m_username->setEnabled(false);
    m_message->setText(tr("Connecting…"));
    QPointer<SignUpDialog> self(this);
    m_messenger->linkThisDevice(
        url,
        [self](const QString &code) {
            if (!self)
                return;
            self->m_code->setText(code);
            self->m_codeRow->show();
            self->m_message->setText(tr("On your other computer: File › Linked Devices › Link New Device…, and "
                                        "type or paste this code there. It works once, for ten minutes."));
        },
        [self](bool ok, const QString &error) {
            if (!self)
                return;
            self->m_busy = false;
            self->m_linking = false;
            if (ok) {
                self->accept();
                return;
            }
            self->m_codeRow->hide();
            self->m_server->setEnabled(true);
            self->m_username->setEnabled(true);
            self->m_link->setEnabled(true);
            self->updateButton();
            self->m_message->setText(error.isEmpty() ? tr("The linking did not work.") : error);
        });
}

void SignUpDialog::reject()
{
    if (m_linking)
        m_messenger->cancelLinking(); // its answer comes before the dialog goes
    QDialog::reject();
}

void SignUpDialog::signUp()
{
    m_busy = true;
    updateButton();
    m_server->setEnabled(false);
    m_username->setEnabled(false);
    m_message->setText(tr("Making your keys and signing up…"));
    QPointer<SignUpDialog> self(this);
    m_messenger->signUp(QUrl(m_server->text().trimmed()), m_username->text(), [self](bool ok, const QString &error) {
        if (!self)
            return;
        self->m_busy = false;
        if (ok) {
            self->accept();
            return;
        }
        self->m_server->setEnabled(true);
        self->m_username->setEnabled(true);
        self->updateButton();
        self->m_message->setText(error.isEmpty() ? tr("The sign-up did not work.") : error);
    });
}

SafetyNumberDialog::SafetyNumberDialog(Messenger *messenger, const QString &username, QWidget *parent)
    : QDialog(parent)
    , m_messenger(messenger)
    , m_username(username)
{
    setWindowTitle(tr("Safety Number with %1").arg(username));
    auto *layout = new QVBoxLayout(this);
    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    m_warning->setText(tr("<b>%1's safety number has changed.</b> They may have reinstalled Argonaut Chat, "
                          "or someone may be trying to sit between you. Compare the new number with them.")
                           .arg(username.toHtmlEscaped()));
    layout->addWidget(m_warning);

    m_number = new QLabel(this);
    m_number->setObjectName(QStringLiteral("safetyNumber"));
    m_number->setAlignment(Qt::AlignCenter);
    m_number->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(mono.pointSizeF() * 1.4);
    m_number->setFont(mono);
    layout->addWidget(m_number);

    auto *explain = new QLabel(tr("Compare these numbers with %1, in person or on a call. If they are the same on both "
                                  "devices, your messages reach only them.")
                                   .arg(username.toHtmlEscaped()),
                               this);
    explain->setWordWrap(true);
    layout->addWidget(explain);
    m_state = new QLabel(this);
    layout->addWidget(m_state);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_verify = buttons->addButton(QString(), QDialogButtonBox::ActionRole);
    m_verify->setObjectName(QStringLiteral("verifyButton"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_verify, &QPushButton::clicked, this, [this] {
        m_messenger->setVerified(m_username, !m_messenger->isVerified(m_username));
        update();
    });
    update();
    resize(460, sizeHint().height());
}

void SafetyNumberDialog::update()
{
    const QString digits = m_messenger->safetyNumber(m_username);
    if (digits.isEmpty()) {
        m_number->setText(tr("Not known yet: write to them first."));
        m_verify->setEnabled(false);
    } else {
        // Three lines of four groups, as Signal shows it.
        const QStringList groups = SafetyNumber::display(digits).split(QLatin1Char(' '));
        QStringList lines;
        for (int i = 0; i < groups.size(); i += 4)
            lines << groups.mid(i, 4).join(QLatin1Char(' '));
        m_number->setText(lines.join(QLatin1Char('\n')));
        m_verify->setEnabled(true);
    }
    m_warning->setVisible(m_messenger->safetyNumberHasChanged(m_username));
    const bool verified = m_messenger->isVerified(m_username);
    m_state->setText(verified ? tr("✓ You have verified it.") : tr("Not verified."));
    m_verify->setText(verified ? tr("Clear Verification") : tr("Mark as Verified"));
}

BlockedDialog::BlockedDialog(Messenger *messenger, QWidget *parent)
    : QDialog(parent)
    , m_messenger(messenger)
{
    setWindowTitle(tr("Blocked People"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Their messages are dropped, and they are not told."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("blockedList"));
    layout->addWidget(m_list);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_unblock = buttons->addButton(tr("Unblock"), QDialogButtonBox::ActionRole);
    m_unblock->setObjectName(QStringLiteral("unblockButton"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) { m_unblock->setEnabled(row >= 0); });
    connect(m_unblock, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem *item = m_list->currentItem()) {
            m_messenger->unblock(item->text());
            reload();
        }
    });
    reload();
    resize(320, 360);
}

void BlockedDialog::reload()
{
    m_list->clear();
    m_list->addItems(m_messenger->blocked());
    m_unblock->setEnabled(false);
}

ProfileDialog::ProfileDialog(Messenger *messenger, QWidget *parent)
    : QDialog(parent)
    , m_messenger(messenger)
{
    setWindowTitle(tr("Profile"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Your name and picture go only to the people you write to or accept, encrypted "
                                "like your messages; the server never sees them. Everyone sees your username "
                                "too."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *row = new QHBoxLayout;
    m_picture = new QLabel(this);
    m_picture->setObjectName(QStringLiteral("profilePicture"));
    m_picture->setFixedSize(96, 96);
    m_picture->setAlignment(Qt::AlignCenter);
    row->addWidget(m_picture);
    auto *pictureButtons = new QVBoxLayout;
    auto *choose = new QPushButton(tr("Choose Picture…"), this);
    m_remove = new QPushButton(tr("Remove Picture"), this);
    pictureButtons->addWidget(choose);
    pictureButtons->addWidget(m_remove);
    pictureButtons->addStretch();
    row->addLayout(pictureButtons);
    row->addStretch();
    layout->addLayout(row);

    auto *form = new QFormLayout;
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("profileName"));
    m_name->setMaxLength(Protocol::MaxDisplayName);
    m_name->setPlaceholderText(tr("Your name, as others will see it"));
    form->addRow(tr("Name:"), m_name);
    layout->addLayout(form);
    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    layout->addWidget(m_message);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_save = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    layout->addWidget(buttons);

    const ChatStore::Profile mine = m_messenger->myProfile();
    m_name->setText(mine.name);
    m_avatar = mine.avatar;
    showPicture();

    connect(choose, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Choose a Picture"), QString(),
                                                          tr("Pictures (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"));
        QString error;
        if (!path.isEmpty() && !setPicture(path, &error))
            m_message->setText(error);
    });
    connect(m_remove, &QPushButton::clicked, this, &ProfileDialog::removePicture);
    connect(buttons, &QDialogButtonBox::accepted, this, &ProfileDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    resize(440, sizeHint().height());
}

QByteArray ProfileDialog::preparePicture(const QImage &image)
{
    if (image.isNull())
        return {};
    const int side = qMin(image.width(), image.height());
    const QImage square = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
                              .scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB32);
    // Made anew: nothing of the original file (its metadata, where it was taken) goes along.
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    square.save(&buffer, "PNG");
    if (out.size() > Messenger::MaxAvatarBytes) {
        out.clear();
        buffer.seek(0);
        square.save(&buffer, "JPEG", 85);
    }
    return out;
}

bool ProfileDialog::setPicture(const QString &path, QString *error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    const QByteArray prepared = preparePicture(image);
    if (prepared.isEmpty()) {
        if (error)
            *error = tr("That is not a picture this program can read.");
        return false;
    }
    m_avatar = prepared;
    showPicture();
    return true;
}

void ProfileDialog::removePicture()
{
    m_avatar.clear();
    showPicture();
}

void ProfileDialog::showPicture()
{
    QImage image;
    if (!m_avatar.isEmpty() && image.loadFromData(m_avatar)) {
        QPixmap round(96, 96);
        round.fill(Qt::transparent);
        QPainter p(&round);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath circle;
        circle.addEllipse(0, 0, 96, 96);
        p.setClipPath(circle);
        p.drawImage(QRect(0, 0, 96, 96), image);
        p.end();
        m_picture->setPixmap(round);
    } else {
        m_picture->setPixmap(QPixmap());
        m_picture->setText(tr("No picture"));
    }
    m_remove->setEnabled(!m_avatar.isEmpty());
}

void ProfileDialog::save()
{
    QString error;
    if (!m_messenger->setMyProfile(m_name->text(), m_avatar, &error)) {
        m_message->setText(error);
        return;
    }
    accept();
}

GroupDialog::GroupDialog(Messenger *messenger, qint64 conversation, QWidget *parent)
    : QDialog(parent)
    , m_messenger(messenger)
    , m_conversation(conversation)
{
    const ChatStore::Group group = conversation ? m_messenger->group(conversation) : ChatStore::Group();
    const bool admin = conversation == 0 || group.admin == m_messenger->username();
    setWindowTitle(conversation == 0 ? tr("New Group") : tr("Group Members"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(conversation == 0
                                 ? tr("A group is kept by its members: the server sees only one-to-one messages. "
                                      "You will be its admin. At most %1 people.")
                                       .arg(Protocol::MaxGroupMembers)
                             : admin ? tr("You are the admin: change its name, add or take out people.")
                                     : tr("Only the admin (%1) can change the group.")
                                           .arg(m_messenger->displayName(group.admin).toHtmlEscaped()),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    m_name = new QLineEdit(group.name, this);
    m_name->setObjectName(QStringLiteral("groupName"));
    m_name->setMaxLength(Protocol::MaxDisplayName);
    m_name->setPlaceholderText(tr("The group's name"));
    m_name->setReadOnly(!admin);
    m_members = new QPlainTextEdit(this);
    m_members->setObjectName(QStringLiteral("groupMembers"));
    m_members->setPlaceholderText(tr("Their usernames: anna, bob, …"));
    QStringList others = group.members;
    others.removeAll(m_messenger->username());
    m_members->setPlainText(others.join(QStringLiteral(", ")));
    m_members->setReadOnly(!admin);
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Members:"), m_members);
    layout->addLayout(form);
    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    layout->addWidget(m_message);
    auto *buttons = new QDialogButtonBox(admin ? QDialogButtonBox::Cancel : QDialogButtonBox::Close, this);
    m_ok = buttons->addButton(conversation == 0 ? tr("Create") : tr("Save"), QDialogButtonBox::AcceptRole);
    m_ok->setVisible(admin);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &GroupDialog::apply);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    resize(440, 320);
}

QStringList GroupDialog::members() const
{
    static const QRegularExpression separators(QStringLiteral("[,;\\s]+"));
    QStringList out = m_members->toPlainText().split(separators, Qt::SkipEmptyParts);
    for (QString &name : out) {
        if (name.startsWith(QLatin1Char('@')))
            name.remove(0, 1);
    }
    return out;
}

void GroupDialog::apply()
{
    m_ok->setEnabled(false);
    m_message->setText(tr("Checking the people…"));
    QPointer<GroupDialog> self(this);
    const auto done = [self](qint64 conversation, const QString &error) {
        if (!self)
            return;
        self->m_ok->setEnabled(true);
        if (conversation == 0) {
            self->m_message->setText(error);
            return;
        }
        self->m_conversation = conversation;
        self->accept();
    };
    if (m_conversation == 0)
        m_messenger->createGroup(m_name->text(), members(), done);
    else
        m_messenger->changeGroup(m_conversation, m_name->text(), members(), done);
}

DevicesDialog::DevicesDialog(Messenger *messenger, QWidget *parent)
    : QDialog(parent)
    , m_messenger(messenger)
{
    setWindowTitle(tr("Linked Devices"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Your account on up to five computers, all with the same identity. A newly linked "
                                "computer starts empty: it gets what comes from then on, not the history."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("deviceList"));
    layout->addWidget(m_list);
    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    layout->addWidget(m_message);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *link = buttons->addButton(tr("Link New Device…"), QDialogButtonBox::ActionRole);
    m_remove = buttons->addButton(tr("Remove"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(link, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString code = QInputDialog::getText(
            this, tr("Link New Device"),
            tr("The code the new computer shows (only ever type a code from your own computer: it gets your account):"),
            QLineEdit::Normal, QString(), &ok);
        if (ok && !code.trimmed().isEmpty())
            linkWithCode(code);
    });
    connect(m_remove, &QPushButton::clicked, this, [this] { removeSelected(true); });
    connect(m_list, &QListWidget::currentRowChanged, this, &DevicesDialog::updateButtons);
    reload();
    resize(420, 340);
}

void DevicesDialog::reload()
{
    QPointer<DevicesDialog> self(this);
    m_messenger->listDevices([self](const QList<quint32> &devices, const QString &error) {
        if (!self)
            return;
        self->m_list->clear();
        if (!error.isEmpty()) {
            self->m_message->setText(error);
            return;
        }
        for (quint32 device : devices) {
            QString text = device == 1 ? tr("Computer %1 (the first)").arg(device) : tr("Computer %1").arg(device);
            if (device == self->m_messenger->device())
                text += tr(" · this one");
            auto *item = new QListWidgetItem(text, self->m_list);
            item->setData(Qt::UserRole, device);
        }
        self->updateButtons();
    });
}

void DevicesDialog::updateButtons()
{
    const QListWidgetItem *item = m_list->currentItem();
    const quint32 device = item ? item->data(Qt::UserRole).toUInt() : 0;
    const quint32 mine = m_messenger->device();
    // The first computer removes the others; any other removes itself; the first stays.
    m_remove->setEnabled(device > 1 && (mine == 1 || device == mine));
}

void DevicesDialog::linkWithCode(const QString &code)
{
    m_message->setText(tr("Sending your account to it…"));
    QPointer<DevicesDialog> self(this);
    m_messenger->linkDevice(code, [self](bool ok, const QString &error) {
        if (!self)
            return;
        self->m_message->setText(ok ? tr("Sent. The new computer is joining.") : error);
        if (ok)
            QTimer::singleShot(2500, self, &DevicesDialog::reload);
    });
}

void DevicesDialog::removeSelected(bool ask)
{
    const QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    const quint32 device = item->data(Qt::UserRole).toUInt();
    const bool self = device == m_messenger->device();
    if (ask && QMessageBox::question(this, tr("Remove"),
                                     self ? tr("Remove this computer from your account? It will no longer get your "
                                               "messages.")
                                          : tr("Remove computer %1 from your account? It will no longer get your "
                                               "messages.")
                                                .arg(device))
                   != QMessageBox::Yes)
        return;
    QPointer<DevicesDialog> dialog(this);
    m_messenger->removeDevice(device, [dialog](bool ok, const QString &error) {
        if (!dialog)
            return;
        dialog->m_message->setText(ok ? tr("Removed.") : error);
        dialog->reload();
    });
}
