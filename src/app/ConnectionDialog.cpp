#include "remotefilemanager/app/ConnectionDialog.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

namespace rfm::app {

ConnectionDialog::ConnectionDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Secure SSH connection"));
    setModal(true);
    setMinimumWidth(430);

    auto* const layout = new QVBoxLayout(this);
    auto* const note = new QLabel(
        tr("Your SSH agent and existing keys are tried first. The server host key is always verified."),
        this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* const form = new QFormLayout;
    m_hostEdit = new QLineEdit(this);
    m_hostEdit->setObjectName(QStringLiteral("hostEdit"));
    m_hostEdit->setPlaceholderText(QStringLiteral("server.example.com"));
    m_userEdit = new QLineEdit(this);
    m_userEdit->setObjectName(QStringLiteral("usernameEdit"));
    m_portSpin = new QSpinBox(this);
    m_portSpin->setObjectName(QStringLiteral("portSpin"));
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_passwordCheck = new QCheckBox(tr("Allow password fallback"), this);
    m_passwordCheck->setObjectName(QStringLiteral("passwordFallbackCheck"));
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setObjectName(QStringLiteral("passwordEdit"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setEnabled(false);
    m_passwordEdit->setPlaceholderText(tr("Never stored"));
    form->addRow(tr("Host:"), m_hostEdit);
    form->addRow(tr("User:"), m_userEdit);
    form->addRow(tr("Port:"), m_portSpin);
    form->addRow({}, m_passwordCheck);
    form->addRow(tr("Password:"), m_passwordEdit);
    layout->addLayout(form);

    m_saveServerCheck =
        new QCheckBox(tr("Save this server for future connections"), this);
    m_saveServerCheck->setObjectName(QStringLiteral("saveServerCheck"));
    m_saveServerCheck->setChecked(false);
    layout->addWidget(m_saveServerCheck);

    m_activityIndicator = new QProgressBar(this);
    m_activityIndicator->setObjectName(QStringLiteral("connectionActivityIndicator"));
    m_activityIndicator->setRange(0, 0);
    m_activityIndicator->setTextVisible(false);
    m_activityIndicator->setVisible(false);
    layout->addWidget(m_activityIndicator);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("connectionStatusLabel"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "QLabel[connectionError=\"true\"] { color: palette(highlight); font-weight: bold; }"));
    m_statusLabel->setVisible(false);
    layout->addWidget(m_statusLabel);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this,
            &ConnectionDialog::requestConnection);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_hostEdit, &QLineEdit::textChanged, this, &ConnectionDialog::updateState);
    connect(m_userEdit, &QLineEdit::textChanged, this, &ConnectionDialog::updateState);
    connect(m_passwordCheck, &QCheckBox::toggled, m_passwordEdit, &QWidget::setEnabled);
    connect(m_passwordCheck, &QCheckBox::toggled, this, &ConnectionDialog::updateState);
    connect(m_passwordEdit, &QLineEdit::textChanged, this, &ConnectionDialog::updateState);
    updateState();
}

rfm::core::ConnectionProfile ConnectionDialog::profile() const
{
    return {m_displayName,
            m_hostEdit->text().trimmed(),
            m_userEdit->text().trimmed(),
            static_cast<quint16>(m_portSpin->value()),
            m_profileId,
            m_passwordCheck->isChecked()};
}

QString ConnectionDialog::password() const
{
    return m_passwordCheck->isChecked() ? m_passwordEdit->text() : QString{};
}

bool ConnectionDialog::saveServerRequested() const
{
    return !m_saveServerCheck->isHidden() && m_saveServerCheck->isChecked();
}

ConnectionDialog::State ConnectionDialog::state() const
{
    return m_state;
}

void ConnectionDialog::setProfile(const rfm::core::ConnectionProfile& profile)
{
    if (m_state == State::Connecting) {
        return;
    }
    m_profileId = profile.id;
    m_displayName = profile.displayName;
    m_hostEdit->setText(profile.host);
    m_userEdit->setText(profile.username);
    m_portSpin->setValue(profile.port == 0 ? 22 : profile.port);
    m_passwordCheck->setChecked(profile.allowPasswordFallback);
    m_saveServerCheck->setChecked(false);
    m_saveServerCheck->setVisible(profile.id.trimmed().isEmpty());
    updateState();
}

void ConnectionDialog::setConnecting(bool connecting)
{
    m_state = connecting ? State::Connecting : State::Idle;
    applyState();
}

void ConnectionDialog::showConnectionError(const QString& message)
{
    m_state = State::Error;
    m_statusLabel->setText(message.trimmed().isEmpty() ? tr("The SSH connection failed.")
                                                       : message.trimmed());
    applyState();
}

void ConnectionDialog::clearConnectionError()
{
    if (m_state != State::Connecting) {
        m_state = State::Idle;
        m_statusLabel->clear();
        applyState();
    }
}

void ConnectionDialog::connectionSucceeded()
{
    if (m_state != State::Connecting) {
        return;
    }
    m_passwordEdit->clear();
    QDialog::accept();
}

void ConnectionDialog::reject()
{
    if (m_state != State::Connecting) {
        QDialog::reject();
    }
}

void ConnectionDialog::closeEvent(QCloseEvent* event)
{
    if (m_state == State::Connecting) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void ConnectionDialog::updateState()
{
    const bool passwordValid = !m_passwordCheck->isChecked() || !m_passwordEdit->text().isEmpty();
    m_buttons->button(QDialogButtonBox::Ok)
        ->setEnabled(m_state != State::Connecting && profile().isValid() && passwordValid);
}

void ConnectionDialog::requestConnection()
{
    if (m_state == State::Connecting || !m_buttons->button(QDialogButtonBox::Ok)->isEnabled()) {
        return;
    }
    setConnecting(true);
    emit connectionRequested(profile(), password());
}

void ConnectionDialog::applyState()
{
    const bool connecting = m_state == State::Connecting;
    m_hostEdit->setEnabled(!connecting);
    m_userEdit->setEnabled(!connecting);
    m_portSpin->setEnabled(!connecting);
    m_passwordCheck->setEnabled(!connecting);
    m_passwordEdit->setEnabled(!connecting && m_passwordCheck->isChecked());
    m_saveServerCheck->setEnabled(!connecting);
    m_buttons->button(QDialogButtonBox::Cancel)->setEnabled(!connecting);
    m_activityIndicator->setVisible(connecting);
    if (connecting) {
        m_statusLabel->setText(tr("Connecting to %1…").arg(m_hostEdit->text().trimmed()));
    }
    m_statusLabel->setVisible(connecting || m_state == State::Error);
    m_statusLabel->setProperty("connectionError", m_state == State::Error);
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);
    updateState();
}

}  // namespace rfm::app
