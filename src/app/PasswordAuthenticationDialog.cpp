#include "remotefilemanager/app/PasswordAuthenticationDialog.hpp"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace rfm::app
{

PasswordAuthenticationDialog::PasswordAuthenticationDialog(
    const rfm::core::ConnectionProfile& profile, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("passwordAuthenticationDialog"));
    setWindowTitle(tr("SSH authentication"));
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(380);

    auto* const layout = new QVBoxLayout(this);
    m_explanation = new QLabel(tr("Password authentication is required to continue."), this);
    m_explanation->setObjectName(QStringLiteral("authenticationPromptLabel"));
    m_explanation->setWordWrap(true);
    layout->addWidget(m_explanation);

    auto* const form = new QFormLayout;
    auto* const serverName = new QLabel(profile.effectiveDisplayName(), this);
    serverName->setObjectName(QStringLiteral("authenticationServerNameLabel"));
    serverName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Server name:"), serverName);
    auto* const identity =
        new QLabel(QStringLiteral("%1@%2").arg(profile.username, profile.host), this);
    identity->setObjectName(QStringLiteral("authenticationServerIdentityLabel"));
    identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Server:"), identity);
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setObjectName(QStringLiteral("authenticationPasswordEdit"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setClearButtonEnabled(false);
    m_passwordEdit->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData |
                                        Qt::ImhNoPredictiveText);
    form->addRow(tr("Password:"), m_passwordEdit);
    layout->addLayout(form);

    m_activityIndicator = new QProgressBar(this);
    m_activityIndicator->setObjectName(QStringLiteral("authenticationActivityIndicator"));
    m_activityIndicator->setRange(0, 0);
    m_activityIndicator->setTextVisible(false);
    m_activityIndicator->hide();
    layout->addWidget(m_activityIndicator);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("authenticationStatusLabel"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setProperty("authenticationError", true);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "QLabel[authenticationError=\"true\"] { color: palette(highlight); font-weight: bold; }"));
    m_statusLabel->hide();
    layout->addWidget(m_statusLabel);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    m_buttons->setObjectName(QStringLiteral("authenticationButtons"));
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(m_passwordEdit, &QLineEdit::textChanged, this, [this](const QString& value) {
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!m_authenticating && !value.isEmpty());
    });
    connect(m_buttons, &QDialogButtonBox::accepted, this,
            &PasswordAuthenticationDialog::requestAuthentication);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &PasswordAuthenticationDialog::reject);
    layout->addWidget(m_buttons);
    m_passwordEdit->setFocus();
}

PasswordAuthenticationDialog::~PasswordAuthenticationDialog() { clearPasswordEdit(); }

rfm::core::SecurePassword PasswordAuthenticationDialog::takePassword()
{
    QString secret = m_passwordEdit->text();
    rfm::core::SecurePassword password = rfm::core::SecurePassword::fromUtf16(secret);
    clearPasswordEdit(secret);
    return password;
}

bool PasswordAuthenticationDialog::isAuthenticating() const { return m_authenticating; }

void PasswordAuthenticationDialog::setAuthenticationMessage(const QString& message)
{
    if (m_explanation != nullptr && !message.trimmed().isEmpty()) {
        m_explanation->setText(message.trimmed());
    }
}

void PasswordAuthenticationDialog::showAuthenticationError(const QString& message)
{
    setAuthenticating(false);
    m_statusLabel->setText(message.trimmed().isEmpty() ? tr("Incorrect password. Please try again.")
                                                       : message.trimmed());
    m_statusLabel->show();
    m_passwordEdit->setFocus();
}

void PasswordAuthenticationDialog::authenticationSucceeded()
{
    clearPasswordEdit();
    QDialog::accept();
}

void PasswordAuthenticationDialog::connectionFailed()
{
    m_authenticating = false;
    clearPasswordEdit();
    QDialog::reject();
}

void PasswordAuthenticationDialog::reject()
{
    if (!m_authenticating) {
        clearPasswordEdit();
        QDialog::reject();
    }
}

void PasswordAuthenticationDialog::closeEvent(QCloseEvent* event)
{
    if (m_authenticating) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void PasswordAuthenticationDialog::requestAuthentication()
{
    if (m_authenticating || m_passwordEdit->text().isEmpty()) {
        return;
    }
    setAuthenticating(true);
    emit authenticationRequested();
}

void PasswordAuthenticationDialog::setAuthenticating(bool authenticating)
{
    m_authenticating = authenticating;
    m_passwordEdit->setEnabled(!authenticating);
    m_buttons->button(QDialogButtonBox::Ok)
        ->setEnabled(!authenticating && !m_passwordEdit->text().isEmpty());
    m_buttons->button(QDialogButtonBox::Cancel)->setEnabled(!authenticating);
    m_activityIndicator->setVisible(authenticating);
    if (authenticating) {
        m_statusLabel->hide();
    }
}

void PasswordAuthenticationDialog::clearPasswordEdit()
{
    if (m_passwordEdit == nullptr) {
        return;
    }
    QString secret = m_passwordEdit->text();
    clearPasswordEdit(secret);
}

void PasswordAuthenticationDialog::clearPasswordEdit(QString& extractedSecret)
{
    if (m_passwordEdit == nullptr || extractedSecret.isEmpty()) {
        return;
    }
    m_passwordEdit->setText(QString(extractedSecret.size(), QChar{'\0'}));
    m_passwordEdit->clear();
    extractedSecret.fill(QChar{'\0'});
    extractedSecret.clear();
}

} // namespace rfm::app
