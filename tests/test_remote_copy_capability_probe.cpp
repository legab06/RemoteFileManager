#include "remotefilemanager/ssh/RemoteCopyCapabilityProbe.hpp"

#include <QTest>

class RemoteCopyCapabilityProbeTest final : public QObject
{
    Q_OBJECT

  private slots:
    void usesFixedReadOnlyProbeAndDeclaresBackendLimitation();
    void exitZeroSupportsPosixCp();
    void nonZeroExitDoesNotSupportPosixCp();
    void transportOrExecutionFailureStaysUnknown_data();
    void transportOrExecutionFailureStaysUnknown();
};

void RemoteCopyCapabilityProbeTest::usesFixedReadOnlyProbeAndDeclaresBackendLimitation()
{
    QCOMPARE(rfm::ssh::RemoteCopyCapabilityProbe::command(),
             QStringLiteral("command -v cp >/dev/null 2>&1"));
    QVERIFY(!rfm::ssh::RemoteCopyCapabilityProbe::sftpCopyDataAvailable);
}

void RemoteCopyCapabilityProbeTest::exitZeroSupportsPosixCp()
{
    const auto capabilities = rfm::ssh::RemoteCopyCapabilityProbe::capabilitiesFrom(
        {true, false, false, 0, {}, {}, false});
    QCOMPARE(capabilities.nativeServerCopy, rfm::core::CapabilitySupport::Supported);
    QCOMPARE(capabilities.nativePrimitive, rfm::core::NativeServerCopyPrimitive::PosixCp);
    QVERIFY(!capabilities.sftpCopyDataAvailable);
}

void RemoteCopyCapabilityProbeTest::nonZeroExitDoesNotSupportPosixCp()
{
    const auto capabilities = rfm::ssh::RemoteCopyCapabilityProbe::capabilitiesFrom(
        {true, false, false, 127, {}, {}, false});
    QCOMPARE(capabilities.nativeServerCopy, rfm::core::CapabilitySupport::Unsupported);
    QCOMPARE(capabilities.nativePrimitive, rfm::core::NativeServerCopyPrimitive::None);
}

void RemoteCopyCapabilityProbeTest::transportOrExecutionFailureStaysUnknown_data()
{
    QTest::addColumn<bool>("started");
    QTest::addColumn<bool>("timedOut");
    QTest::addColumn<bool>("crashed");
    QTest::addColumn<int>("exitCode");

    QTest::newRow("transport-start-failure") << false << false << false << -1;
    QTest::newRow("timeout") << true << true << false << -1;
    QTest::newRow("ssh-read-error") << true << false << true << -1;
    QTest::newRow("no-exit-status") << true << false << false << -1;
}

void RemoteCopyCapabilityProbeTest::transportOrExecutionFailureStaysUnknown()
{
    QFETCH(bool, started);
    QFETCH(bool, timedOut);
    QFETCH(bool, crashed);
    QFETCH(int, exitCode);
    const auto capabilities = rfm::ssh::RemoteCopyCapabilityProbe::capabilitiesFrom(
        {started, timedOut, crashed, exitCode, {}, {}, false});
    QCOMPARE(capabilities.nativeServerCopy, rfm::core::CapabilitySupport::Unknown);
    QCOMPARE(capabilities.nativePrimitive, rfm::core::NativeServerCopyPrimitive::None);
}

QTEST_APPLESS_MAIN(RemoteCopyCapabilityProbeTest)

#include "test_remote_copy_capability_probe.moc"
