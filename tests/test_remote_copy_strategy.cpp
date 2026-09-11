#include "remotefilemanager/core/RemoteCopyStrategy.hpp"

#include <QDateTime>
#include <QTest>

namespace
{

rfm::core::ServerCapabilities copyDataServer()
{
    return rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("1")}}, QDateTime::currentDateTimeUtc());
}

rfm::core::RemoteCopyExecutionCapabilities
executionCapabilities(bool copyDataAvailable, rfm::core::CapabilitySupport nativeSupport,
                      rfm::core::NativeServerCopyPrimitive primitive)
{
    return {copyDataAvailable, nativeSupport, primitive};
}

} // namespace

class RemoteCopyStrategyTest final : public QObject
{
    Q_OBJECT

  private slots:
    void selectsSftpCopyDataOnlyWhenServerAndBackendSupportIt();
    void selectsNativeCopyWhenCopyDataCannotBeExecuted();
    void fallsBackWhenCopyDataAndNativeCopyAreUnavailable();
    void selectsNativeCopyWithoutCopyData();
    void fallsBackForUnknownCapabilities();
    void fallsBackForUnsupportedNativeCopy();
    void fallsBackForUnknownNativeCopy();
    void undetectedServerCannotEnableCopyData();
    void requiresAnIdentifiedNativePrimitive();
    void requiresNoHostOrOperatingSystemMetadata();
};

void RemoteCopyStrategyTest::selectsSftpCopyDataOnlyWhenServerAndBackendSupportIt()
{
    QCOMPARE(
        rfm::core::selectRemoteCopyMethod(
            copyDataServer(), executionCapabilities(true, rfm::core::CapabilitySupport::Supported,
                                                    rfm::core::NativeServerCopyPrimitive::PosixCp)),
        rfm::core::RemoteCopyMethod::SftpCopyData);
}

void RemoteCopyStrategyTest::selectsNativeCopyWhenCopyDataCannotBeExecuted()
{
    QCOMPARE(
        rfm::core::selectRemoteCopyMethod(
            copyDataServer(), executionCapabilities(false, rfm::core::CapabilitySupport::Supported,
                                                    rfm::core::NativeServerCopyPrimitive::PosixCp)),
        rfm::core::RemoteCopyMethod::NativeServerCopy);
}

void RemoteCopyStrategyTest::fallsBackWhenCopyDataAndNativeCopyAreUnavailable()
{
    QCOMPARE(rfm::core::selectRemoteCopyMethod(
                 copyDataServer(),
                 executionCapabilities(false, rfm::core::CapabilitySupport::Unsupported,
                                       rfm::core::NativeServerCopyPrimitive::None)),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::selectsNativeCopyWithoutCopyData()
{
    const auto server = rfm::core::detectedServerCapabilities({}, QDateTime::currentDateTimeUtc());
    QCOMPARE(rfm::core::selectRemoteCopyMethod(
                 server, executionCapabilities(false, rfm::core::CapabilitySupport::Supported,
                                               rfm::core::NativeServerCopyPrimitive::PosixCp)),
             rfm::core::RemoteCopyMethod::NativeServerCopy);
}

void RemoteCopyStrategyTest::fallsBackForUnknownCapabilities()
{
    rfm::core::ServerCapabilities server;
    server.detectionState = rfm::core::CapabilityDetectionState::Detected;
    QCOMPARE(rfm::core::selectRemoteCopyMethod(server, {}),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::fallsBackForUnsupportedNativeCopy()
{
    QCOMPARE(rfm::core::selectRemoteCopyMethod(
                 {}, executionCapabilities(false, rfm::core::CapabilitySupport::Unsupported,
                                           rfm::core::NativeServerCopyPrimitive::None)),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::fallsBackForUnknownNativeCopy()
{
    QCOMPARE(
        rfm::core::selectRemoteCopyMethod(
            copyDataServer(), executionCapabilities(false, rfm::core::CapabilitySupport::Unknown,
                                                    rfm::core::NativeServerCopyPrimitive::None)),
        rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::undetectedServerCannotEnableCopyData()
{
    rfm::core::ServerCapabilities server;
    server.copyDataVersion1 = rfm::core::CapabilitySupport::Supported;
    QCOMPARE(rfm::core::selectRemoteCopyMethod(
                 server, executionCapabilities(true, rfm::core::CapabilitySupport::Unknown,
                                               rfm::core::NativeServerCopyPrimitive::None)),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::requiresAnIdentifiedNativePrimitive()
{
    QCOMPARE(rfm::core::selectRemoteCopyMethod(
                 {}, executionCapabilities(false, rfm::core::CapabilitySupport::Supported,
                                           rfm::core::NativeServerCopyPrimitive::None)),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::requiresNoHostOrOperatingSystemMetadata()
{
    const auto server = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("1")}}, QDateTime::currentDateTimeUtc(), 3);
    QCOMPARE(rfm::core::selectRemoteCopyMethod(
                 server, executionCapabilities(false, rfm::core::CapabilitySupport::Supported,
                                               rfm::core::NativeServerCopyPrimitive::PosixCp)),
             rfm::core::RemoteCopyMethod::NativeServerCopy);
}

QTEST_APPLESS_MAIN(RemoteCopyStrategyTest)

#include "test_remote_copy_strategy.moc"
