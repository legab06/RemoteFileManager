#include "remotefilemanager/core/ServerCapabilities.hpp"

#include <QDateTime>
#include <QTest>

class ServerCapabilitiesTest final : public QObject
{
    Q_OBJECT

  private slots:
    void startsUndetectedWithUnknownCapabilities();
    void supportsCopyDataRevisionOne();
    void reportsMissingCopyDataAsUnsupported();
    void rejectsOtherCopyDataRevisions();
    void preservesAnnouncedExtensions();
};

void ServerCapabilitiesTest::startsUndetectedWithUnknownCapabilities()
{
    const rfm::core::ServerCapabilities capabilities;

    QCOMPARE(capabilities.detectionState, rfm::core::CapabilityDetectionState::NotDetected);
    QCOMPARE(capabilities.copyDataVersion1, rfm::core::CapabilitySupport::Unknown);
    QVERIFY(!capabilities.detectedAt.isValid());
    QVERIFY(!capabilities.sftpProtocolVersion.has_value());
    QVERIFY(capabilities.sftpExtensions.isEmpty());
}

void ServerCapabilitiesTest::supportsCopyDataRevisionOne()
{
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("1")}}, QDateTime::currentDateTimeUtc());

    QCOMPARE(capabilities.detectionState, rfm::core::CapabilityDetectionState::Detected);
    QCOMPARE(capabilities.copyDataVersion1, rfm::core::CapabilitySupport::Supported);
}

void ServerCapabilitiesTest::reportsMissingCopyDataAsUnsupported()
{
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("fsync@openssh.com"), QStringLiteral("1")}},
        QDateTime::currentDateTimeUtc());

    QCOMPARE(capabilities.copyDataVersion1, rfm::core::CapabilitySupport::Unsupported);
}

void ServerCapabilitiesTest::rejectsOtherCopyDataRevisions()
{
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("2")}}, QDateTime::currentDateTimeUtc());

    QCOMPARE(capabilities.copyDataVersion1, rfm::core::CapabilitySupport::Unsupported);
}

void ServerCapabilitiesTest::preservesAnnouncedExtensions()
{
    const QDateTime detectedAt = QDateTime::currentDateTimeUtc();
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("1")},
         {QStringLiteral("vendor-extension@example.test"), QStringLiteral("revision-alpha")}},
        detectedAt, 3);

    QCOMPARE(capabilities.detectedAt, detectedAt);
    QCOMPARE(capabilities.sftpProtocolVersion, std::optional<int>{3});
    QCOMPARE(capabilities.sftpExtensions.size(), 2);
    QCOMPARE(capabilities.sftpExtensions.at(0).name, QStringLiteral("copy-data"));
    QCOMPARE(capabilities.sftpExtensions.at(0).data, QStringLiteral("1"));
    QCOMPARE(capabilities.sftpExtensions.at(1).name,
             QStringLiteral("vendor-extension@example.test"));
    QCOMPARE(capabilities.sftpExtensions.at(1).data, QStringLiteral("revision-alpha"));
}

QTEST_APPLESS_MAIN(ServerCapabilitiesTest)

#include "test_server_capabilities.moc"
