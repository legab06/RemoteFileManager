#include "remotefilemanager/app/ServerProfileDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace rfm::app
{

ServerProfileDialog::ServerProfileDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Server profile"));
    setModal(true);
    setMinimumWidth(430);

    auto* const layout = new QVBoxLayout(this);
    auto* const note = new QLabel(
        tr("Connection settings are stored locally. Passwords and private keys are never saved."),
        this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* const form = new QFormLayout;
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName(QStringLiteral("profileNameEdit"));
    m_nameEdit->setPlaceholderText(tr("Optional — for example Home server"));
    m_hostEdit = new QLineEdit(this);
    m_hostEdit->setObjectName(QStringLiteral("profileHostEdit"));
    m_hostEdit->setPlaceholderText(QStringLiteral("server.example.com"));
    m_userEdit = new QLineEdit(this);
    m_userEdit->setObjectName(QStringLiteral("profileUsernameEdit"));
    m_portSpin = new QSpinBox(this);
    m_portSpin->setObjectName(QStringLiteral("profilePortSpin"));
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_passwordCheck = new QCheckBox(tr("Allow password fallback"), this);
    m_passwordCheck->setObjectName(QStringLiteral("profilePasswordFallbackCheck"));
    form->addRow(tr("Name:"), m_nameEdit);
    form->addRow(tr("Host:"), m_hostEdit);
    form->addRow(tr("User:"), m_userEdit);
    form->addRow(tr("Port:"), m_portSpin);
    form->addRow({}, m_passwordCheck);
    layout->addLayout(form);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save, this);
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_hostEdit, &QLineEdit::textChanged, this, &ServerProfileDialog::updateState);
    connect(m_userEdit, &QLineEdit::textChanged, this, &ServerProfileDialog::updateState);
    updateState();
}

rfm::core::ConnectionProfile ServerProfileDialog::profile() const
{
    return {m_nameEdit->text().trimmed(),
            m_hostEdit->text().trimmed(),
            m_userEdit->text().trimmed(),
            static_cast<quint16>(m_portSpin->value()),
            m_profileId,
            m_passwordCheck->isChecked()};
}

void ServerProfileDialog::setProfile(const rfm::core::ConnectionProfile& profile)
{
    m_profileId = profile.id;
    m_nameEdit->setText(profile.displayName);
    m_hostEdit->setText(profile.host);
    m_userEdit->setText(profile.username);
    m_portSpin->setValue(profile.port == 0 ? 22 : profile.port);
    m_passwordCheck->setChecked(profile.allowPasswordFallback);
    updateState();
}

void ServerProfileDialog::updateState()
{
    m_buttons->button(QDialogButtonBox::Save)->setEnabled(profile().isValid());
}

} // namespace rfm::app
