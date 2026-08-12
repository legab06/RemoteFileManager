#pragma once

#include "remotefilemanager/core/VolumeService.hpp"

#include <QByteArray>
#include <QString>

#include <optional>

namespace rfm::ssh
{

struct RemoteLinuxVolumeCapabilities {
    bool known{false};
    bool lsblk{false};
    bool udisksctl{false};
    bool mount{false};
    bool umount{false};
};

class RemoteLinuxVolumeCapabilityCache final
{
  public:
    [[nodiscard]] const RemoteLinuxVolumeCapabilities& value() const;
    void update(RemoteLinuxVolumeCapabilities capabilities);
    void reset();

  private:
    RemoteLinuxVolumeCapabilities m_capabilities;
};

class RemoteLinuxVolumeService final
{
  public:
    [[nodiscard]] static QString capabilityProbeCommand();
    [[nodiscard]] static RemoteLinuxVolumeCapabilities
    parseCapabilities(const QByteArray& standardOutput);
    [[nodiscard]] static QString blockDeviceDiscoveryCommand();
    [[nodiscard]] static std::optional<QString>
    operationCommand(const rfm::core::VolumeOperationRequest& request,
                     const RemoteLinuxVolumeCapabilities& capabilities,
                     rfm::core::VolumeOperationResult* immediateResult = nullptr);
    [[nodiscard]] static std::optional<QString>
    interactiveOperationCommand(const rfm::core::VolumeOperationRequest& request,
                                const RemoteLinuxVolumeCapabilities& capabilities,
                                rfm::core::VolumeOperationResult* immediateResult = nullptr);
    [[nodiscard]] static rfm::core::VolumeOperationResult
    operationResult(const rfm::core::VolumeOperationRequest& request,
                    const rfm::core::VolumeCommandResult& commandResult);
    [[nodiscard]] static rfm::core::VolumeOperationResult
    interactiveOperationResult(const rfm::core::VolumeOperationRequest& request,
                               const rfm::core::VolumeCommandResult& commandResult,
                               std::optional<rfm::core::VolumeOperationError> protocolError);
};

struct SshCommandPollSchedule {
    int delayMilliseconds{0};
    quint64 generation{0};
};

class SshCommandPollScheduler final
{
  public:
    static constexpr int idleDelayMilliseconds = 10;
    static constexpr qint64 commandTimeoutMilliseconds = 60'000;

    [[nodiscard]] std::optional<SshCommandPollSchedule> schedule(bool activityAvailable);
    [[nodiscard]] bool consume(quint64 generation);
    void cancel();
    [[nodiscard]] bool pending() const;

  private:
    bool m_pending{false};
    quint64 m_generation{0};
};

enum class RemotePolkitPromptEvent {
    None,
    PasswordPrompt,
    AuthenticationFailed,
    TimedOutBeforePrompt,
    TimedOutAfterPrompt
};

enum class RemotePolkitAuthenticationState {
    WaitingForPasswordPrompt,
    PasswordPromptReceived,
    WaitingForAuthenticationResult,
    AuthenticationSucceeded,
    AuthenticationFailed
};

class RemotePolkitPromptParser final
{
  public:
    [[nodiscard]] RemotePolkitPromptEvent consume(const QByteArray& output);
    [[nodiscard]] RemotePolkitPromptEvent timedOut() const;
    void passwordSent();
    [[nodiscard]] RemotePolkitAuthenticationState authenticationState() const;
    [[nodiscard]] bool authenticationCompleted() const;
    [[nodiscard]] bool permissionDenied() const;
    [[nodiscard]] bool volumeBusy() const;
    [[nodiscard]] bool deviceNotFound() const;
    [[nodiscard]] std::optional<rfm::core::VolumeOperationError> operationError() const;
    void clear();

  private:
    enum class EscapeState { None, Escape, ControlSequence, OperatingSystemCommand };

    QByteArray m_recentOutput;
    RemotePolkitAuthenticationState m_authenticationState{
        RemotePolkitAuthenticationState::WaitingForPasswordPrompt};
    EscapeState m_escapeState{EscapeState::None};
    bool m_permissionDenied{false};
    bool m_volumeBusy{false};
    bool m_deviceNotFound{false};
};

} // namespace rfm::ssh
