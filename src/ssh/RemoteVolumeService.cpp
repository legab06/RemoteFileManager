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

std::optional<QString> RemoteLinuxVolumeService::interactiveOperationCommand(
    const rfm::core::VolumeOperationRequest& request,
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
    if (!capabilities.udisksctl) {
        return reject(rfm::core::VolumeOperationError::ToolUnavailable,
                      QStringLiteral("Interactive authorization requires udisksctl."));
    }
    const QString verb = request.operation == rfm::core::VolumeOperation::Mount
                             ? QStringLiteral("mount")
                             : QStringLiteral("unmount");
    return QStringLiteral("LC_ALL=C udisksctl %1 -b %2").arg(verb, request.target.device);
}

rfm::core::VolumeOperationResult
RemoteLinuxVolumeService::operationResult(const rfm::core::VolumeOperationRequest& request,
                                          const rfm::core::VolumeCommandResult& commandResult)
{
    const QString diagnostic = commandResult.standardError.trimmed().isEmpty()
                                   ? commandResult.standardOutput
                                   : commandResult.standardError;
    const QString protocolDiagnostic =
        commandResult.standardError + QChar{'\n'} + commandResult.standardOutput;
    const bool failedCommand = commandResult.started && !commandResult.timedOut &&
                               !commandResult.crashed && commandResult.exitCode != 0;
    const bool canAuthenticate =
        failedCommand && (protocolDiagnostic.contains(QStringLiteral("NotAuthorizedCanObtain"),
                                                      Qt::CaseInsensitive) ||
                          protocolDiagnostic.contains(QStringLiteral("Authentication is required"),
                                                      Qt::CaseInsensitive));
    const bool authenticationFailed =
        failedCommand &&
        protocolDiagnostic.contains(QStringLiteral("authentication failed"), Qt::CaseInsensitive);
    const auto error = canAuthenticate ? rfm::core::VolumeOperationError::AuthenticationRequired
                       : authenticationFailed
                           ? rfm::core::VolumeOperationError::AuthenticationFailed
                           : rfm::core::volumeOperationErrorFromCommand(commandResult);
    return rfm::core::makeVolumeOperationResult(request, error, diagnostic);
}

RemotePolkitPromptEvent RemotePolkitPromptParser::consume(const QByteArray& output)
{
    if (output.isEmpty()) {
        return RemotePolkitPromptEvent::None;
    }
    QByteArray cleaned;
    cleaned.reserve(output.size());
    bool inEscapeSequence = false;
    for (const char character : output) {
        const auto byte = static_cast<unsigned char>(character);
        if (inEscapeSequence) {
            if (byte >= 0x40U && byte <= 0x7eU) {
                inEscapeSequence = false;
            }
            continue;
        }
        if (byte == 0x1bU) {
            inEscapeSequence = true;
            continue;
        }
        if (character == '\b') {
            if (!cleaned.isEmpty()) {
                cleaned.chop(1);
            }
            continue;
        }
        if (character != '\r') {
            cleaned.append(character);
        }
    }
    m_recentOutput.append(cleaned.toLower());
    constexpr qsizetype maximumProtocolWindow = 1024;
    if (m_recentOutput.size() > maximumProtocolWindow) {
        m_recentOutput.remove(0, m_recentOutput.size() - maximumProtocolWindow);
    }

    m_authenticationCompleted =
        m_authenticationCompleted || m_recentOutput.contains("authentication complete");
    m_permissionDenied = m_permissionDenied || m_recentOutput.contains("not authorized") ||
                         m_recentOutput.contains("permission denied");
    m_volumeBusy = m_volumeBusy || m_recentOutput.contains("target is busy") ||
                   m_recentOutput.contains("device is busy");
    const bool explicitFailure = m_recentOutput.contains("authentication failed") ||
                                 m_recentOutput.contains("authentication failure") ||
                                 m_recentOutput.contains("sorry, try again");
    const bool passwordPrompt = m_recentOutput.contains("password:");
    if (explicitFailure || (m_passwordSent && passwordPrompt && !m_authenticationCompleted)) {
        m_recentOutput.fill('\0');
        m_recentOutput.clear();
        return RemotePolkitPromptEvent::AuthenticationFailed;
    }
    if (!m_passwordSent && !m_passwordPromptSeen && passwordPrompt) {
        m_passwordPromptSeen = true;
        m_recentOutput.fill('\0');
        m_recentOutput.clear();
        return RemotePolkitPromptEvent::PasswordPrompt;
    }
    return RemotePolkitPromptEvent::None;
}

RemotePolkitPromptEvent RemotePolkitPromptParser::timedOut() const
{
    return m_passwordPromptSeen ? RemotePolkitPromptEvent::TimedOutAfterPrompt
                                : RemotePolkitPromptEvent::TimedOutBeforePrompt;
}

void RemotePolkitPromptParser::passwordSent()
{
    m_passwordSent = true;
    m_recentOutput.fill('\0');
    m_recentOutput.clear();
}

bool RemotePolkitPromptParser::authenticationCompleted() const { return m_authenticationCompleted; }

bool RemotePolkitPromptParser::permissionDenied() const { return m_permissionDenied; }

bool RemotePolkitPromptParser::volumeBusy() const { return m_volumeBusy; }

void RemotePolkitPromptParser::clear()
{
    m_recentOutput.fill('\0');
    m_recentOutput.clear();
    m_passwordSent = false;
    m_passwordPromptSeen = false;
    m_authenticationCompleted = false;
    m_permissionDenied = false;
    m_volumeBusy = false;
}

} // namespace rfm::ssh
