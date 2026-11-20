#include "Dialogs.h"

#include "ChatStore.h"
#include "Messenger.h"
#include "Protocol.h"
#include "SafetyNumber.h"

#include <QBuffer>
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
#include <QPushButton>
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

    auto *buttons = new QDialogButtonBox(this);
    m_signUp = buttons->addButton(tr("Sign Up"), QDialogButtonBox::AcceptRole);
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
