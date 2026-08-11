#include "remotefilemanager/core/VolumeService.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QDir>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
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
        process.start(QIODevice::ReadOnly);
        if (!process.waitForStarted()) {
            return {};
        }
        if (!process.waitForFinished(timeoutMilliseconds)) {
            process.kill();
            static_cast<void>(process.waitForFinished());
            return {true,
                    true,
                    false,
                    process.exitCode(),
                    QString::fromLocal8Bit(process.readAllStandardOutput()),
                    QString::fromLocal8Bit(process.readAllStandardError())};
        }
        return {true,
                false,
                process.exitStatus() == QProcess::CrashExit,
                process.exitCode(),
                QString::fromLocal8Bit(process.readAllStandardOutput()),
                QString::fromLocal8Bit(process.readAllStandardError())};
    }
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

VolumeOperationResult LocalLinuxVolumeService::execute(const VolumeOperationRequest& request)
{
#ifndef Q_OS_LINUX
    return makeVolumeOperationResult(
        request, VolumeOperationError::NotSupported,
        QStringLiteral("Local volume operations are currently implemented on Linux only."));
#else
    if (request.operation == VolumeOperation::Unmount &&
        (QDir::cleanPath(request.target.mountPoint) == QStringLiteral("/") ||
         request.target.kind == StorageKind::System)) {
        return makeVolumeOperationResult(
            request, VolumeOperationError::NotSupported,
            QStringLiteral("RemoteFileManager never unmounts the system volume."));
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
    QString program = m_runner->findExecutable(QStringLiteral("udisksctl"));
    QStringList arguments;
    if (!program.isEmpty()) {
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
        arguments = {QStringLiteral("--"), device};
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

} // namespace rfm::core
