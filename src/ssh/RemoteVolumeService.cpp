#include "remotefilemanager/ssh/RemoteVolumeService.hpp"

#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"

#include <QSet>

#include <cctype>

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
RemoteLinuxVolumeService::unmountTopologyCommand(const rfm::core::VolumeOperationRequest& request,
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
    if (rfm::core::volumeUnmountTargetMode(request) ==
        rfm::core::VolumeUnmountTargetMode::Invalid) {
        return reject(rfm::core::VolumeOperationError::DeviceNotFound,
                      QStringLiteral("The remote volume snapshot is inconsistent."));
    }
    if (!capabilities.known) {
        return reject(rfm::core::VolumeOperationError::ConnectionLost,
                      QStringLiteral("Remote session capabilities are unavailable."));
    }
    if (!capabilities.lsblk) {
        return reject(rfm::core::VolumeOperationError::ToolUnavailable,
                      QStringLiteral("The current volume attachment topology cannot be verified."));
    }
    const QString device = RemoteCopyCommand::quoteArgument(request.target.device);
    if (device.isEmpty()) {
        return reject(rfm::core::VolumeOperationError::DeviceNotFound,
                      QStringLiteral("The remote volume has no safe Linux device identifier."));
    }
    return QStringLiteral("LC_ALL=C lsblk --json --paths --output PATH,MOUNTPOINTS -- %1")
        .arg(device);
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
    const rfm::core::VolumeUnmountTargetMode unmountTargetMode =
        rfm::core::volumeUnmountTargetMode(request);
    if (request.operation == rfm::core::VolumeOperation::Unmount &&
        unmountTargetMode == rfm::core::VolumeUnmountTargetMode::Invalid) {
        return reject(rfm::core::VolumeOperationError::DeviceNotFound,
                      QStringLiteral("The remote volume has no safe, consistent mount point."));
    }
    if (!capabilities.known) {
        return reject(rfm::core::VolumeOperationError::ConnectionLost,
                      QStringLiteral("Remote session capabilities are unavailable."));
    }

    const QString& device = request.target.device;
    const bool targetedUnmount =
        request.operation == rfm::core::VolumeOperation::Unmount &&
        unmountTargetMode == rfm::core::VolumeUnmountTargetMode::MountPoint;
    if (capabilities.udisksctl && !targetedUnmount) {
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
        const QString mountPoint = RemoteCopyCommand::quoteArgument(request.target.mountPoint);
        if (mountPoint.isEmpty()) {
            return reject(rfm::core::VolumeOperationError::DeviceNotFound,
                          QStringLiteral("The remote volume has no safe mount point."));
        }
        return QStringLiteral("LC_ALL=C umount -- %1").arg(mountPoint);
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
    const rfm::core::VolumeUnmountTargetMode unmountTargetMode =
        rfm::core::volumeUnmountTargetMode(request);
    if (request.operation == rfm::core::VolumeOperation::Unmount &&
        unmountTargetMode == rfm::core::VolumeUnmountTargetMode::Invalid) {
        return reject(rfm::core::VolumeOperationError::DeviceNotFound,
                      QStringLiteral("The remote volume has no safe, consistent mount point."));
    }
    if (request.operation == rfm::core::VolumeOperation::Unmount &&
        unmountTargetMode == rfm::core::VolumeUnmountTargetMode::MountPoint) {
        return reject(
            rfm::core::VolumeOperationError::PermissionDenied,
            QStringLiteral("Targeted unmount authentication is unavailable without UDisks."));
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

std::optional<RemoteVolumeOperationCommand> RemoteLinuxVolumeService::revalidatedUnmountCommand(
    const rfm::core::VolumeOperationRequest& request,
    const rfm::core::VolumeCommandResult& topologyCommand,
    const RemoteLinuxVolumeCapabilities& capabilities, rfm::core::SecurePassword& password,
    rfm::core::VolumeOperationResult* immediateResult)
{
    const auto revalidated =
        rfm::core::revalidatedVolumeUnmountRequest(request, topologyCommand, immediateResult);
    if (!revalidated.has_value()) {
        password.clear();
        return std::nullopt;
    }

    const bool interactive =
        !password.isEmpty() && rfm::core::volumeUnmountTargetMode(*revalidated) ==
                                   rfm::core::VolumeUnmountTargetMode::Device;
    if (!interactive) {
        password.clear();
    }
    const auto command =
        interactive ? interactiveOperationCommand(*revalidated, capabilities, immediateResult)
                    : operationCommand(*revalidated, capabilities, immediateResult);
    if (!command.has_value()) {
        password.clear();
        return std::nullopt;
    }
    return RemoteVolumeOperationCommand{*revalidated, *command, interactive};
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
    const bool udisksInteractiveOperation =
        request.operation == rfm::core::VolumeOperation::Mount ||
        rfm::core::volumeUnmountTargetMode(request) == rfm::core::VolumeUnmountTargetMode::Device;
    const bool canAuthenticate =
        failedCommand && udisksInteractiveOperation &&
        (protocolDiagnostic.contains(QStringLiteral("NotAuthorizedCanObtain"),
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

rfm::core::VolumeOperationResult RemoteLinuxVolumeService::interactiveOperationResult(
    const rfm::core::VolumeOperationRequest& request,
    const rfm::core::VolumeCommandResult& commandResult,
    std::optional<rfm::core::VolumeOperationError> protocolError)
{
    if (protocolError.has_value()) {
        return rfm::core::makeVolumeOperationResult(request, *protocolError);
    }
    return operationResult(request, commandResult);
}

std::optional<SshCommandPollSchedule> SshCommandPollScheduler::schedule(bool activityAvailable)
{
    if (m_pending) {
        return std::nullopt;
    }
    m_pending = true;
    ++m_generation;
    if (m_generation == 0) {
        ++m_generation;
    }
    return SshCommandPollSchedule{activityAvailable ? 0 : idleDelayMilliseconds, m_generation};
}

bool SshCommandPollScheduler::consume(quint64 generation)
{
    if (!m_pending || generation == 0 || generation != m_generation) {
        return false;
    }
    m_pending = false;
    return true;
}

void SshCommandPollScheduler::cancel()
{
    m_pending = false;
    ++m_generation;
    if (m_generation == 0) {
        ++m_generation;
    }
}

bool SshCommandPollScheduler::pending() const { return m_pending; }

namespace
{

bool containsPasswordPrompt(const QByteArray& output)
{
    qsizetype passwordOffset = output.indexOf("password");
    while (passwordOffset >= 0) {
        qsizetype suffixOffset = passwordOffset + qsizetype{8};
        while (suffixOffset < output.size() &&
               (output.at(suffixOffset) == ' ' || output.at(suffixOffset) == '\t')) {
            ++suffixOffset;
        }
        if (suffixOffset < output.size() && output.at(suffixOffset) == ':') {
            return true;
        }
        if (output.mid(suffixOffset, 4) == QByteArrayLiteral("for ")) {
            const qsizetype colonOffset = output.indexOf(':', suffixOffset + 4);
            const qsizetype lineEnd = output.indexOf('\n', suffixOffset + 4);
            if (colonOffset >= 0 && colonOffset - suffixOffset <= 132 &&
                (lineEnd < 0 || colonOffset < lineEnd)) {
                return true;
            }
        }
        passwordOffset = output.indexOf("password", passwordOffset + 1);
    }
    return false;
}

} // namespace

RemotePolkitPromptEvent RemotePolkitPromptParser::consume(const QByteArray& output)
{
    if (output.isEmpty()) {
        return RemotePolkitPromptEvent::None;
    }
    for (const char character : output) {
        const auto byte = static_cast<unsigned char>(character);
        if (m_escapeState == EscapeState::ControlSequence) {
            if (byte >= 0x40U && byte <= 0x7eU) {
                m_escapeState = EscapeState::None;
            }
            continue;
        }
        if (m_escapeState == EscapeState::OperatingSystemCommand) {
            if (byte == 0x07U) {
                m_escapeState = EscapeState::None;
            } else if (byte == 0x1bU) {
                m_escapeState = EscapeState::Escape;
            }
            continue;
        }
        if (m_escapeState == EscapeState::Escape) {
            m_escapeState = byte == static_cast<unsigned char>('[') ? EscapeState::ControlSequence
                            : byte == static_cast<unsigned char>(']')
                                ? EscapeState::OperatingSystemCommand
                                : EscapeState::None;
            continue;
        }
        if (byte == 0x1bU) {
            m_escapeState = EscapeState::Escape;
            continue;
        }
        if (character == '\b') {
            if (!m_recentOutput.isEmpty()) {
                m_recentOutput.chop(1);
            }
            continue;
        }
        if (character != '\r' && (byte >= 0x20U || character == '\n' || character == '\t')) {
            m_recentOutput.append(static_cast<char>(std::tolower(byte)));
        }
    }
    constexpr qsizetype maximumProtocolWindow = 1024;
    if (m_recentOutput.size() > maximumProtocolWindow) {
        m_recentOutput.remove(0, m_recentOutput.size() - maximumProtocolWindow);
    }

    if (m_recentOutput.contains("authentication complete")) {
        m_authenticationState = RemotePolkitAuthenticationState::AuthenticationSucceeded;
    }
    m_permissionDenied = m_permissionDenied || m_recentOutput.contains("not authorized") ||
                         m_recentOutput.contains("permission denied");
    m_volumeBusy = m_volumeBusy || m_recentOutput.contains("target is busy") ||
                   m_recentOutput.contains("device is busy");
    m_deviceNotFound = m_deviceNotFound || m_recentOutput.contains("no such file") ||
                       m_recentOutput.contains("does not exist") ||
                       m_recentOutput.contains("error looking up object for device");
    const bool explicitFailure = m_recentOutput.contains("authentication failed") ||
                                 m_recentOutput.contains("authentication failure") ||
                                 m_recentOutput.contains("sorry, try again");
    const bool passwordPrompt = containsPasswordPrompt(m_recentOutput);
    const bool authenticationRetry =
        passwordPrompt &&
        m_authenticationState == RemotePolkitAuthenticationState::WaitingForAuthenticationResult;
    if (explicitFailure || authenticationRetry) {
        m_authenticationState = RemotePolkitAuthenticationState::AuthenticationFailed;
        m_recentOutput.fill('\0');
        m_recentOutput.clear();
        return RemotePolkitPromptEvent::AuthenticationFailed;
    }
    if (m_authenticationState == RemotePolkitAuthenticationState::WaitingForPasswordPrompt &&
        passwordPrompt) {
        m_authenticationState = RemotePolkitAuthenticationState::PasswordPromptReceived;
        m_recentOutput.fill('\0');
        m_recentOutput.clear();
        return RemotePolkitPromptEvent::PasswordPrompt;
    }
    return RemotePolkitPromptEvent::None;
}

RemotePolkitPromptEvent RemotePolkitPromptParser::timedOut() const
{
    return m_authenticationState == RemotePolkitAuthenticationState::WaitingForPasswordPrompt
               ? RemotePolkitPromptEvent::TimedOutBeforePrompt
               : RemotePolkitPromptEvent::TimedOutAfterPrompt;
}

void RemotePolkitPromptParser::passwordSent()
{
    if (m_authenticationState == RemotePolkitAuthenticationState::PasswordPromptReceived) {
        m_authenticationState = RemotePolkitAuthenticationState::WaitingForAuthenticationResult;
    }
    m_recentOutput.fill('\0');
    m_recentOutput.clear();
}

RemotePolkitAuthenticationState RemotePolkitPromptParser::authenticationState() const
{
    return m_authenticationState;
}

bool RemotePolkitPromptParser::authenticationCompleted() const
{
    return m_authenticationState == RemotePolkitAuthenticationState::AuthenticationSucceeded;
}

bool RemotePolkitPromptParser::permissionDenied() const { return m_permissionDenied; }

bool RemotePolkitPromptParser::volumeBusy() const { return m_volumeBusy; }

bool RemotePolkitPromptParser::deviceNotFound() const { return m_deviceNotFound; }

std::optional<rfm::core::VolumeOperationError> RemotePolkitPromptParser::operationError() const
{
    if (m_authenticationState == RemotePolkitAuthenticationState::AuthenticationFailed) {
        return rfm::core::VolumeOperationError::AuthenticationFailed;
    }
    if (m_volumeBusy) {
        return rfm::core::VolumeOperationError::VolumeBusy;
    }
    if (m_deviceNotFound) {
        return rfm::core::VolumeOperationError::DeviceNotFound;
    }
    if (m_permissionDenied) {
        return rfm::core::VolumeOperationError::PermissionDenied;
    }
    return std::nullopt;
}

void RemotePolkitPromptParser::clear()
{
    m_recentOutput.fill('\0');
    m_recentOutput.clear();
    m_authenticationState = RemotePolkitAuthenticationState::WaitingForPasswordPrompt;
    m_escapeState = EscapeState::None;
    m_permissionDenied = false;
    m_volumeBusy = false;
    m_deviceNotFound = false;
}

} // namespace rfm::ssh
