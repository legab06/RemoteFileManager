#include "remotefilemanager/ssh/RemoteStorageCapabilityProbe.hpp"

#include <libssh/sftp.h>

#include <QTest>

#include <utility>

namespace
{

rfm::core::VolumeCommandResult commandResult(QString output, int exitCode = 0)
{
    return {true, false, false, exitCode, std::move(output), {}, false};
}

} // namespace

class RemoteStorageCapabilityProbeTest final : public QObject
{
    Q_OBJECT

  private slots:
    void acceptsOnlyMarkedPosixProbe();
    void mapsMountInfoAvailabilityConservatively();
    void detectsMarkedWindowsPowerShellCommands();
    void doesNotTrustUnmarkedWindowsOutput();
    void selectsProvidersFromRuntimeProof();
    void reportsNoProviderAsUnsupportedDiscovery();
    void completesWindowsLifecycleWithoutLinuxScanner();
    void completesLifecycleWithoutAnyProvider();
    void resetsLifecycleForReconnect();
};

void RemoteStorageCapabilityProbeTest::acceptsOnlyMarkedPosixProbe()
{
    const auto withLsblk = rfm::ssh::RemoteStorageCapabilityProbe::applyPosixResult(
        {}, commandResult(QStringLiteral("RFM_POSIX_STORAGE_V1\nlsblk\nmount\n")));
    QCOMPARE(withLsblk.lsblk, rfm::core::CapabilitySupport::Supported);

    const auto withoutLsblk = rfm::ssh::RemoteStorageCapabilityProbe::applyPosixResult(
        {}, commandResult(QStringLiteral("RFM_POSIX_STORAGE_V1\nmount\n")));
    QCOMPARE(withoutLsblk.lsblk, rfm::core::CapabilitySupport::Unsupported);

    const auto unmarked = rfm::ssh::RemoteStorageCapabilityProbe::applyPosixResult(
        {}, commandResult(QStringLiteral("lsblk\nmount\n")));
    QCOMPARE(unmarked.lsblk, rfm::core::CapabilitySupport::Unknown);
    QVERIFY(rfm::ssh::RemoteStorageCapabilityProbe::posixCommand().contains(
        QStringLiteral("RFM_POSIX_STORAGE_V1")));
}

void RemoteStorageCapabilityProbeTest::mapsMountInfoAvailabilityConservatively()
{
    QCOMPARE(rfm::ssh::RemoteStorageCapabilityProbe::mountInfoCapabilityFromSftpStatus(0, true),
             rfm::core::CapabilitySupport::Supported);
    QCOMPARE(rfm::ssh::RemoteStorageCapabilityProbe::mountInfoCapabilityFromSftpStatus(
                 SSH_FX_NO_SUCH_FILE, false),
             rfm::core::CapabilitySupport::Unsupported);
    QCOMPARE(rfm::ssh::RemoteStorageCapabilityProbe::mountInfoCapabilityFromSftpStatus(
                 SSH_FX_NO_SUCH_PATH, false),
             rfm::core::CapabilitySupport::Unsupported);
    QCOMPARE(rfm::ssh::RemoteStorageCapabilityProbe::mountInfoCapabilityFromSftpStatus(
                 SSH_FX_PERMISSION_DENIED, false),
             rfm::core::CapabilitySupport::Unknown);
    QCOMPARE(rfm::ssh::RemoteStorageCapabilityProbe::mountInfoCapabilityFromSftpStatus(
                 SSH_FX_CONNECTION_LOST, false),
             rfm::core::CapabilitySupport::Unknown);
}

void RemoteStorageCapabilityProbeTest::detectsMarkedWindowsPowerShellCommands()
{
    const auto capabilities = rfm::ssh::RemoteStorageCapabilityProbe::applyWindowsResult(
        {}, commandResult(QStringLiteral("RFM_WINDOWS_STORAGE_V1\npowershell\nGet-Volume\n")));
    QCOMPARE(capabilities.windowsPowerShell, rfm::core::CapabilitySupport::Supported);
    QCOMPARE(capabilities.windowsGetVolume, rfm::core::CapabilitySupport::Supported);
    QCOMPARE(capabilities.windowsGetDisk, rfm::core::CapabilitySupport::Unsupported);
    const auto withoutVolume = rfm::ssh::RemoteStorageCapabilityProbe::applyWindowsResult(
        {}, commandResult(QStringLiteral("RFM_WINDOWS_STORAGE_V1\npowershell\nGet-Disk\n")));
    QCOMPARE(withoutVolume.windowsGetVolume, rfm::core::CapabilitySupport::Unsupported);
    QCOMPARE(withoutVolume.windowsGetDisk, rfm::core::CapabilitySupport::Supported);
    QVERIFY(rfm::ssh::RemoteStorageCapabilityProbe::windowsPowerShellCommand().contains(
        QStringLiteral("-NonInteractive")));
}

void RemoteStorageCapabilityProbeTest::doesNotTrustUnmarkedWindowsOutput()
{
    const auto capabilities = rfm::ssh::RemoteStorageCapabilityProbe::applyWindowsResult(
        {}, commandResult(QStringLiteral("powershell\nGet-Volume\nGet-Disk\n")));
    QCOMPARE(capabilities.windowsPowerShell, rfm::core::CapabilitySupport::Unknown);
    QCOMPARE(capabilities.windowsGetVolume, rfm::core::CapabilitySupport::Unknown);
    QCOMPARE(capabilities.windowsGetDisk, rfm::core::CapabilitySupport::Unknown);
}

void RemoteStorageCapabilityProbeTest::selectsProvidersFromRuntimeProof()
{
    rfm::core::RemoteStorageCapabilities linux;
    linux.linuxMountInfo = rfm::core::CapabilitySupport::Supported;
    linux = rfm::ssh::RemoteStorageCapabilityProbe::finalized(linux);
    QCOMPARE(linux.provider, rfm::core::RemoteStorageProvider::Linux);
    QVERIFY(rfm::core::storageDiscoverySupported(linux));

    rfm::core::RemoteStorageCapabilities windows;
    windows.windowsPowerShell = rfm::core::CapabilitySupport::Supported;
    windows.windowsGetVolume = rfm::core::CapabilitySupport::Supported;
    windows = rfm::ssh::RemoteStorageCapabilityProbe::finalized(windows);
    QCOMPARE(windows.provider, rfm::core::RemoteStorageProvider::WindowsPowerShell);
    QVERIFY(rfm::core::storageDiscoverySupported(windows));
}

void RemoteStorageCapabilityProbeTest::reportsNoProviderAsUnsupportedDiscovery()
{
    rfm::core::RemoteStorageCapabilities windowsLike;
    windowsLike.linuxMountInfo = rfm::core::CapabilitySupport::Unsupported;
    windowsLike.lsblk = rfm::core::CapabilitySupport::Unknown;
    windowsLike = rfm::ssh::RemoteStorageCapabilityProbe::finalized(windowsLike);
    QCOMPARE(windowsLike.provider, rfm::core::RemoteStorageProvider::None);
    QVERIFY(!rfm::core::storageDiscoverySupported(windowsLike));
}

void RemoteStorageCapabilityProbeTest::completesWindowsLifecycleWithoutLinuxScanner()
{
    rfm::ssh::RemoteStorageCapabilityLifecycle lifecycle;
    QCOMPARE(lifecycle.stage(), rfm::ssh::RemoteStorageCapabilityProbeStage::Posix);
    lifecycle.recordPosixResult(commandResult(QStringLiteral("lsblk\n"), 1));
    QCOMPARE(lifecycle.stage(),
             rfm::ssh::RemoteStorageCapabilityProbeStage::WindowsPowerShell);
    lifecycle.recordWindowsResult(commandResult(
        QStringLiteral("RFM_WINDOWS_STORAGE_V1\nGet-Volume\nGet-Disk\n")));
    QCOMPARE(lifecycle.stage(), rfm::ssh::RemoteStorageCapabilityProbeStage::MountInfo);
    lifecycle.recordMountInfo(rfm::core::CapabilitySupport::Unsupported);

    QCOMPARE(lifecycle.stage(), rfm::ssh::RemoteStorageCapabilityProbeStage::Complete);
    const auto& capabilities = lifecycle.capabilities();
    QCOMPARE(capabilities.detectionState, rfm::core::CapabilityDetectionState::Detected);
    QCOMPARE(capabilities.provider, rfm::core::RemoteStorageProvider::WindowsPowerShell);
    QCOMPARE(capabilities.windowsPowerShell, rfm::core::CapabilitySupport::Supported);
    QCOMPARE(capabilities.windowsGetVolume, rfm::core::CapabilitySupport::Supported);
    QCOMPARE(capabilities.windowsGetDisk, rfm::core::CapabilitySupport::Supported);
    QVERIFY(!rfm::ssh::RemoteStorageCapabilityProbe::linuxScannerApplicable(capabilities));
}

void RemoteStorageCapabilityProbeTest::completesLifecycleWithoutAnyProvider()
{
    rfm::ssh::RemoteStorageCapabilityLifecycle lifecycle;
    lifecycle.recordPosixResult(commandResult(QStringLiteral("not a POSIX marker\n"), 1));
    lifecycle.recordWindowsResult(commandResult(QStringLiteral("not a Windows marker\n"), 1));
    lifecycle.recordMountInfo(rfm::core::CapabilitySupport::Unsupported);

    QCOMPARE(lifecycle.stage(), rfm::ssh::RemoteStorageCapabilityProbeStage::Complete);
    QCOMPARE(lifecycle.capabilities().detectionState,
             rfm::core::CapabilityDetectionState::Detected);
    QCOMPARE(lifecycle.capabilities().provider, rfm::core::RemoteStorageProvider::None);
    QVERIFY(!rfm::ssh::RemoteStorageCapabilityProbe::linuxScannerApplicable(
        lifecycle.capabilities()));
}

void RemoteStorageCapabilityProbeTest::resetsLifecycleForReconnect()
{
    rfm::ssh::RemoteStorageCapabilityLifecycle lifecycle;
    lifecycle.skipPosix();
    lifecycle.skipWindows();
    lifecycle.recordMountInfo(rfm::core::CapabilitySupport::Supported);
    QCOMPARE(lifecycle.stage(), rfm::ssh::RemoteStorageCapabilityProbeStage::Complete);

    lifecycle.reset();
    QCOMPARE(lifecycle.stage(), rfm::ssh::RemoteStorageCapabilityProbeStage::Posix);
    QCOMPARE(lifecycle.capabilities().detectionState,
             rfm::core::CapabilityDetectionState::NotDetected);
    QCOMPARE(lifecycle.capabilities().provider, rfm::core::RemoteStorageProvider::None);
}

QTEST_APPLESS_MAIN(RemoteStorageCapabilityProbeTest)

#include "test_remote_storage_capability_probe.moc"
