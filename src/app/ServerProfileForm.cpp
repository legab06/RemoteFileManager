#include "remotefilemanager/app/ServerProfileForm.hpp"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace rfm::app
{

ServerProfileForm::ServerProfileForm(QWidget* parent) : QWidget(parent)
{
    auto* const form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName(QStringLiteral("serverNameEdit"));
    m_nameEdit->setPlaceholderText(tr("Optional — for example Home server"));
    m_hostEdit = new QLineEdit(this);
    m_hostEdit->setObjectName(QStringLiteral("serverHostEdit"));
    m_hostEdit->setPlaceholderText(QStringLiteral("server.example.com"));
    m_userEdit = new QLineEdit(this);
    m_userEdit->setObjectName(QStringLiteral("serverUsernameEdit"));
    m_portSpin = new QSpinBox(this);
    m_portSpin->setObjectName(QStringLiteral("serverPortSpin"));
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_keyModeRadio = new QRadioButton(tr("SSH key / agent"), this);
    m_keyModeRadio->setObjectName(QStringLiteral("keyOrAgentRadio"));
    m_keyModeRadio->setChecked(true);
    m_passwordOnlyRadio = new QRadioButton(tr("Password only"), this);
    m_passwordOnlyRadio->setObjectName(QStringLiteral("passwordOnlyRadio"));
    m_privateKeyEdit = new QLineEdit(this);
    m_privateKeyEdit->setObjectName(QStringLiteral("privateKeyPathEdit"));
    m_privateKeyEdit->setPlaceholderText(QStringLiteral("~/.ssh/id_ed25519"));
    m_privateKeyEdit->setToolTip(
        tr("Only the path is saved. Passphrase-protected files must be loaded in an SSH agent."));
    m_privateKeyBrowseButton = new QPushButton(tr("Browse…"), this);
    m_privateKeyBrowseButton->setObjectName(QStringLiteral("privateKeyBrowseButton"));
    m_passwordCheck = new QCheckBox(tr("Allow password authentication if key fails"), this);
    m_passwordCheck->setObjectName(QStringLiteral("passwordAuthenticationCheck"));

    auto* const privateKeyRow = new QWidget(this);
    auto* const privateKeyLayout = new QHBoxLayout(privateKeyRow);
    privateKeyLayout->setContentsMargins(0, 0, 0, 0);
    privateKeyLayout->addWidget(m_privateKeyEdit);
    privateKeyLayout->addWidget(m_privateKeyBrowseButton);

    form->addRow(tr("Name:"), m_nameEdit);
    form->addRow(tr("Host:"), m_hostEdit);
    form->addRow(tr("User:"), m_userEdit);
    form->addRow(tr("Port:"), m_portSpin);
    auto* const authenticationBox = new QGroupBox(tr("Authentication"), this);
    auto* const authenticationLayout = new QVBoxLayout(authenticationBox);
    authenticationLayout->addWidget(m_keyModeRadio);
    auto* const privateKeyForm = new QFormLayout;
    privateKeyForm->setContentsMargins(20, 0, 0, 0);
    privateKeyForm->addRow(tr("Private key:"), privateKeyRow);
    authenticationLayout->addLayout(privateKeyForm);
    authenticationLayout->addWidget(m_passwordCheck);
    authenticationLayout->addWidget(m_passwordOnlyRadio);
    form->addRow(authenticationBox);

    connect(m_hostEdit, &QLineEdit::textChanged, this, &ServerProfileForm::publishValidity);
    connect(m_userEdit, &QLineEdit::textChanged, this, &ServerProfileForm::publishValidity);
    connect(m_privateKeyBrowseButton, &QPushButton::clicked, this,
            &ServerProfileForm::browseForPrivateKey);
    connect(m_keyModeRadio, &QRadioButton::toggled, this,
            &ServerProfileForm::updateAuthenticationControls);
    updateAuthenticationControls();
}

rfm::core::ConnectionProfile ServerProfileForm::profile(const QString& id) const
{
    const bool keyOrAgent = m_keyModeRadio->isChecked();
    return {m_nameEdit->text().trimmed(),
            m_hostEdit->text().trimmed(),
            m_userEdit->text().trimmed(),
            static_cast<quint16>(m_portSpin->value()),
            id,
            keyOrAgent && m_passwordCheck->isChecked(),
            keyOrAgent ? m_privateKeyEdit->text().trimmed() : QString{},
            keyOrAgent ? rfm::core::AuthenticationMode::KeyOrAgent
                       : rfm::core::AuthenticationMode::PasswordOnly};
}

void ServerProfileForm::setProfile(const rfm::core::ConnectionProfile& profile)
{
    m_nameEdit->setText(profile.displayName);
    m_hostEdit->setText(profile.host);
    m_userEdit->setText(profile.username);
    m_portSpin->setValue(profile.port == 0 ? 22 : profile.port);
    m_keyModeRadio->setChecked(profile.authenticationMode ==
                               rfm::core::AuthenticationMode::KeyOrAgent);
    m_passwordOnlyRadio->setChecked(profile.authenticationMode ==
                                    rfm::core::AuthenticationMode::PasswordOnly);
    m_privateKeyEdit->setText(profile.privateKeyPath);
    m_passwordCheck->setChecked(profile.allowPasswordAuthentication);
    updateAuthenticationControls();
    publishValidity();
}

void ServerProfileForm::publishValidity() { emit validityChanged(profile().isValid()); }

void ServerProfileForm::updateAuthenticationControls()
{
    const bool keyOrAgent = m_keyModeRadio->isChecked();
    m_privateKeyEdit->setEnabled(keyOrAgent);
    m_privateKeyBrowseButton->setEnabled(keyOrAgent);
    m_passwordCheck->setEnabled(keyOrAgent);
    if (!keyOrAgent) {
        m_passwordCheck->setChecked(false);
    }
}

void ServerProfileForm::browseForPrivateKey()
{
    QString initialPath = m_privateKeyEdit->text().trimmed();
    if (initialPath == QStringLiteral("~")) {
        initialPath = QDir::homePath();
    } else if (initialPath.startsWith(QStringLiteral("~/"))) {
        initialPath = QDir(QDir::homePath()).filePath(initialPath.sliced(2));
    }
    if (initialPath.isEmpty()) {
        initialPath = QDir(QDir::homePath()).filePath(QStringLiteral(".ssh"));
    }

    auto* const dialog = new QFileDialog(this, tr("Select SSH private key"), initialPath);
    dialog->setObjectName(QStringLiteral("privateKeyFileDialog"));
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setOption(QFileDialog::DontConfirmOverwrite);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QFileDialog::fileSelected, m_privateKeyEdit, &QLineEdit::setText);
    dialog->open();
}

} // namespace rfm::app
