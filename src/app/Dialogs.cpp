#include "Dialogs.h"

#include "Messenger.h"
#include "Protocol.h"
#include "SafetyNumber.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
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
