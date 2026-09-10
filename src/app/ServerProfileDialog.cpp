#include "remotefilemanager/app/ServerProfileDialog.hpp"
#include "remotefilemanager/app/ServerProfileForm.hpp"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace rfm::app
{

ServerProfileDialog::ServerProfileDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Server profile"));
    setModal(true);
    setMinimumWidth(430);

    auto* const layout = new QVBoxLayout(this);
    auto* const tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("serverProfileTabs"));
    auto* const generalTab = new QWidget(tabs);
    generalTab->setObjectName(QStringLiteral("serverProfileGeneralTab"));
    auto* const generalLayout = new QVBoxLayout(generalTab);
    auto* const note = new QLabel(
        tr("Connection settings and an optional private-key path are stored locally. Passwords, "
           "passphrases, and private-key contents are never saved."),
        generalTab);
    note->setWordWrap(true);
    generalLayout->addWidget(note);

    m_profileForm = new ServerProfileForm(generalTab);
    generalLayout->addWidget(m_profileForm);
    generalLayout->addStretch();
    tabs->addTab(generalTab, tr("General"));

    auto* const capabilitiesTab = new QWidget(tabs);
    capabilitiesTab->setObjectName(QStringLiteral("serverProfileCapabilitiesTab"));
    auto* const capabilitiesLayout = new QVBoxLayout(capabilitiesTab);

    auto* const serverInformation = new QGroupBox(tr("Server information"), capabilitiesTab);
    auto* const serverInformationLayout = new QFormLayout(serverInformation);
    m_sftpProtocolVersion = new QLabel(serverInformation);
    m_sftpProtocolVersion->setObjectName(QStringLiteral("sftpProtocolVersionLabel"));
    m_lastDetected = new QLabel(serverInformation);
    m_lastDetected->setObjectName(QStringLiteral("capabilitiesLastDetectedLabel"));
    serverInformationLayout->addRow(tr("SFTP protocol version:"), m_sftpProtocolVersion);
    serverInformationLayout->addRow(tr("Last detected:"), m_lastDetected);
    capabilitiesLayout->addWidget(serverInformation);

    m_capabilitiesContext = new QLabel(capabilitiesTab);
    m_capabilitiesContext->setObjectName(QStringLiteral("capabilitiesContextLabel"));
    m_capabilitiesContext->setWordWrap(true);
    capabilitiesLayout->addWidget(m_capabilitiesContext);

    auto* const remoteCopy = new QGroupBox(tr("Remote copy"), capabilitiesTab);
    auto* const remoteCopyLayout = new QFormLayout(remoteCopy);
    m_copyDataStatus = new QLabel(remoteCopy);
    m_copyDataStatus->setObjectName(QStringLiteral("copyDataStatusLabel"));
    auto* const protocol = new QLabel(QStringLiteral("copy-data v1"), remoteCopy);
    protocol->setObjectName(QStringLiteral("copyDataProtocolLabel"));
    remoteCopyLayout->addRow(tr("Server-side SFTP copy:"), m_copyDataStatus);
    remoteCopyLayout->addRow(tr("SFTP extension:"), protocol);
    m_copyDataDescription = new QLabel(remoteCopy);
    m_copyDataDescription->setObjectName(QStringLiteral("copyDataDescriptionLabel"));
    m_copyDataDescription->setWordWrap(true);
    remoteCopyLayout->addRow(m_copyDataDescription);
    capabilitiesLayout->addWidget(remoteCopy);

    auto* const extensions = new QGroupBox(tr("SFTP extensions"), capabilitiesTab);
    auto* const extensionsLayout = new QVBoxLayout(extensions);
    m_sftpExtensions = new QTableWidget(extensions);
    m_sftpExtensions->setObjectName(QStringLiteral("sftpExtensionsTable"));
    m_sftpExtensions->setColumnCount(2);
    m_sftpExtensions->setHorizontalHeaderLabels({tr("Extension"), tr("Data / revision")});
    m_sftpExtensions->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_sftpExtensions->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_sftpExtensions->verticalHeader()->setVisible(false);
    m_sftpExtensions->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sftpExtensions->setSelectionBehavior(QAbstractItemView::SelectRows);
    extensionsLayout->addWidget(m_sftpExtensions);
    capabilitiesLayout->addWidget(extensions, 1);

    tabs->addTab(capabilitiesTab, tr("Capabilities"));
    layout->addWidget(tabs);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save, this);
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_profileForm, &ServerProfileForm::validityChanged, this,
            &ServerProfileDialog::updateState);
    clearServerCapabilities();
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
    clearServerCapabilities();
    updateState();
}

void ServerProfileDialog::setServerCapabilities(const rfm::core::ServerCapabilities& capabilities,
                                                bool currentlyConnected)
{
    const bool detected =
        capabilities.detectionState == rfm::core::CapabilityDetectionState::Detected;
    m_sftpProtocolVersion->setText(detected
                                       ? (capabilities.sftpProtocolVersion.has_value()
                                              ? QString::number(*capabilities.sftpProtocolVersion)
                                              : tr("Unknown"))
                                       : tr("Not detected"));
    m_lastDetected->setText(
        detected && capabilities.detectedAt.isValid()
            ? QLocale().toString(capabilities.detectedAt.toLocalTime(), QLocale::ShortFormat)
            : tr("Not detected"));
    m_capabilitiesContext->setText(
        !detected
            ? tr("Not detected — Connect to this server to detect its SFTP capabilities.")
            : (currentlyConnected
                   ? tr("Currently detected — These capabilities were detected for the current "
                        "connection.")
                   : tr("Last known — These capabilities were detected during an earlier "
                        "connection and are not currently verified.")));

    const bool copyDataSupported =
        detected && capabilities.copyDataVersion1 == rfm::core::CapabilitySupport::Supported;
    m_copyDataStatus->setText(!detected || capabilities.copyDataVersion1 ==
                                               rfm::core::CapabilitySupport::Unknown
                                  ? tr("Not detected")
                                  : (copyDataSupported ? tr("Supported") : tr("Not supported")));
    m_copyDataDescription->setText(
        !detected
            ? tr("Connect to this server to detect the SFTP features it advertises.")
            : (copyDataSupported
                   ? tr("The server supports server-side SFTP copying. Remote file data can be "
                        "copied without passing through this computer.")
                   : tr("The server does not advertise SFTP server-side copying. RFM may use "
                        "another copy strategy.")));

    const qsizetype extensionCount =
        detected ? std::min(capabilities.sftpExtensions.size(),
                            static_cast<qsizetype>(std::numeric_limits<int>::max()))
                 : 0;
    m_sftpExtensions->setRowCount(static_cast<int>(extensionCount));
    if (!detected) {
        return;
    }
    for (qsizetype row = 0; row < extensionCount; ++row) {
        const auto& extension = capabilities.sftpExtensions.at(row);
        auto* const name = new QTableWidgetItem(extension.name);
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        auto* const data = new QTableWidgetItem(extension.data);
        data->setFlags(data->flags() & ~Qt::ItemIsEditable);
        m_sftpExtensions->setItem(static_cast<int>(row), 0, name);
        m_sftpExtensions->setItem(static_cast<int>(row), 1, data);
    }
}

void ServerProfileDialog::clearServerCapabilities() { setServerCapabilities({}); }

void ServerProfileDialog::updateState()
{
    m_buttons->button(QDialogButtonBox::Save)->setEnabled(profile().isValid());
}

} // namespace rfm::app
