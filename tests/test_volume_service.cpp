#include "remotefilemanager/core/VolumeService.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace
{

class FakeCommandRunner final : public rfm::core::VolumeCommandRunner
{
  public:
    QString findExecutable(const QString& name) const override { return executables.value(name); }

    rfm::core::VolumeCommandResult run(const QString& program, const QStringList& arguments,
                                       int timeoutMilliseconds) override
    {
        ++runCount;
        lastProgram = program;
        lastArguments = arguments;
        lastTimeoutMilliseconds = timeoutMilliseconds;
        return result;
    }

    QHash<QString, QString> executables;
    rfm::core::VolumeCommandResult result{true, false, false, 0, {}, {}, false};
    int runCount{0};
    QString lastProgram;
    QStringList lastArguments;
    int lastTimeoutMilliseconds{0};
};

class BlockingCommandRunner final : public rfm::core::VolumeCommandRunner
{
  public:
    QString findExecutable(const QString& name) const override
    {
        return name == QStringLiteral("udisksctl") ? QStringLiteral("/usr/bin/udisksctl")
                                                   : QString{};
    }

    rfm::core::VolumeCommandResult run(const QString&, const QStringList&, int) override
    {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [this] { return released || cancelled; });
        return {true, false, false, cancelled ? -1 : 0, {}, {}, cancelled};
    }

    bool waitUntilEntered()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(2), [this] { return entered; });
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }

    void requestCancellation() override
    {
        std::lock_guard lock(mutex);
        cancelled = true;
        condition.notify_all();
    }

  private:
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool released{false};
    bool cancelled{false};
};

rfm::core::VolumeOperationRequest
requestFor(rfm::core::VolumeOperation operation, QString device = QStringLiteral("/dev/sde1"),
           QString mountPoint = QStringLiteral("/media/data"),
           rfm::core::StorageKind kind = rfm::core::StorageKind::External,
           QStringList knownMountPoints = {})
{
    if (operation == rfm::core::VolumeOperation::Unmount && knownMountPoints.isEmpty()) {
        knownMountPoints.push_back(mountPoint);
    }
    return {42,
            operation,
            {std::move(device), std::move(mountPoint), kind, std::move(knownMountPoints)}};
}

} // namespace

class VolumeServiceTest final : public QObject
{
    Q_OBJECT

  private slots:
    void refusesToUnmountRootOrSystemVolume();
    void reportsDisappearedDevice();
    void mapsCommandErrors_data();
    void mapsCommandErrors();
    void prefersUdisksctl();
    void targetsSelectedMountPointForMultipleAttachments();
    void fallsBackToSystemTools();
    void rejectsUnsafeOrInconsistentMountPoints_data();
    void rejectsUnsafeOrInconsistentMountPoints();
    void reportsUnavailableTools();
    void passesDeviceAsOneArgumentAndIgnoresPresentationMetadata();
    void preventsConcurrentOperationsOnOneDevice();
    void cancellationInterruptsOperationAndReleasesDeviceLock();
    void cancellationTerminatesOwnedProcessQuickly();
};

void VolumeServiceTest::refusesToUnmountRootOrSystemVolume()
{
    auto runner = std::make_unique<FakeCommandRunner>();
    FakeCommandRunner* runnerObserver = runner.get();
    rfm::core::LocalLinuxVolumeService service(std::move(runner));

    const auto rootResult =
        service.execute(requestFor(rfm::core::VolumeOperation::Unmount, QStringLiteral("/dev/root"),
                                   QStringLiteral("/"), rfm::core::StorageKind::System));
    QCOMPARE(rootResult.error, rfm::core::VolumeOperationError::NotSupported);

    const auto systemResult =
        service.execute(requestFor(rfm::core::VolumeOperation::Unmount, QStringLiteral("/dev/sda2"),
                                   QStringLiteral("/boot"), rfm::core::StorageKind::System));
    QCOMPARE(systemResult.error, rfm::core::VolumeOperationError::NotSupported);
    QCOMPARE(runnerObserver->runCount, 0);
}

void VolumeServiceTest::reportsDisappearedDevice()
{
    auto runner = std::make_unique<FakeCommandRunner>();
    runner->executables.insert(QStringLiteral("udisksctl"), QStringLiteral("/usr/bin/udisksctl"));
    runner->result = {true, false, false,
                      1,    {},    QStringLiteral("Error looking up object for device /dev/sde1"),
                      false};
    rfm::core::LocalLinuxVolumeService service(std::move(runner));

    const auto result = service.execute(requestFor(rfm::core::VolumeOperation::Mount));

    QCOMPARE(result.error, rfm::core::VolumeOperationError::DeviceNotFound);
    QVERIFY(result.technicalMessage.contains(QStringLiteral("/dev/sde1")));
}

void VolumeServiceTest::mapsCommandErrors_data()
{
    QTest::addColumn<QString>("diagnostic");
    QTest::addColumn<rfm::core::VolumeOperationError>("expectedError");

    QTest::newRow("permission") << QStringLiteral("Error mounting: Not authorized")
                                << rfm::core::VolumeOperationError::PermissionDenied;
    QTest::newRow("busy") << QStringLiteral("umount: target is busy")
                          << rfm::core::VolumeOperationError::VolumeBusy;
    QTest::newRow("not-found") << QStringLiteral("mount: /dev/sde1: no such file")
                               << rfm::core::VolumeOperationError::DeviceNotFound;
    QTest::newRow("other") << QStringLiteral("unexpected I/O failure")
                           << rfm::core::VolumeOperationError::SystemError;
}

void VolumeServiceTest::mapsCommandErrors()
{
    QFETCH(QString, diagnostic);
    QFETCH(rfm::core::VolumeOperationError, expectedError);

    auto runner = std::make_unique<FakeCommandRunner>();
    runner->executables.insert(QStringLiteral("udisksctl"), QStringLiteral("/usr/bin/udisksctl"));
    runner->result = {true, false, false, 1, {}, diagnostic, false};
    rfm::core::LocalLinuxVolumeService service(std::move(runner));

    const auto result = service.execute(requestFor(rfm::core::VolumeOperation::Unmount));

    QCOMPARE(result.error, expectedError);
    QCOMPARE(result.technicalMessage, diagnostic);
}

void VolumeServiceTest::prefersUdisksctl()
{
    auto runner = std::make_unique<FakeCommandRunner>();
    runner->executables.insert(QStringLiteral("udisksctl"), QStringLiteral("/usr/bin/udisksctl"));
    runner->executables.insert(QStringLiteral("mount"), QStringLiteral("/usr/bin/mount"));
    FakeCommandRunner* runnerObserver = runner.get();
    rfm::core::LocalLinuxVolumeService service(std::move(runner));

    const auto result = service.execute(requestFor(rfm::core::VolumeOperation::Mount));

    QVERIFY(result.succeeded());
    QCOMPARE(runnerObserver->lastProgram, QStringLiteral("/usr/bin/udisksctl"));
    QCOMPARE(
        runnerObserver->lastArguments,
        QStringList({QStringLiteral("mount"), QStringLiteral("-b"), QStringLiteral("/dev/sde1")}));
    QVERIFY(runnerObserver->lastTimeoutMilliseconds > 0);

    const auto unmountResult = service.execute(requestFor(rfm::core::VolumeOperation::Unmount));
    QVERIFY(unmountResult.succeeded());
    QCOMPARE(runnerObserver->lastProgram, QStringLiteral("/usr/bin/udisksctl"));
    QCOMPARE(runnerObserver->lastArguments,
             QStringList(
                 {QStringLiteral("unmount"), QStringLiteral("-b"), QStringLiteral("/dev/sde1")}));
}

void VolumeServiceTest::targetsSelectedMountPointForMultipleAttachments()
{
    auto runner = std::make_unique<FakeCommandRunner>();
    runner->executables.insert(QStringLiteral("udisksctl"), QStringLiteral("/usr/bin/udisksctl"));
    runner->executables.insert(QStringLiteral("umount"), QStringLiteral("/usr/bin/umount"));
    FakeCommandRunner* runnerObserver = runner.get();
    rfm::core::LocalLinuxVolumeService service(std::move(runner));

    const auto result = service.execute(
        requestFor(rfm::core::VolumeOperation::Unmount, QStringLiteral("/dev/sde1"),
                   QStringLiteral("/mnt/My Backup"), rfm::core::StorageKind::External,
                   {QStringLiteral("/mnt/My Backup"), QStringLiteral("/mnt/other")}));

    QVERIFY(result.succeeded());
    QCOMPARE(runnerObserver->lastProgram, QStringLiteral("/usr/bin/umount"));
    QCOMPARE(runnerObserver->lastArguments,
             QStringList({QStringLiteral("--"), QStringLiteral("/mnt/My Backup")}));
    QVERIFY(!runnerObserver->lastArguments.contains(QStringLiteral("--all-targets")));

    const auto rootSiblingResult =
        service.execute(requestFor(rfm::core::VolumeOperation::Unmount, QStringLiteral("/dev/sde1"),
                                   QStringLiteral("/mnt/data"), rfm::core::StorageKind::External,
                                   {QStringLiteral("/"), QStringLiteral("/mnt/data")}));
    QVERIFY(rootSiblingResult.succeeded());
    QCOMPARE(runnerObserver->lastProgram, QStringLiteral("/usr/bin/umount"));
    QCOMPARE(runnerObserver->lastArguments,
             QStringList({QStringLiteral("--"), QStringLiteral("/mnt/data")}));

    auto udisksOnlyRunner = std::make_unique<FakeCommandRunner>();
    udisksOnlyRunner->executables.insert(QStringLiteral("udisksctl"),
                                         QStringLiteral("/usr/bin/udisksctl"));
    FakeCommandRunner* udisksOnlyObserver = udisksOnlyRunner.get();
    rfm::core::LocalLinuxVolumeService udisksOnlyService(std::move(udisksOnlyRunner));
    const auto unavailable = udisksOnlyService.execute(requestFor(
        rfm::core::VolumeOperation::Unmount, QStringLiteral("/dev/sde1"), QStringLiteral("/mnt/a"),
        rfm::core::StorageKind::External, {QStringLiteral("/mnt/a"), QStringLiteral("/mnt/b")}));
    QCOMPARE(unavailable.error, rfm::core::VolumeOperationError::ToolUnavailable);
    QCOMPARE(udisksOnlyObserver->runCount, 0);
}

void VolumeServiceTest::fallsBackToSystemTools()
{
    auto mountRunner = std::make_unique<FakeCommandRunner>();
    mountRunner->executables.insert(QStringLiteral("mount"), QStringLiteral("/usr/bin/mount"));
    FakeCommandRunner* mountObserver = mountRunner.get();
    rfm::core::LocalLinuxVolumeService mountService(std::move(mountRunner));

    QVERIFY(mountService.execute(requestFor(rfm::core::VolumeOperation::Mount)).succeeded());
    QCOMPARE(mountObserver->lastProgram, QStringLiteral("/usr/bin/mount"));
    QCOMPARE(mountObserver->lastArguments,
             QStringList({QStringLiteral("--"), QStringLiteral("/dev/sde1")}));

    auto unmountRunner = std::make_unique<FakeCommandRunner>();
    unmountRunner->executables.insert(QStringLiteral("umount"), QStringLiteral("/usr/bin/umount"));
    FakeCommandRunner* unmountObserver = unmountRunner.get();
    rfm::core::LocalLinuxVolumeService unmountService(std::move(unmountRunner));

    QVERIFY(unmountService.execute(requestFor(rfm::core::VolumeOperation::Unmount)).succeeded());
    QCOMPARE(unmountObserver->lastProgram, QStringLiteral("/usr/bin/umount"));
    QCOMPARE(unmountObserver->lastArguments,
             QStringList({QStringLiteral("--"), QStringLiteral("/media/data")}));
}

void VolumeServiceTest::rejectsUnsafeOrInconsistentMountPoints_data()
{
    QTest::addColumn<QString>("mountPoint");
    QTest::addColumn<QStringList>("knownMountPoints");

    QTest::newRow("empty") << QString{} << QStringList{};
    QTest::newRow("relative") << QStringLiteral("mnt/data")
                              << QStringList{QStringLiteral("mnt/data")};
    QTest::newRow("not-normalized") << QStringLiteral("/mnt/data/../other")
                                    << QStringList{QStringLiteral("/mnt/data/../other")};
    QTest::newRow("line-break") << QStringLiteral("/mnt/data\nother")
                                << QStringList{QStringLiteral("/mnt/data\nother")};
    QTest::newRow("missing-from-snapshot")
        << QStringLiteral("/mnt/data") << QStringList{QStringLiteral("/mnt/other")};
    QTest::newRow("invalid-snapshot-entry")
        << QStringLiteral("/mnt/data")
        << QStringList{QStringLiteral("/mnt/data"), QStringLiteral("relative")};
}

void VolumeServiceTest::rejectsUnsafeOrInconsistentMountPoints()
{
    QFETCH(QString, mountPoint);
    QFETCH(QStringList, knownMountPoints);
    auto runner = std::make_unique<FakeCommandRunner>();
    runner->executables.insert(QStringLiteral("udisksctl"), QStringLiteral("/usr/bin/udisksctl"));
    runner->executables.insert(QStringLiteral("umount"), QStringLiteral("/usr/bin/umount"));
    FakeCommandRunner* runnerObserver = runner.get();
    rfm::core::LocalLinuxVolumeService service(std::move(runner));
    const rfm::core::VolumeOperationRequest request{42,
                                                    rfm::core::VolumeOperation::Unmount,
                                                    {QStringLiteral("/dev/sde1"), mountPoint,
                                                     rfm::core::StorageKind::External,
                                                     knownMountPoints}};

    const auto result = service.execute(request);

    QCOMPARE(result.error, rfm::core::VolumeOperationError::DeviceNotFound);
    QCOMPARE(runnerObserver->runCount, 0);
}

void VolumeServiceTest::reportsUnavailableTools()
{
    auto runner = std::make_unique<FakeCommandRunner>();
    FakeCommandRunner* runnerObserver = runner.get();
    rfm::core::LocalLinuxVolumeService service(std::move(runner));

    const auto result = service.execute(requestFor(rfm::core::VolumeOperation::Mount));

    QCOMPARE(result.error, rfm::core::VolumeOperationError::ToolUnavailable);
    QCOMPARE(runnerObserver->runCount, 0);
}

void VolumeServiceTest::passesDeviceAsOneArgumentAndIgnoresPresentationMetadata()
{
    const QString hostileMetadata = QStringLiteral("Backup; touch /tmp/rfm-injected");
    const rfm::core::StorageVolume volume(
        hostileMetadata, QStringLiteral("/media/My Backup"),
        QStringLiteral("/dev/disk/by-label/My Backup;$HOME"), QByteArrayLiteral("ext4"), 1,
        rfm::core::StorageKind::External, true, true, false, hostileMetadata, hostileMetadata);
    auto runner = std::make_unique<FakeCommandRunner>();
    runner->executables.insert(QStringLiteral("udisksctl"), QStringLiteral("/usr/bin/udisksctl"));
    FakeCommandRunner* runnerObserver = runner.get();
    rfm::core::LocalLinuxVolumeService service(std::move(runner));

    const auto result = service.execute(
        {9, rfm::core::VolumeOperation::Mount, {volume.device, volume.rootPath, volume.kind, {}}});

    QVERIFY(result.succeeded());
    QCOMPARE(runnerObserver->lastArguments.size(), 3);
    QCOMPARE(runnerObserver->lastArguments.constLast(), volume.device);
    QVERIFY(!runnerObserver->lastArguments.contains(hostileMetadata));
}

void VolumeServiceTest::preventsConcurrentOperationsOnOneDevice()
{
    auto runner = std::make_unique<BlockingCommandRunner>();
    BlockingCommandRunner* runnerObserver = runner.get();
    rfm::core::LocalLinuxVolumeService service(std::move(runner));
    std::optional<rfm::core::VolumeOperationResult> firstResult;

    std::thread firstOperation([&service, &firstResult] {
        firstResult = service.execute(requestFor(rfm::core::VolumeOperation::Mount));
    });
    const bool firstEntered = runnerObserver->waitUntilEntered();
    if (!firstEntered) {
        runnerObserver->release();
        firstOperation.join();
        QFAIL("The first operation did not reach the command runner");
    }

    const auto concurrentResult = service.execute(requestFor(rfm::core::VolumeOperation::Unmount));
    runnerObserver->release();
    firstOperation.join();

    QCOMPARE(concurrentResult.error, rfm::core::VolumeOperationError::VolumeBusy);
    QVERIFY(firstResult.has_value());
    QVERIFY(firstResult->succeeded());
}

void VolumeServiceTest::cancellationInterruptsOperationAndReleasesDeviceLock()
{
    auto runner = std::make_unique<BlockingCommandRunner>();
    BlockingCommandRunner* runnerObserver = runner.get();
    rfm::core::LocalLinuxVolumeService service(std::move(runner));
    std::optional<rfm::core::VolumeOperationResult> result;

    std::thread operation([&service, &result] {
        result = service.execute(requestFor(rfm::core::VolumeOperation::Mount));
    });
    const bool entered = runnerObserver->waitUntilEntered();
    if (!entered) {
        runnerObserver->release();
        operation.join();
        QFAIL("The operation did not reach the command runner");
    }

    const auto started = std::chrono::steady_clock::now();
    service.requestCancellation();
    operation.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    QVERIFY(elapsed < std::chrono::seconds(1));
    QVERIFY(result.has_value());
    QCOMPARE(result->error, rfm::core::VolumeOperationError::Cancelled);
    const auto afterCancellation = service.execute(requestFor(rfm::core::VolumeOperation::Unmount));
    QCOMPARE(afterCancellation.error, rfm::core::VolumeOperationError::Cancelled);
    QVERIFY(afterCancellation.error != rfm::core::VolumeOperationError::VolumeBusy);
}

void VolumeServiceTest::cancellationTerminatesOwnedProcessQuickly()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString executable = directory.filePath(QStringLiteral("udisksctl"));
    const QString marker = directory.filePath(QStringLiteral("started"));
    QFile script(executable);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray contents =
        QByteArrayLiteral("#!/bin/sh\n") + QByteArrayLiteral("touch '") + marker.toUtf8() +
        QByteArrayLiteral("'\ntrap 'exit 0' TERM\nwhile :; do sleep 1; done\n");
    QCOMPARE(script.write(contents), contents.size());
    script.close();
    QVERIFY(QFile::setPermissions(executable, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    const QByteArray originalPath = qgetenv("PATH");
    struct PathRestorer {
        QByteArray value;
        ~PathRestorer() { qputenv("PATH", value); }
    } pathRestorer{originalPath};
    qputenv("PATH", directory.path().toUtf8() + ':' + originalPath);

    rfm::core::LocalLinuxVolumeService service;
    std::optional<rfm::core::VolumeOperationResult> result;
    std::thread operation([&service, &result] {
        result = service.execute(requestFor(rfm::core::VolumeOperation::Mount));
    });
    QElapsedTimer startupTimer;
    startupTimer.start();
    while (!QFile::exists(marker) && startupTimer.elapsed() < 2000) {
        std::this_thread::yield();
    }
    if (!QFile::exists(marker)) {
        service.requestCancellation();
        operation.join();
        QFAIL("The fixture process did not start");
    }

    QElapsedTimer cancellationTimer;
    cancellationTimer.start();
    service.requestCancellation();
    operation.join();

    QVERIFY(cancellationTimer.elapsed() < 1000);
    QVERIFY(result.has_value());
    QCOMPARE(result->error, rfm::core::VolumeOperationError::Cancelled);
}

QTEST_GUILESS_MAIN(VolumeServiceTest)

#include "test_volume_service.moc"
