#include "remotefilemanager/ssh/RemoteStorageCapabilityProbe.hpp"

#include <libssh/sftp.h>

#include <QSet>

namespace rfm::ssh
{
namespace
{

constexpr auto PosixMarker = "RFM_POSIX_STORAGE_V1";
constexpr auto WindowsMarker = "RFM_WINDOWS_STORAGE_V1";

QSet<QByteArray> outputLines(const QString& output)
{
    QSet<QByteArray> lines;
    for (const QString& line : output.split(QChar{'\n'})) {
        const QByteArray value = line.trimmed().toUtf8();
        if (!value.isEmpty()) {
            lines.insert(value);
        }
    }
    return lines;
}

bool completed(const rfm::core::VolumeCommandResult& result)
{
    return result.started && !result.timedOut && !result.crashed && !result.cancelled &&
           result.exitCode >= 0;
}

} // namespace

QString RemoteStorageCapabilityProbe::posixCommand()
{
    // The marker proves that a POSIX-compatible command interpreter executed
    // this fixed probe; tool names alone are never considered proof.
    return QStringLiteral(
        "printf '%s\\n' RFM_POSIX_STORAGE_V1; "
        "for rfm_tool in lsblk udisksctl mount umount; do "
        "command -v \"$rfm_tool\" >/dev/null 2>&1 && printf '%s\\n' \"$rfm_tool\"; done");
}

QString RemoteStorageCapabilityProbe::windowsPowerShellCommand()
{
    // Fixed, non-interactive and read-only. The marker is written only after
    // PowerShell itself has started. No server or user data is interpolated.
    return QStringLiteral(
        "powershell.exe -NoProfile -NonInteractive -Command \"Write-Output "
        "RFM_WINDOWS_STORAGE_V1; Write-Output powershell; if (Get-Command Get-Volume "
        "-ErrorAction SilentlyContinue) { Write-Output Get-Volume }; if (Get-Command Get-Disk "
        "-ErrorAction SilentlyContinue) { Write-Output Get-Disk }\"");
}

rfm::core::CapabilitySupport
RemoteStorageCapabilityProbe::mountInfoCapabilityFromSftpStatus(int sftpStatus, bool readSucceeded)
{
    if (readSucceeded) {
        return rfm::core::CapabilitySupport::Supported;
    }
    if (sftpStatus == SSH_FX_NO_SUCH_FILE || sftpStatus == SSH_FX_NO_SUCH_PATH) {
        return rfm::core::CapabilitySupport::Unsupported;
    }
    // Permission errors, transport loss, and incoherent SFTP statuses do not
    // prove that Linux mount information is unavailable.
    return rfm::core::CapabilitySupport::Unknown;
}

rfm::core::RemoteStorageCapabilities
RemoteStorageCapabilityProbe::applyPosixResult(rfm::core::RemoteStorageCapabilities capabilities,
                                               const rfm::core::VolumeCommandResult& result)
{
    if (!completed(result)) {
        return capabilities;
    }
    const QSet<QByteArray> lines = outputLines(result.standardOutput);
    if (!lines.contains(PosixMarker)) {
        return capabilities;
    }
    capabilities.lsblk = lines.contains(QByteArrayLiteral("lsblk"))
                             ? rfm::core::CapabilitySupport::Supported
                             : rfm::core::CapabilitySupport::Unsupported;
    return capabilities;
}

rfm::core::RemoteStorageCapabilities
RemoteStorageCapabilityProbe::applyWindowsResult(rfm::core::RemoteStorageCapabilities capabilities,
                                                 const rfm::core::VolumeCommandResult& result)
{
    if (!completed(result)) {
        return capabilities;
    }
    const QSet<QByteArray> lines = outputLines(result.standardOutput);
    if (!lines.contains(WindowsMarker)) {
        if (result.exitCode != 0) {
            capabilities.windowsPowerShell = rfm::core::CapabilitySupport::Unsupported;
        }
        return capabilities;
    }
    capabilities.windowsPowerShell = rfm::core::CapabilitySupport::Supported;
    capabilities.windowsGetVolume = lines.contains(QByteArrayLiteral("Get-Volume"))
                                        ? rfm::core::CapabilitySupport::Supported
                                        : rfm::core::CapabilitySupport::Unsupported;
    capabilities.windowsGetDisk = lines.contains(QByteArrayLiteral("Get-Disk"))
                                      ? rfm::core::CapabilitySupport::Supported
                                      : rfm::core::CapabilitySupport::Unsupported;
    return capabilities;
}

rfm::core::RemoteStorageCapabilities
RemoteStorageCapabilityProbe::finalized(rfm::core::RemoteStorageCapabilities capabilities)
{
    capabilities.detectionState = rfm::core::CapabilityDetectionState::Detected;
    capabilities.provider = rfm::core::selectRemoteStorageProvider(capabilities);
    return capabilities;
}

bool RemoteStorageCapabilityProbe::linuxScannerApplicable(
    const rfm::core::RemoteStorageCapabilities& capabilities)
{
    return capabilities.detectionState == rfm::core::CapabilityDetectionState::Detected &&
           capabilities.provider == rfm::core::RemoteStorageProvider::Linux &&
           capabilities.linuxMountInfo == rfm::core::CapabilitySupport::Supported;
}

RemoteStorageCapabilityProbeStage RemoteStorageCapabilityLifecycle::stage() const
{
    return m_stage;
}

const rfm::core::RemoteStorageCapabilities&
RemoteStorageCapabilityLifecycle::capabilities() const
{
    return m_capabilities;
}

void RemoteStorageCapabilityLifecycle::recordPosixResult(
    const rfm::core::VolumeCommandResult& result)
{
    if (m_stage != RemoteStorageCapabilityProbeStage::Posix) {
        return;
    }
    m_capabilities = RemoteStorageCapabilityProbe::applyPosixResult(m_capabilities, result);
    m_stage = RemoteStorageCapabilityProbeStage::WindowsPowerShell;
}

void RemoteStorageCapabilityLifecycle::skipPosix()
{
    if (m_stage == RemoteStorageCapabilityProbeStage::Posix) {
        m_stage = RemoteStorageCapabilityProbeStage::WindowsPowerShell;
    }
}

void RemoteStorageCapabilityLifecycle::recordWindowsResult(
    const rfm::core::VolumeCommandResult& result)
{
    if (m_stage != RemoteStorageCapabilityProbeStage::WindowsPowerShell) {
        return;
    }
    m_capabilities = RemoteStorageCapabilityProbe::applyWindowsResult(m_capabilities, result);
    m_stage = RemoteStorageCapabilityProbeStage::MountInfo;
}

void RemoteStorageCapabilityLifecycle::skipWindows()
{
    if (m_stage == RemoteStorageCapabilityProbeStage::WindowsPowerShell) {
        m_stage = RemoteStorageCapabilityProbeStage::MountInfo;
    }
}

void RemoteStorageCapabilityLifecycle::recordMountInfo(rfm::core::CapabilitySupport support)
{
    if (m_stage != RemoteStorageCapabilityProbeStage::MountInfo) {
        return;
    }
    m_capabilities.linuxMountInfo = support;
    m_capabilities = RemoteStorageCapabilityProbe::finalized(m_capabilities);
    m_stage = RemoteStorageCapabilityProbeStage::Complete;
}

void RemoteStorageCapabilityLifecycle::reset()
{
    m_capabilities = {};
    m_stage = RemoteStorageCapabilityProbeStage::Posix;
}

} // namespace rfm::ssh
