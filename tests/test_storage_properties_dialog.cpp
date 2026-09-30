#include "remotefilemanager/app/StoragePropertiesDialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QTest>

#include <limits>
#include <optional>

namespace
{

QString formattedSize(quint64 bytes)
{
    return QLocale{}.formattedDataSize(static_cast<qint64>(bytes), 1,
                                       QLocale::DataSizeTraditionalFormat);
}

rfm::core::StorageVolume completeVolume()
{
    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("System");
    volume.fileSystemLabel = QStringLiteral("Primary");
    volume.device = QStringLiteral("/dev/nvme0n1p2");
    volume.deviceModel = QStringLiteral("Fixture SSD");
    volume.fileSystemType = QByteArrayLiteral("btrfs");
    volume.rootPath = QStringLiteral("/");
    volume.mounted = true;
    volume.readOnly = false;
    volume.bytesTotal = 1000;
    volume.bytesAvailable = 250;
    return volume;
}

} // namespace

class StoragePropertiesDialogTest final : public QObject
{
    Q_OBJECT

  private slots:
    void showsMetadataAndUsage();
    void showsEmptyAndFullUsage_data();
    void showsEmptyAndFullUsage();
    void hidesUnavailableOrInvalidUsage_data();
    void hidesUnavailableOrInvalidUsage();
    void showsUnmountedMetadataWithoutMountStatistics();
    void omitsUnavailableMetadataAndUnrepresentableSizes();
};

void StoragePropertiesDialogTest::showsMetadataAndUsage()
{
    const rfm::core::StorageVolume volume = completeVolume();
    rfm::app::StoragePropertiesDialog dialog(volume);

    QCOMPARE(dialog.objectName(), QStringLiteral("storagePropertiesDialog"));
    QVERIFY(dialog.isModal());
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Properties — System"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageNameValue"))->text(),
             volume.displayName);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageLabelValue"))->text(),
             volume.fileSystemLabel);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageDeviceValue"))->text(),
             volume.device);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageModelValue"))->text(),
             volume.deviceModel);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageFilesystemValue"))->text(),
             QStringLiteral("btrfs"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageMountPointValue"))->text(),
             volume.rootPath);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageStatusValue"))->text(),
             QStringLiteral("Mounted"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageAccessValue"))->text(),
             QStringLiteral("Writable"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageCapacityValue"))->text(),
             formattedSize(1000));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageUsedValue"))->text(),
             formattedSize(750));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageAvailableValue"))->text(),
             formattedSize(250));

    auto* const usageBar = dialog.findChild<QProgressBar*>(QStringLiteral("storageUsageBar"));
    QVERIFY(usageBar != nullptr);
    QCOMPARE(usageBar->minimum(), 0);
    QCOMPARE(usageBar->maximum(), 100);
    QCOMPARE(usageBar->value(), 75);
    QVERIFY(!usageBar->isTextVisible());
    QCOMPARE(usageBar->accessibleDescription(), QStringLiteral("75% used"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageUsageLabel"))->text(),
             QStringLiteral("75% used"));
    QVERIFY(dialog.findChild<QDialogButtonBox*>(QStringLiteral("storagePropertiesButtons")) !=
            nullptr);
}

void StoragePropertiesDialogTest::showsEmptyAndFullUsage_data()
{
    QTest::addColumn<qulonglong>("available");
    QTest::addColumn<qulonglong>("used");
    QTest::addColumn<int>("percentage");

    QTest::newRow("empty") << qulonglong{1000} << qulonglong{0} << 0;
    QTest::newRow("full") << qulonglong{0} << qulonglong{1000} << 100;
}

void StoragePropertiesDialogTest::showsEmptyAndFullUsage()
{
    QFETCH(qulonglong, available);
    QFETCH(qulonglong, used);
    QFETCH(int, percentage);

    rfm::core::StorageVolume volume = completeVolume();
    volume.bytesAvailable = static_cast<quint64>(available);
    rfm::app::StoragePropertiesDialog dialog(volume);

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageUsedValue"))->text(),
             formattedSize(static_cast<quint64>(used)));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageAvailableValue"))->text(),
             formattedSize(static_cast<quint64>(available)));
    QCOMPARE(dialog.findChild<QProgressBar*>(QStringLiteral("storageUsageBar"))->value(),
             percentage);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageUsageLabel"))->text(),
             QStringLiteral("%1% used").arg(percentage));
}

void StoragePropertiesDialogTest::hidesUnavailableOrInvalidUsage_data()
{
    QTest::addColumn<int>("scenario");

    QTest::newRow("availability unknown") << 0;
    QTest::newRow("zero total") << 1;
    QTest::newRow("availability exceeds total") << 2;
}

void StoragePropertiesDialogTest::hidesUnavailableOrInvalidUsage()
{
    QFETCH(int, scenario);

    rfm::core::StorageVolume volume = completeVolume();
    if (scenario == 0) {
        volume.bytesAvailable = std::nullopt;
    } else if (scenario == 1) {
        volume.bytesTotal = 0;
        volume.bytesAvailable = 0;
    } else {
        volume.bytesAvailable = volume.bytesTotal + 1;
    }
    rfm::app::StoragePropertiesDialog dialog(volume);

    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageUsedValue")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageAvailableValue")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageUsageBar")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageUsageLabel")) == nullptr);
}

void StoragePropertiesDialogTest::showsUnmountedMetadataWithoutMountStatistics()
{
    rfm::core::StorageVolume volume = completeVolume();
    volume.mounted = false;
    volume.readOnly = true;
    volume.rootPath = QStringLiteral("/must-not-be-presented");
    rfm::app::StoragePropertiesDialog dialog(volume);

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageDeviceValue"))->text(),
             volume.device);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageStatusValue"))->text(),
             QStringLiteral("Not mounted"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("storageAccessValue"))->text(),
             QStringLiteral("Read-only"));
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageMountPointValue")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageUsedValue")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageAvailableValue")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageUsageBar")) == nullptr);
}

void StoragePropertiesDialogTest::omitsUnavailableMetadataAndUnrepresentableSizes()
{
    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("Unknown");
    volume.fileSystemLabel = QStringLiteral("  ");
    volume.device = QStringLiteral("unknown");
    volume.deviceModel = QStringLiteral("Unknown");
    volume.mounted = false;
    volume.bytesTotal = std::numeric_limits<quint64>::max();
    rfm::app::StoragePropertiesDialog dialog(volume);

    QCOMPARE(dialog.windowTitle(), QStringLiteral("Properties — Storage volume"));
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageNameValue")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageLabelValue")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageDeviceValue")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageModelValue")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageFilesystemValue")) == nullptr);
    QVERIFY(dialog.findChild<QObject*>(QStringLiteral("storageCapacityValue")) == nullptr);
}

QTEST_MAIN(StoragePropertiesDialogTest)

#include "test_storage_properties_dialog.moc"
