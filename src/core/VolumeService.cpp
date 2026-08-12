#include "remotefilemanager/core/VolumeService.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <utility>

namespace rfm::core
{
namespace
{

constexpr int commandTimeoutMilliseconds = 60'000;
constexpr qsizetype maximumDiagnosticCharacters = 32 * 1024;

class QProcessVolumeCommandRunner final : public VolumeCommandRunner
{
  public:
    QString findExecutable(const QString& name) const override
    {
        return QStandardPaths::findExecutable(name);
    }

    VolumeCommandResult run(const QString& program, const QStringList& arguments,
                            int timeoutMilliseconds) override
    {
        QProcess process;
        process.setProgram(program);
        process.setArguments(arguments);
        process.setProcessChannelMode(QProcess::SeparateChannels);
        QElapsedTimer timer;
        timer.start();
        process.start(QIODevice::ReadOnly);

        while (!process.waitForStarted(waitSliceMilliseconds)) {
            if (m_cancellationRequested.load(std::memory_order_acquire)) {
                stopProcess(process);
                return commandResult(process, false, false, true);
            }
            if (process.state() == QProcess::NotRunning) {
                return {};
            }
            if (timer.elapsed() >= timeoutMilliseconds) {
                stopProcess(process);
                return commandResult(process, false, true, false);
            }
        }

        while (process.state() != QProcess::NotRunning) {
            if (m_cancellationRequested.load(std::memory_order_acquire)) {
                stopProcess(process);
                return commandResult(process, true, false, true);
            }
            const qint64 remaining = timeoutMilliseconds - timer.elapsed();
            if (remaining <= 0) {
                stopProcess(process);
                return commandResult(process, true, true, false);
            }
            static_cast<void>(process.waitForFinished(
                static_cast<int>(std::min<qint64>(remaining, waitSliceMilliseconds))));
        }
        return commandResult(process, true, false, false);
    }

    void requestCancellation() override
    {
        m_cancellationRequested.store(true, std::memory_order_release);
    }

  private:
    static constexpr int waitSliceMilliseconds = 20;
    static constexpr int terminateGraceMilliseconds = 200;

    static void stopProcess(QProcess& process)
    {
        if (process.state() == QProcess::NotRunning) {
            return;
        }
        process.terminate();
        if (!process.waitForFinished(terminateGraceMilliseconds)) {
            process.kill();
            static_cast<void>(process.waitForFinished(terminateGraceMilliseconds));
        }
    }

    static VolumeCommandResult commandResult(QProcess& process, bool started, bool timedOut,
                                             bool cancelled)
    {
        return {started,
                timedOut,
                started && !timedOut && !cancelled && process.exitStatus() == QProcess::CrashExit,
                process.exitCode(),
                QString::fromLocal8Bit(process.readAllStandardOutput()),
                QString::fromLocal8Bit(process.readAllStandardError()),
                cancelled};
    }

    std::atomic_bool m_cancellationRequested{false};
};

QString boundedDiagnostic(QString value)
{
    value = value.trimmed();
    if (value.size() > maximumDiagnosticCharacters) {
        value.truncate(maximumDiagnosticCharacters);
    }
    return value;
}

} // namespace

VolumeOperationError volumeOperationErrorFromCommand(const VolumeCommandResult& result)
{
    if (result.cancelled) {
        return VolumeOperationError::Cancelled;
    }
    if (!result.started) {
        return VolumeOperationError::ToolUnavailable;
    }
    if (result.timedOut || result.crashed) {
        return VolumeOperationError::SystemError;
    }
    if (result.exitCode == 0) {
        return VolumeOperationError::None;
    }
    if (result.exitCode == 126 || result.exitCode == 127) {
        return VolumeOperationError::ToolUnavailable;
    }

    const QString diagnostic =
        (result.standardError + QChar{'\n'} + result.standardOutput).toCaseFolded();
    if (diagnostic.contains(QStringLiteral("not authorized")) ||
        diagnostic.contains(QStringLiteral("not permitted")) ||
        diagnostic.contains(QStringLiteral("permission denied")) ||
        diagnostic.contains(QStringLiteral("authentication is required")) ||
        diagnostic.contains(QStringLiteral("authorization is required")) ||
        diagnostic.contains(QStringLiteral("must be superuser"))) {
        return VolumeOperationError::PermissionDenied;
    }
    if (diagnostic.contains(QStringLiteral("device or resource busy")) ||
        diagnostic.contains(QStringLiteral("target is busy")) ||
        diagnostic.contains(QStringLiteral("device is busy"))) {
        return VolumeOperationError::VolumeBusy;
    }
    if (diagnostic.contains(QStringLiteral("no such file")) ||
        diagnostic.contains(QStringLiteral("not found")) ||
        diagnostic.contains(QStringLiteral("does not exist")) ||
        diagnostic.contains(QStringLiteral("looking up object for device"))) {
        return VolumeOperationError::DeviceNotFound;
    }
    return VolumeOperationError::SystemError;
}

VolumeOperationResult makeVolumeOperationResult(const VolumeOperationRequest& request,
                                                VolumeOperationError error,
                                                QString technicalMessage)
{
    return {request.id, request.operation, request.target.device, error,
            boundedDiagnostic(std::move(technicalMessage))};
}

bool isSafeLinuxDevicePath(const QString& device)
{
    static const QRegularExpression pattern(QStringLiteral("^/dev/[A-Za-z0-9._+:/-]+$"));
    const QString trimmed = device.trimmed();
    return trimmed == device && pattern.match(device).hasMatch() &&
           RemotePath::normalize(device) == device && device != QStringLiteral("/dev");
}

namespace
{

bool isNormalizedAbsoluteLinuxPath(const QString& path)
{
    if (path.isEmpty() || !path.startsWith(QChar{'/'}) || RemotePath::normalize(path) != path) {
        return false;
    }
    return std::ranges::none_of(path, [](QChar character) {
        return character.category() == QChar::Other_Control ||
               character.category() == QChar::Separator_Line ||
               character.category() == QChar::Separator_Paragraph;
    });
}

} // namespace

bool isSafeLinuxMountPoint(const QString& mountPoint)
{
    return mountPoint != QStringLiteral("/") && isNormalizedAbsoluteLinuxPath(mountPoint);
}

VolumeUnmountTargetMode volumeUnmountTargetMode(const VolumeOperationRequest& request)
{
    if (request.operation != VolumeOperation::Unmount ||
        !isSafeLinuxMountPoint(request.target.mountPoint)) {
        return VolumeUnmountTargetMode::Invalid;
    }

    QSet<QString> uniqueMountPoints;
    for (const QString& mountPoint : request.target.knownMountPoints) {
        if (!isNormalizedAbsoluteLinuxPath(mountPoint)) {
            return VolumeUnmountTargetMode::Invalid;
        }
        uniqueMountPoints.insert(mountPoint);
    }
    if (!uniqueMountPoints.contains(request.target.mountPoint)) {
        return VolumeUnmountTargetMode::Invalid;
    }
    return uniqueMountPoints.size() == 1 ? VolumeUnmountTargetMode::Device
                                         : VolumeUnmountTargetMode::MountPoint;
}

bool isProtectedVolumeOperation(const VolumeOperationRequest& request)
{
    return request.operation == VolumeOperation::Unmount &&
           (RemotePath::normalize(request.target.mountPoint) == QStringLiteral("/") ||
            request.target.kind == StorageKind::System);
}

LocalLinuxVolumeService::LocalLinuxVolumeService(std::unique_ptr<VolumeCommandRunner> runner)
    : m_runner(runner == nullptr ? std::make_unique<QProcessVolumeCommandRunner>()
                                 : std::move(runner))
{}

void LocalLinuxVolumeService::requestCancellation() { m_runner->requestCancellation(); }

VolumeOperationResult LocalLinuxVolumeService::execute(const VolumeOperationRequest& request)
{
#ifndef Q_OS_LINUX
    return makeVolumeOperationResult(
        request, VolumeOperationError::NotSupported,
        QStringLiteral("Local volume operations are currently implemented on Linux only."));
#else
    if (isProtectedVolumeOperation(request)) {
        return makeVolumeOperationResult(
            request, VolumeOperationError::NotSupported,
            QStringLiteral("RemoteFileManager never unmounts the system volume."));
    }
    const VolumeUnmountTargetMode unmountTargetMode = volumeUnmountTargetMode(request);
    if (request.operation == VolumeOperation::Unmount &&
        unmountTargetMode == VolumeUnmountTargetMode::Invalid) {
        return makeVolumeOperationResult(
            request, VolumeOperationError::DeviceNotFound,
            QStringLiteral("The volume has no safe, consistent mount point."));
    }
    const QString device = QDir::cleanPath(request.target.device.trimmed());
    if (!device.startsWith(QStringLiteral("/dev/")) || device == QStringLiteral("/dev")) {
        return makeVolumeOperationResult(
            request, VolumeOperationError::DeviceNotFound,
            QStringLiteral("The volume has no valid system device identifier."));
    }

    {
        QMutexLocker lock(&m_activeDevicesMutex);
        if (m_activeDevices.contains(device)) {
            return makeVolumeOperationResult(
                request, VolumeOperationError::VolumeBusy,
                QStringLiteral("Another operation is already active for this volume."));
        }
        m_activeDevices.insert(device);
    }

    VolumeOperationResult result = executeUnlocked(request, device);
    {
        QMutexLocker lock(&m_activeDevicesMutex);
        m_activeDevices.remove(device);
    }
    return result;
#endif
}

VolumeOperationResult
LocalLinuxVolumeService::executeUnlocked(const VolumeOperationRequest& request,
                                         const QString& device)
{
    const VolumeUnmountTargetMode unmountTargetMode = volumeUnmountTargetMode(request);
    const bool targetedUnmount = request.operation == VolumeOperation::Unmount &&
                                 unmountTargetMode == VolumeUnmountTargetMode::MountPoint;
    QString program = m_runner->findExecutable(QStringLiteral("udisksctl"));
    QStringList arguments;
    if (!program.isEmpty() && !targetedUnmount) {
        arguments = {request.operation == VolumeOperation::Mount ? QStringLiteral("mount")
                                                                 : QStringLiteral("unmount"),
                     QStringLiteral("-b"), device};
    } else {
        const QString fallback = request.operation == VolumeOperation::Mount
                                     ? QStringLiteral("mount")
                                     : QStringLiteral("umount");
        program = m_runner->findExecutable(fallback);
        if (program.isEmpty()) {
            return makeVolumeOperationResult(
                request, VolumeOperationError::ToolUnavailable,
                QStringLiteral("No supported local volume tool is available."));
        }
        arguments = {QStringLiteral("--"), request.operation == VolumeOperation::Unmount
                                               ? request.target.mountPoint
                                               : device};
    }

    const VolumeCommandResult command =
        m_runner->run(program, arguments, commandTimeoutMilliseconds);
    const QString diagnostic =
        command.standardError.trimmed().isEmpty() ? command.standardOutput : command.standardError;
    return makeVolumeOperationResult(request, volumeOperationErrorFromCommand(command), diagnostic);
}

VolumeOperationWorker::VolumeOperationWorker(std::unique_ptr<VolumeService> service,
                                             QObject* parent)
    : QObject(parent), m_service(std::move(service))
{}

void VolumeOperationWorker::execute(VolumeOperationRequest request)
{
    if (m_service == nullptr) {
        emit finished(
            makeVolumeOperationResult(request, VolumeOperationError::NotSupported,
                                      QStringLiteral("No volume service is configured.")));
        return;
    }
    emit finished(m_service->execute(request));
}

void VolumeOperationWorker::requestCancellation()
{
    if (m_service != nullptr) {
        m_service->requestCancellation();
    }
}

} // namespace rfm::core
