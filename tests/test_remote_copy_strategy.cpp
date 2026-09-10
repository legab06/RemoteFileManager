#include "remotefilemanager/core/RemoteCopyStrategy.hpp"

#include <QDateTime>
#include <QTest>

class RemoteCopyStrategyTest final : public QObject
{
    Q_OBJECT

  private slots:
    void fallsBackForUndetectedCapabilities();
    void fallsBackWhenCopyDataIsAbsent();
    void selectsSftpCopyDataForCopyDataRevisionOne();
    void fallsBackForOtherCopyDataRevisions();
    void fallsBackForExplicitlyUnknownCapability();
    void ignoresUnrelatedSftpExtensions();
    void requiresNoHostOrOperatingSystemMetadata();
};

void RemoteCopyStrategyTest::fallsBackForUndetectedCapabilities()
{
    QCOMPARE(rfm::core::selectRemoteCopyMethod({}),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);

    rfm::core::ServerCapabilities incompleteCapabilities;
    incompleteCapabilities.copyDataVersion1 = rfm::core::CapabilitySupport::Supported;
    QCOMPARE(rfm::core::selectRemoteCopyMethod(incompleteCapabilities),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::fallsBackWhenCopyDataIsAbsent()
{
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("fsync@openssh.com"), QStringLiteral("1")}},
        QDateTime::currentDateTimeUtc());

    QCOMPARE(rfm::core::selectRemoteCopyMethod(capabilities),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::selectsSftpCopyDataForCopyDataRevisionOne()
{
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("1")}}, QDateTime::currentDateTimeUtc());

    QCOMPARE(rfm::core::selectRemoteCopyMethod(capabilities),
             rfm::core::RemoteCopyMethod::SftpCopyData);
}

void RemoteCopyStrategyTest::fallsBackForOtherCopyDataRevisions()
{
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("2")}}, QDateTime::currentDateTimeUtc());

    QCOMPARE(rfm::core::selectRemoteCopyMethod(capabilities),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::fallsBackForExplicitlyUnknownCapability()
{
    rfm::core::ServerCapabilities capabilities;
    capabilities.detectionState = rfm::core::CapabilityDetectionState::Detected;
    capabilities.copyDataVersion1 = rfm::core::CapabilitySupport::Unknown;

    QCOMPARE(rfm::core::selectRemoteCopyMethod(capabilities),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::ignoresUnrelatedSftpExtensions()
{
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("fsync@openssh.com"), QStringLiteral("2")},
         {QStringLiteral("vendor-copy@server.example"), QStringLiteral("1")}},
        QDateTime::currentDateTimeUtc());

    QCOMPARE(rfm::core::selectRemoteCopyMethod(capabilities),
             rfm::core::RemoteCopyMethod::ClientMediatedSftp);
}

void RemoteCopyStrategyTest::requiresNoHostOrOperatingSystemMetadata()
{
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("1")}}, QDateTime::currentDateTimeUtc(), 3);

    QCOMPARE(rfm::core::selectRemoteCopyMethod(capabilities),
             rfm::core::RemoteCopyMethod::SftpCopyData);
}

QTEST_APPLESS_MAIN(RemoteCopyStrategyTest)

#include "test_remote_copy_strategy.moc"
