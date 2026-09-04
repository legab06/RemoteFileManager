#include "remotefilemanager/app/ServerProfileDialog.hpp"
#include "remotefilemanager/app/ServerProfileForm.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace rfm::app
{

ServerProfileDialog::ServerProfileDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Server profile"));
    setModal(true);
    setMinimumWidth(430);

    auto* const layout = new QVBoxLayout(this);
    auto* const note = new QLabel(
        tr("Connection settings and an optional private-key path are stored locally. Passwords, "
           "passphrases, and private-key contents are never saved."),
        this);
    note->setWordWrap(true);
    layout->addWidget(note);

    m_profileForm = new ServerProfileForm(this);
    layout->addWidget(m_profileForm);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save, this);
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_profileForm, &ServerProfileForm::validityChanged, this,
            &ServerProfileDialog::updateState);
    updateState();
}

rfm::core::ConnectionProfile ServerProfileDialog::profile() const
{
    return m_profileForm->profile(m_profileId);
}

void ServerProfileDialog::setProfile(const rfm::core::ConnectionProfile& profile)
{
    m_profileId = profile.id;
    m_profileForm->setProfile(profile);
    updateState();
}

void ServerProfileDialog::updateState()
{
    m_buttons->button(QDialogButtonBox::Save)->setEnabled(profile().isValid());
}

} // namespace rfm::app
