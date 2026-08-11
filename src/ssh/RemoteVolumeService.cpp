#include "remotefilemanager/ssh/RemoteVolumeService.hpp"

#include <QSet>

namespace rfm::ssh
{

const RemoteLinuxVolumeCapabilities& RemoteLinuxVolumeCapabilityCache::value() const
{
    return m_capabilities;
}

void RemoteLinuxVolumeCapabilityCache::update(RemoteLinuxVolumeCapabilities capabilities)
{
    m_capabilities = capabilities;
}

void RemoteLinuxVolumeCapabilityCache::reset() { m_capabilities = {}; }

QString RemoteLinuxVolumeService::capabilityProbeCommand()
{
    // This is a fixed command. No UI or server metadata is interpolated.
    return QStringLiteral(
        "for rfm_tool in lsblk udisksctl mount umount; do "
        "command -v \"$rfm_tool\" >/dev/null 2>&1 && printf '%s\\n' \"$rfm_tool\"; done");
}

RemoteLinuxVolumeCapabilities
RemoteLinuxVolumeService::parseCapabilities(const QByteArray& standardOutput)
{
    QSet<QByteArray> tools;
    for (const QByteArray& line : standardOutput.split('\n')) {
        const QByteArray tool = line.trimmed();
        if (!tool.isEmpty()) {
            tools.insert(tool);
        }
    }
    return {true, tools.contains(QByteArrayLiteral("lsblk")),
            tools.contains(QByteArrayLiteral("udisksctl")),
            tools.contains(QByteArrayLiteral("mount")),
            tools.contains(QByteArrayLiteral("umount"))};
}

QString RemoteLinuxVolumeService::blockDeviceDiscoveryCommand()
{
    return QStringLiteral("LC_ALL=C lsblk --json --bytes --paths --output "
                          "PATH,NAME,PKNAME,TYPE,FSTYPE,LABEL,SIZE,MOUNTPOINTS,RO,RM,TRAN,MODEL");
}

std::optional<QString>
RemoteLinuxVolumeService::operationCommand(const rfm::core::VolumeOperationRequest& request,
                                           const RemoteLinuxVolumeCapabilities& capabilities,
                                           rfm::core::VolumeOperationResult* immediateResult)
{
    auto reject = [&request, immediateResult](rfm::core::VolumeOperationError error,
                                              const QString& detail) {
        if (immediateResult != nullptr) {
            *immediateResult = rfm::core::makeVolumeOperationResult(request, error, detail);
        }
        return std::optional<QString>{};
    };

    if (rfm::core::isProtectedVolumeOperation(request)) {
        return reject(rfm::core::VolumeOperationError::NotSupported,
                      QStringLiteral("RemoteFileManager never unmounts the system volume."));
    }
    if (!rfm::core::isSafeLinuxDevicePath(request.target.device)) {
        return reject(rfm::core::VolumeOperationError::DeviceNotFound,
                      QStringLiteral("The remote volume has no safe Linux device identifier."));
    }
    if (!capabilities.known) {
        return reject(rfm::core::VolumeOperationError::ConnectionLost,
                      QStringLiteral("Remote session capabilities are unavailable."));
    }

    const QString& device = request.target.device;
    if (capabilities.udisksctl) {
        const QString verb = request.operation == rfm::core::VolumeOperation::Mount
                                 ? QStringLiteral("mount")
                                 : QStringLiteral("unmount");
        return QStringLiteral("LC_ALL=C udisksctl %1 -b %2 --no-user-interaction")
            .arg(verb, device);
    }
    if (request.operation == rfm::core::VolumeOperation::Mount && capabilities.mount) {
        return QStringLiteral("LC_ALL=C mount -- %1").arg(device);
    }
    if (request.operation == rfm::core::VolumeOperation::Unmount && capabilities.umount) {
        return QStringLiteral("LC_ALL=C umount -- %1").arg(device);
    }
    return reject(rfm::core::VolumeOperationError::ToolUnavailable,
                  QStringLiteral("No supported remote volume tool is available."));
}

rfm::core::VolumeOperationResult
RemoteLinuxVolumeService::operationResult(const rfm::core::VolumeOperationRequest& request,
                                          const rfm::core::VolumeCommandResult& commandResult)
{
    const QString diagnostic = commandResult.standardError.trimmed().isEmpty()
                                   ? commandResult.standardOutput
                                   : commandResult.standardError;
    return rfm::core::makeVolumeOperationResult(
        request, rfm::core::volumeOperationErrorFromCommand(commandResult), diagnostic);
}

} // namespace rfm::ssh
