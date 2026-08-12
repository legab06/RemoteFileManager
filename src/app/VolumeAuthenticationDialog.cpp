#include "remotefilemanager/app/VolumeAuthenticationDialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace rfm::app
{

VolumeAuthenticationDialog::VolumeAuthenticationDialog(QString server, QString device,
                                                       rfm::core::VolumeOperation operation,
                                                       QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("volumeAuthenticationDialog"));
    setWindowTitle(tr("Authentication required"));
    setWindowModality(Qt::WindowModal);

    auto* const layout = new QVBoxLayout(this);
    auto* const explanation =
        new QLabel(operation == rfm::core::VolumeOperation::Mount
                       ? tr("The server requires authentication to mount this volume.")
                       : tr("The server requires authentication to unmount this volume."),
                   this);
    explanation->setObjectName(QStringLiteral("authenticationExplanationLabel"));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    auto* const form = new QFormLayout;
    auto* const serverLabel = new QLabel(std::move(server), this);
    serverLabel->setObjectName(QStringLiteral("authenticationServerLabel"));
    serverLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Server:"), serverLabel);
    auto* const deviceLabel = new QLabel(std::move(device), this);
    deviceLabel->setObjectName(QStringLiteral("authenticationDeviceLabel"));
    deviceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Device:"), deviceLabel);
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setObjectName(QStringLiteral("volumeAuthenticationPasswordEdit"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setClearButtonEnabled(false);
    m_passwordEdit->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData |
                                        Qt::ImhNoPredictiveText);
    form->addRow(tr("Password:"), m_passwordEdit);
    layout->addLayout(form);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    m_buttons->setObjectName(QStringLiteral("volumeAuthenticationButtons"));
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Authenticate"));
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(m_passwordEdit, &QLineEdit::textChanged, this, [this](const QString& value) {
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!value.isEmpty());
    });
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    m_passwordEdit->setFocus();
}

VolumeAuthenticationDialog::~VolumeAuthenticationDialog() { clearPasswordEdit(); }

rfm::core::SecurePassword VolumeAuthenticationDialog::takePassword()
{
    QString secret = m_passwordEdit->text();
    rfm::core::SecurePassword password = rfm::core::SecurePassword::fromUtf16(secret);
    clearPasswordEdit(secret);
    return password;
}

void VolumeAuthenticationDialog::clearPasswordEdit()
{
    if (m_passwordEdit == nullptr) {
        return;
    }
    QString secret = m_passwordEdit->text();
    clearPasswordEdit(secret);
}

void VolumeAuthenticationDialog::clearPasswordEdit(QString& extractedSecret)
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
