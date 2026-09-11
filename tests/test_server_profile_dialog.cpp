#include "remotefilemanager/app/ServerProfileDialog.hpp"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>

#include <utility>

class ServerProfileDialogTest final : public QObject
{
    Q_OBJECT

  private slots:
    void exposesGeneralAndCapabilitiesTabs();
    void startsWithCapabilitiesNotDetected();
    void displaysSupportedCopyDataAndExtensionsReadOnly();
    void displaysRuntimeCopyCapabilitiesAndEffectiveNativeMethod();
    void displaysClientMediatedFallbackWhenNoServerCopyIsAvailable();
    void displaysEffectiveMethodOnlyForCurrentRuntimeSession();
    void displaysUnsupportedCopyData_data();
    void displaysUnsupportedCopyData();
    void displaysStorageCapabilitiesAndOtherProviderAsNotApplicable();
};

void ServerProfileDialogTest::exposesGeneralAndCapabilitiesTabs()
{
    rfm::app::ServerProfileDialog dialog;
    auto* const tabs = dialog.findChild<QTabWidget*>(QStringLiteral("serverProfileTabs"));
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->tabText(0), QStringLiteral("General"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("Capabilities"));
    QVERIFY(dialog.findChild<QWidget*>(QStringLiteral("serverProfileGeneralTab")) != nullptr);
    QVERIFY(dialog.findChild<QWidget*>(QStringLiteral("serverProfileCapabilitiesTab")) != nullptr);

    dialog.setProfile({QStringLiteral("Server A"), QStringLiteral("a.example.test"),
                       QStringLiteral("alice"), 2222, QStringLiteral("profile-a")});
    dialog.findChild<QLineEdit*>(QStringLiteral("serverNameEdit"))
        ->setText(QStringLiteral("Updated server"));
    dialog.findChild<QLineEdit*>(QStringLiteral("serverHostEdit"))
        ->setText(QStringLiteral("updated.example.test"));

    QCOMPARE(dialog.profile().displayName, QStringLiteral("Updated server"));
    QCOMPARE(dialog.profile().host, QStringLiteral("updated.example.test"));
    QCOMPARE(dialog.profile().id, QStringLiteral("profile-a"));
    QVERIFY(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
}

void ServerProfileDialogTest::startsWithCapabilitiesNotDetected()
{
    rfm::app::ServerProfileDialog dialog;

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataStatusLabel"))->text(),
             QStringLiteral("Not detected"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("sftpProtocolVersionLabel"))->text(),
             QStringLiteral("Not detected"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("capabilitiesLastDetectedLabel"))->text(),
             QStringLiteral("Not detected"));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("capabilitiesContextLabel"))
                ->text()
                .startsWith(QStringLiteral("Not detected")));
    auto* const protocol =
        dialog.findChild<QLabel*>(QStringLiteral("copyDataProtocolLabel"));
    QVERIFY(protocol != nullptr);
    auto* const remoteCopyLayout = qobject_cast<QFormLayout*>(protocol->parentWidget()->layout());
    QVERIFY(remoteCopyLayout != nullptr);
    auto* const extensionLabel = qobject_cast<QLabel*>(remoteCopyLayout->labelForField(protocol));
    QVERIFY(extensionLabel != nullptr);
    QCOMPARE(extensionLabel->text(), QStringLiteral("SFTP extension:"));
    QCOMPARE(dialog.findChild<QTableWidget*>(QStringLiteral("sftpExtensionsTable"))->rowCount(), 0);
}

void ServerProfileDialogTest::displaysSupportedCopyDataAndExtensionsReadOnly()
{
    rfm::app::ServerProfileDialog dialog;
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("1")},
         {QStringLiteral("statvfs@openssh.com"), QStringLiteral("2")}},
        QDateTime::currentDateTimeUtc(), 3);
    dialog.setServerCapabilities(capabilities);

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("sftpProtocolVersionLabel"))->text(),
             QStringLiteral("3"));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("capabilitiesContextLabel"))
                ->text()
                .startsWith(QStringLiteral("Last known")));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("copyDataDescriptionLabel"))
                ->text()
                .contains(QStringLiteral("server advertises SFTP copy-data")));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataBackendStatusLabel"))->text(),
             QStringLiteral("Not detected"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("nativeCopyStatusLabel"))->text(),
             QStringLiteral("Not detected"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("effectiveCopyMethodLabel"))->text(),
             QStringLiteral("Not detected"));

    auto* const extensions = dialog.findChild<QTableWidget*>(QStringLiteral("sftpExtensionsTable"));
    QVERIFY(extensions != nullptr);
    QCOMPARE(extensions->editTriggers(), QAbstractItemView::NoEditTriggers);
    QCOMPARE(extensions->rowCount(), 2);
    QCOMPARE(extensions->item(0, 0)->text(), QStringLiteral("copy-data"));
    QCOMPARE(extensions->item(0, 1)->text(), QStringLiteral("1"));
    QCOMPARE(extensions->item(1, 0)->text(), QStringLiteral("statvfs@openssh.com"));
    QCOMPARE(extensions->item(1, 1)->text(), QStringLiteral("2"));
    QVERIFY(!extensions->item(0, 0)->flags().testFlag(Qt::ItemIsEditable));

    dialog.setServerCapabilities(capabilities, true);
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("capabilitiesContextLabel"))
                ->text()
                .startsWith(QStringLiteral("Currently detected")));
}

void ServerProfileDialogTest::displaysRuntimeCopyCapabilitiesAndEffectiveNativeMethod()
{
    rfm::app::ServerProfileDialog dialog;
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("1")}}, QDateTime::currentDateTimeUtc());
    const rfm::core::RemoteCopyExecutionCapabilities runtime{
        false, rfm::core::CapabilitySupport::Supported,
        rfm::core::NativeServerCopyPrimitive::PosixCp};
    dialog.setServerCapabilities(capabilities, true, runtime);

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataBackendStatusLabel"))->text(),
             QStringLiteral("Not supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("nativeCopyStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("effectiveCopyMethodLabel"))->text(),
             QStringLiteral("Native server copy (POSIX cp)"));
    const QString description =
        dialog.findChild<QLabel*>(QStringLiteral("copyDataDescriptionLabel"))->text();
    QVERIFY(description.contains(QStringLiteral("cannot invoke")));
    QVERIFY(!description.contains(QStringLiteral("without passing through this computer")));

    const rfm::core::RemoteCopyExecutionCapabilities sftpRuntime{
        true, rfm::core::CapabilitySupport::Supported,
        rfm::core::NativeServerCopyPrimitive::PosixCp};
    dialog.setServerCapabilities(capabilities, true, sftpRuntime);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataBackendStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("effectiveCopyMethodLabel"))->text(),
             QStringLiteral("SFTP copy-data"));
}

void ServerProfileDialogTest::displaysClientMediatedFallbackWhenNoServerCopyIsAvailable()
{
    rfm::app::ServerProfileDialog dialog;
    const auto capabilities =
        rfm::core::detectedServerCapabilities({}, QDateTime::currentDateTimeUtc());
    const rfm::core::RemoteCopyExecutionCapabilities runtime{
        false, rfm::core::CapabilitySupport::Unsupported,
        rfm::core::NativeServerCopyPrimitive::None};
    dialog.setServerCapabilities(capabilities, true, runtime);

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataStatusLabel"))->text(),
             QStringLiteral("Not supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataBackendStatusLabel"))->text(),
             QStringLiteral("Not supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("nativeCopyStatusLabel"))->text(),
             QStringLiteral("Not supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("effectiveCopyMethodLabel"))->text(),
             QStringLiteral("Client-mediated SFTP"));
}

void ServerProfileDialogTest::displaysEffectiveMethodOnlyForCurrentRuntimeSession()
{
    rfm::app::ServerProfileDialog dialog;
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("1")}}, QDateTime::currentDateTimeUtc());
    const rfm::core::RemoteCopyExecutionCapabilities runtime{
        true, rfm::core::CapabilitySupport::Supported,
        rfm::core::NativeServerCopyPrimitive::PosixCp};

    dialog.setServerCapabilities(capabilities, false, runtime);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataBackendStatusLabel"))->text(),
             QStringLiteral("Not detected"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("effectiveCopyMethodLabel"))->text(),
             QStringLiteral("Not detected"));
}

void ServerProfileDialogTest::displaysUnsupportedCopyData_data()
{
    QTest::addColumn<QList<rfm::core::SftpExtensionCapability>>("extensions");
    QTest::newRow("not-announced") << QList<rfm::core::SftpExtensionCapability>{
        {QStringLiteral("fsync@openssh.com"), QStringLiteral("1")}};
    QTest::newRow("other-revision") << QList<rfm::core::SftpExtensionCapability>{
        {QStringLiteral("copy-data"), QStringLiteral("2")}};
}

void ServerProfileDialogTest::displaysUnsupportedCopyData()
{
    QFETCH(QList<rfm::core::SftpExtensionCapability>, extensions);
    rfm::app::ServerProfileDialog dialog;
    dialog.setServerCapabilities(rfm::core::detectedServerCapabilities(
        std::move(extensions), QDateTime::currentDateTimeUtc(), 3));

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataStatusLabel"))->text(),
             QStringLiteral("Not supported"));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("copyDataDescriptionLabel"))
                ->text()
                .contains(QStringLiteral("does not advertise")));
}

void ServerProfileDialogTest::displaysStorageCapabilitiesAndOtherProviderAsNotApplicable()
{
    rfm::app::ServerProfileDialog dialog;
    auto capabilities = rfm::core::detectedServerCapabilities({}, QDateTime::currentDateTimeUtc());
    capabilities.storage.detectionState = rfm::core::CapabilityDetectionState::Detected;
    capabilities.storage.windowsPowerShell = rfm::core::CapabilitySupport::Supported;
    capabilities.storage.windowsGetVolume = rfm::core::CapabilitySupport::Supported;
    capabilities.storage.windowsGetDisk = rfm::core::CapabilitySupport::Unsupported;
    capabilities.storage.provider = rfm::core::selectRemoteStorageProvider(capabilities.storage);
    dialog.setServerCapabilities(capabilities, true);

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageDiscoveryStatusLabel"))->text(),
             QStringLiteral("Completed"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageVolumeListingStatusLabel"))->text(),
             QStringLiteral("Not implemented"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageProviderLabel"))->text(),
             QStringLiteral("Windows PowerShell"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("linuxMountInfoStatusLabel"))->text(),
             QStringLiteral("Not applicable"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("lsblkStatusLabel"))->text(),
             QStringLiteral("Not applicable"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("windowsPowerShellStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("windowsGetVolumeStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("windowsGetDiskStatusLabel"))->text(),
             QStringLiteral("Not supported"));

    capabilities.storage = {};
    capabilities.storage.detectionState = rfm::core::CapabilityDetectionState::Detected;
    capabilities.storage.linuxMountInfo = rfm::core::CapabilitySupport::Supported;
    capabilities.storage.lsblk = rfm::core::CapabilitySupport::Supported;
    capabilities.storage.provider = rfm::core::selectRemoteStorageProvider(capabilities.storage);
    dialog.setServerCapabilities(capabilities, true);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageProviderLabel"))->text(),
             QStringLiteral("Linux"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageVolumeListingStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("linuxMountInfoStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("lsblkStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("windowsPowerShellStatusLabel"))->text(),
             QStringLiteral("Not applicable"));
}

QTEST_MAIN(ServerProfileDialogTest)

#include "test_server_profile_dialog.moc"
