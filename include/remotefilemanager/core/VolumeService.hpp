#pragma once

#include "remotefilemanager/core/Storage.hpp"

#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

namespace rfm::core
{

enum class VolumeOperation { Mount, Unmount };

enum class VolumeOperationError {
    None,
    NotSupported,
    AuthenticationRequired,
    AuthenticationFailed,
    Cancelled,
    PermissionDenied,
    DeviceNotFound,
    VolumeBusy,
    ToolUnavailable,
    ConnectionLost,
    SystemError
};

struct VolumeOperationTarget {
    // System identity used as a process argument. Never derive this from displayName.
    QString device;
    QString mountPoint;
    StorageKind kind{StorageKind::Unknown};
};

struct VolumeOperationRequest {
    quint64 id{0};
    VolumeOperation operation{VolumeOperation::Mount};
    VolumeOperationTarget target;
};

struct VolumeOperationResult {
    quint64 id{0};
    VolumeOperation operation{VolumeOperation::Mount};
    QString device;
    VolumeOperationError error{VolumeOperationError::None};
    QString technicalMessage;
    quint64 authenticationToken{0};

    [[nodiscard]] bool succeeded() const { return error == VolumeOperationError::None; }
};

struct VolumeCommandResult {
    bool started{false};
    bool timedOut{false};
    bool crashed{false};
    int exitCode{-1};
    QString standardOutput;
    QString standardError;
    bool cancelled{false};
};

[[nodiscard]] VolumeOperationError
volumeOperationErrorFromCommand(const VolumeCommandResult& result);
[[nodiscard]] VolumeOperationResult makeVolumeOperationResult(const VolumeOperationRequest& request,
                                                              VolumeOperationError error,
                                                              QString technicalMessage = {});
[[nodiscard]] bool isSafeLinuxDevicePath(const QString& device);
[[nodiscard]] bool isProtectedVolumeOperation(const VolumeOperationRequest& request);

class VolumeCommandRunner
{
  public:
    virtual ~VolumeCommandRunner() = default;

    [[nodiscard]] virtual QString findExecutable(const QString& name) const = 0;
    [[nodiscard]] virtual VolumeCommandResult
    run(const QString& program, const QStringList& arguments, int timeoutMilliseconds) = 0;
    // May be called from another thread while run() is active.
    virtual void requestCancellation() {}
};

class VolumeService
{
  public:
    virtual ~VolumeService() = default;
    [[nodiscard]] virtual VolumeOperationResult execute(const VolumeOperationRequest& request) = 0;
    // May be called from another thread while execute() is active.
    virtual void requestCancellation() {}
};

class LocalLinuxVolumeService final : public VolumeService
{
  public:
    explicit LocalLinuxVolumeService(std::unique_ptr<VolumeCommandRunner> runner = {});
    [[nodiscard]] VolumeOperationResult execute(const VolumeOperationRequest& request) override;
    void requestCancellation() override;

  private:
    [[nodiscard]] VolumeOperationResult executeUnlocked(const VolumeOperationRequest& request,
                                                        const QString& device);

    std::unique_ptr<VolumeCommandRunner> m_runner;
    QMutex m_activeDevicesMutex;
    QSet<QString> m_activeDevices;
};

class VolumeOperationWorker final : public QObject
{
    Q_OBJECT

  public:
    explicit VolumeOperationWorker(std::unique_ptr<VolumeService> service,
                                   QObject* parent = nullptr);
    // Thread-safe: shutdown calls this directly because execute() may occupy the worker thread.
    void requestCancellation();

  public slots:
    void execute(rfm::core::VolumeOperationRequest request);

  signals:
    void finished(rfm::core::VolumeOperationResult result);

  private:
    std::unique_ptr<VolumeService> m_service;
};

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::VolumeOperation)
Q_DECLARE_METATYPE(rfm::core::VolumeOperationError)
Q_DECLARE_METATYPE(rfm::core::VolumeOperationTarget)
Q_DECLARE_METATYPE(rfm::core::VolumeOperationRequest)
Q_DECLARE_METATYPE(rfm::core::VolumeOperationResult)
