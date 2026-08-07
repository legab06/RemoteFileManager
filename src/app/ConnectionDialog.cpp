#include "remotefilemanager/app/ConnectionDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
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

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Connect"));
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
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
    return {{}, m_hostEdit->text().trimmed(), m_userEdit->text().trimmed(),
            static_cast<quint16>(m_portSpin->value())};
}

QString ConnectionDialog::password() const
{
    return m_passwordCheck->isChecked() ? m_passwordEdit->text() : QString{};
}

void ConnectionDialog::updateState()
{
    const bool passwordValid = !m_passwordCheck->isChecked() || !m_passwordEdit->text().isEmpty();
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(profile().isValid() && passwordValid);
}

}  // namespace rfm::app
