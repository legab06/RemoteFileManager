#include "remotefilemanager/app/ConnectionDialog.hpp"
#include "remotefilemanager/app/ServerProfileForm.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace rfm::app
{

ConnectionDialog::ConnectionDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("New SSH connection"));
    setModal(true);
    setMinimumWidth(430);

    auto* const layout = new QVBoxLayout(this);
    auto* const note =
        new QLabel(tr("A configured private key, your SSH agent, and existing keys are tried "
                      "after the server host key has been verified."),
                   this);
    note->setWordWrap(true);
    layout->addWidget(note);

    m_profileForm = new ServerProfileForm(this);
    layout->addWidget(m_profileForm);

    m_saveServerCheck = new QCheckBox(tr("Save this server for future connections"), this);
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
    connect(m_buttons, &QDialogButtonBox::accepted, this, &ConnectionDialog::requestConnection);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_profileForm, &ServerProfileForm::validityChanged, this,
            &ConnectionDialog::updateState);
    updateState();
}

rfm::core::ConnectionProfile ConnectionDialog::profile() const
{
    return m_profileForm->profile(m_profileId);
}

bool ConnectionDialog::saveServerRequested() const
{
    return !m_saveServerCheck->isHidden() && m_saveServerCheck->isChecked();
}

ConnectionDialog::State ConnectionDialog::state() const { return m_state; }

void ConnectionDialog::setProfile(const rfm::core::ConnectionProfile& profile)
{
    if (m_state == State::Connecting) {
        return;
    }
    m_profileId = profile.id;
    m_profileForm->setProfile(profile);
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
    QDialog::accept();
}

void ConnectionDialog::connectionCancelled()
{
    if (m_state == State::Connecting) {
        m_state = State::Idle;
        m_statusLabel->clear();
        applyState();
    }
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
    m_buttons->button(QDialogButtonBox::Ok)
        ->setEnabled(m_state != State::Connecting && profile().isValid());
}

void ConnectionDialog::requestConnection()
{
    if (m_state == State::Connecting || !m_buttons->button(QDialogButtonBox::Ok)->isEnabled()) {
        return;
    }
    setConnecting(true);
    emit connectionRequested(profile());
}

void ConnectionDialog::applyState()
{
    const bool connecting = m_state == State::Connecting;
    m_profileForm->setEnabled(!connecting);
    m_saveServerCheck->setEnabled(!connecting);
    m_buttons->button(QDialogButtonBox::Cancel)->setEnabled(!connecting);
    m_activityIndicator->setVisible(connecting);
    if (connecting) {
        m_statusLabel->setText(tr("Connecting to %1…").arg(profile().host));
    }
    m_statusLabel->setVisible(connecting || m_state == State::Error);
    m_statusLabel->setProperty("connectionError", m_state == State::Error);
    m_statusLabel->style()->unpolish(m_statusLabel);
    m_statusLabel->style()->polish(m_statusLabel);
    updateState();
}

} // namespace rfm::app
