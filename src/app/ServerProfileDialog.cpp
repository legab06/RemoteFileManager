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
    m_copyDataBackendStatus = new QLabel(remoteCopy);
    m_copyDataBackendStatus->setObjectName(QStringLiteral("copyDataBackendStatusLabel"));
    m_nativeCopyStatus = new QLabel(remoteCopy);
    m_nativeCopyStatus->setObjectName(QStringLiteral("nativeCopyStatusLabel"));
    m_effectiveCopyMethod = new QLabel(remoteCopy);
    m_effectiveCopyMethod->setObjectName(QStringLiteral("effectiveCopyMethodLabel"));
    auto* const protocol = new QLabel(QStringLiteral("copy-data v1"), remoteCopy);
    protocol->setObjectName(QStringLiteral("copyDataProtocolLabel"));
    remoteCopyLayout->addRow(tr("SFTP copy-data — Server support:"), m_copyDataStatus);
    remoteCopyLayout->addRow(tr("SFTP copy-data — RFM backend support:"), m_copyDataBackendStatus);
    remoteCopyLayout->addRow(tr("Native server copy — POSIX cp:"), m_nativeCopyStatus);
    remoteCopyLayout->addRow(tr("Effective method:"), m_effectiveCopyMethod);
    remoteCopyLayout->addRow(tr("SFTP extension:"), protocol);
    m_copyDataDescription = new QLabel(remoteCopy);
    m_copyDataDescription->setObjectName(QStringLiteral("copyDataDescriptionLabel"));
    m_copyDataDescription->setWordWrap(true);
    remoteCopyLayout->addRow(m_copyDataDescription);
    capabilitiesLayout->addWidget(remoteCopy);

    auto* const storage = new QGroupBox(tr("Storage"), capabilitiesTab);
    auto* const storageLayout = new QFormLayout(storage);
    m_storageDiscoveryStatus = new QLabel(storage);
    m_storageDiscoveryStatus->setObjectName(QStringLiteral("storageDiscoveryStatusLabel"));
    m_storageVolumeListingStatus = new QLabel(storage);
    m_storageVolumeListingStatus->setObjectName(
        QStringLiteral("storageVolumeListingStatusLabel"));
    m_storageProvider = new QLabel(storage);
    m_storageProvider->setObjectName(QStringLiteral("storageProviderLabel"));
    m_linuxMountInfo = new QLabel(storage);
    m_linuxMountInfo->setObjectName(QStringLiteral("linuxMountInfoStatusLabel"));
    m_lsblk = new QLabel(storage);
    m_lsblk->setObjectName(QStringLiteral("lsblkStatusLabel"));
    m_windowsPowerShell = new QLabel(storage);
    m_windowsPowerShell->setObjectName(QStringLiteral("windowsPowerShellStatusLabel"));
    m_windowsGetVolume = new QLabel(storage);
    m_windowsGetVolume->setObjectName(QStringLiteral("windowsGetVolumeStatusLabel"));
    m_windowsGetDisk = new QLabel(storage);
    m_windowsGetDisk->setObjectName(QStringLiteral("windowsGetDiskStatusLabel"));
    storageLayout->addRow(tr("Provider detection:"), m_storageDiscoveryStatus);
    storageLayout->addRow(tr("Volume listing:"), m_storageVolumeListingStatus);
    storageLayout->addRow(tr("Provider:"), m_storageProvider);
    storageLayout->addRow(tr("Linux mount information:"), m_linuxMountInfo);
    storageLayout->addRow(tr("lsblk:"), m_lsblk);
    storageLayout->addRow(tr("PowerShell:"), m_windowsPowerShell);
    storageLayout->addRow(tr("Get-Volume:"), m_windowsGetVolume);
    storageLayout->addRow(tr("Get-Disk:"), m_windowsGetDisk);
    capabilitiesLayout->addWidget(storage);

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

void ServerProfileDialog::setServerCapabilities(
    const rfm::core::ServerCapabilities& capabilities, bool currentlyConnected,
    std::optional<rfm::core::RemoteCopyExecutionCapabilities> remoteCopyExecutionCapabilities)
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

    const bool runtimeCopyDetected =
        detected && currentlyConnected && remoteCopyExecutionCapabilities.has_value();
    m_copyDataBackendStatus->setText(!runtimeCopyDetected
                                         ? tr("Not detected")
                                         : (remoteCopyExecutionCapabilities->sftpCopyDataAvailable
                                                ? tr("Supported")
                                                : tr("Not supported")));

    const auto nativeCopyStatus = [&]() {
        if (!runtimeCopyDetected) {
            return tr("Not detected");
        }
        switch (remoteCopyExecutionCapabilities->nativeServerCopy) {
        case rfm::core::CapabilitySupport::Supported:
            return remoteCopyExecutionCapabilities->nativePrimitive ==
                           rfm::core::NativeServerCopyPrimitive::PosixCp
                       ? tr("Supported")
                       : tr("Unknown");
        case rfm::core::CapabilitySupport::Unsupported:
            return tr("Not supported");
        case rfm::core::CapabilitySupport::Unknown:
            return tr("Unknown");
        }
        return tr("Unknown");
    };
    m_nativeCopyStatus->setText(nativeCopyStatus());

    if (!runtimeCopyDetected) {
        m_effectiveCopyMethod->setText(tr("Not detected"));
    } else {
        const auto method =
            rfm::core::selectRemoteCopyMethod(capabilities, *remoteCopyExecutionCapabilities);
        switch (method) {
        case rfm::core::RemoteCopyMethod::SftpCopyData:
            m_effectiveCopyMethod->setText(tr("SFTP copy-data"));
            break;
        case rfm::core::RemoteCopyMethod::NativeServerCopy:
            m_effectiveCopyMethod->setText(remoteCopyExecutionCapabilities->nativePrimitive ==
                                                   rfm::core::NativeServerCopyPrimitive::PosixCp
                                               ? tr("Native server copy (POSIX cp)")
                                               : tr("Unknown"));
            break;
        case rfm::core::RemoteCopyMethod::ClientMediatedSftp:
            m_effectiveCopyMethod->setText(tr("Client-mediated SFTP"));
            break;
        }
    }

    QString copyDataDescription;
    if (!detected) {
        copyDataDescription =
            tr("Connect to this server to detect the SFTP features it advertises.");
    } else if (!runtimeCopyDetected) {
        copyDataDescription =
            copyDataSupported
                ? tr("The server advertises SFTP copy-data. RFM backend support and the "
                     "effective method are available only for the current session.")
                : tr("The server does not advertise SFTP copy-data. RFM may use another "
                     "copy strategy.");
    } else if (copyDataSupported && !remoteCopyExecutionCapabilities->sftpCopyDataAvailable &&
               remoteCopyExecutionCapabilities->nativeServerCopy ==
                   rfm::core::CapabilitySupport::Supported &&
               remoteCopyExecutionCapabilities->nativePrimitive ==
                   rfm::core::NativeServerCopyPrimitive::PosixCp) {
        copyDataDescription =
            tr("The server advertises SFTP copy-data, but the current RFM/libssh backend "
               "cannot invoke it. RFM uses the verified native POSIX copy method instead.");
    } else if (copyDataSupported && !remoteCopyExecutionCapabilities->sftpCopyDataAvailable) {
        copyDataDescription =
            tr("The server advertises SFTP copy-data, but the current RFM/libssh backend "
               "cannot invoke it.");
    } else if (copyDataSupported) {
        copyDataDescription =
            tr("The server advertises SFTP copy-data and the current backend can invoke it.");
    } else {
        copyDataDescription =
            tr("The server does not advertise SFTP copy-data. RFM uses the effective method "
               "shown above.");
    }
    m_copyDataDescription->setText(copyDataDescription);

    const auto storageStatus = [this, &capabilities](rfm::core::CapabilitySupport support,
                                                     bool otherProvider) {
        if (capabilities.storage.detectionState != rfm::core::CapabilityDetectionState::Detected) {
            return tr("Not detected");
        }
        if (otherProvider) {
            return tr("Not applicable");
        }
        switch (support) {
        case rfm::core::CapabilitySupport::Supported:
            return tr("Supported");
        case rfm::core::CapabilitySupport::Unsupported:
            return tr("Not supported");
        case rfm::core::CapabilitySupport::Unknown:
            return tr("Unknown");
        }
        return tr("Unknown");
    };
    const auto& storageCapabilities = capabilities.storage;
    const bool storageDetected =
        storageCapabilities.detectionState == rfm::core::CapabilityDetectionState::Detected;
    const bool linuxNotApplicable =
        storageCapabilities.provider == rfm::core::RemoteStorageProvider::WindowsPowerShell;
    const bool windowsNotApplicable =
        storageCapabilities.provider == rfm::core::RemoteStorageProvider::Linux;
    m_storageDiscoveryStatus->setText(storageDetected ? tr("Completed") : tr("Not detected"));
    m_storageVolumeListingStatus->setText(
        !storageDetected
            ? tr("Not detected")
            : (storageCapabilities.provider == rfm::core::RemoteStorageProvider::Linux
                   ? tr("Supported")
                   : (storageCapabilities.provider ==
                              rfm::core::RemoteStorageProvider::WindowsPowerShell
                          ? tr("Not implemented")
                          : tr("Not supported"))));
    switch (storageCapabilities.provider) {
    case rfm::core::RemoteStorageProvider::Linux:
        m_storageProvider->setText(tr("Linux"));
        break;
    case rfm::core::RemoteStorageProvider::WindowsPowerShell:
        m_storageProvider->setText(tr("Windows PowerShell"));
        break;
    case rfm::core::RemoteStorageProvider::None:
        m_storageProvider->setText(storageDetected ? tr("Unavailable") : tr("Not detected"));
        break;
    }
    m_linuxMountInfo->setText(
        storageStatus(storageCapabilities.linuxMountInfo, linuxNotApplicable));
    m_lsblk->setText(storageStatus(storageCapabilities.lsblk, linuxNotApplicable));
    m_windowsPowerShell->setText(
        storageStatus(storageCapabilities.windowsPowerShell, windowsNotApplicable));
    m_windowsGetVolume->setText(
        storageStatus(storageCapabilities.windowsGetVolume, windowsNotApplicable));
    m_windowsGetDisk->setText(
        storageStatus(storageCapabilities.windowsGetDisk, windowsNotApplicable));

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
