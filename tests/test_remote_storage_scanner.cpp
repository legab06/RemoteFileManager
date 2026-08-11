#include "remotefilemanager/ssh/RemoteStorageScanner.hpp"

#include <QTest>

#include <algorithm>
#include <memory>
#include <utility>

namespace
{

class FakeRemoteStorageReader final : public rfm::ssh::RemoteStorageReader
{
  public:
    rfm::ssh::RemoteStorageError beginMountInfo() override
    {
        ++mountInfoOpens;
        mountInfoOpen = true;
        mountInfoOffset = 0;
        return rfm::ssh::RemoteStorageError::None;
    }

    rfm::ssh::RemoteStorageChunkResult readMountInfoChunk(qsizetype maximumBytes) override
    {
        ++mountInfoReads;
        if (!mountInfoOpen || maximumBytes <= 0) {
            return {{}, rfm::ssh::RemoteStorageError::OtherError, false};
        }
        if (mountInfoError != rfm::ssh::RemoteStorageError::None) {
            return {{}, mountInfoError, false};
        }
        if (mountInfoOffset >= mountInfo.size()) {
            return {{}, rfm::ssh::RemoteStorageError::None, true};
        }
        const qsizetype count =
            std::min({maximumBytes, mountInfoFragmentSize, mountInfo.size() - mountInfoOffset});
        const QByteArray chunk = mountInfo.sliced(mountInfoOffset, count);
        mountInfoOffset += count;
        return {chunk, rfm::ssh::RemoteStorageError::None, false};
    }

    void endMountInfo() override
    {
        if (mountInfoOpen) {
            mountInfoOpen = false;
            ++mountInfoCloses;
        }
    }

    rfm::ssh::RemoteStorageByteResult readFile(const QString& path, qsizetype maximumBytes) override
    {
        ++fileReads;
        Q_UNUSED(path)
        Q_UNUSED(maximumBytes)
        return {{}, rfm::ssh::RemoteStorageError::NotFound, false};
    }

    rfm::ssh::RemoteStorageStringResult readLink(const QString& path) override
    {
        ++linkReads;
        if (path.startsWith(QStringLiteral("/sys/dev/block/"))) {
            return {QStringLiteral("../../devices/pci0000:00/usb1/1-1/block/sdb/sdb1"),
                    rfm::ssh::RemoteStorageError::None};
        }
        return {{}, rfm::ssh::RemoteStorageError::NotFound};
    }

    rfm::ssh::RemoteStorageError beginFileSystemLabels() override
    {
        labelsOpen = labelsAvailable;
        return labelsAvailable ? rfm::ssh::RemoteStorageError::None
                               : rfm::ssh::RemoteStorageError::NotFound;
    }

    rfm::ssh::RemoteStorageLabelResult nextFileSystemLabel() override
    {
        ++labelReads;
        if (labelIndex >= labels.size()) {
            return {{}, {}, rfm::ssh::RemoteStorageError::None, true};
        }
        const auto& [label, identity] = labels.at(labelIndex++);
        return {label, identity, rfm::ssh::RemoteStorageError::None, false};
    }

    void endFileSystemLabels() override
    {
        labelsOpen = false;
        ++labelCloses;
    }

    rfm::ssh::RemoteStorageTopologyResult readTopologyNode(const QString& path) override
    {
        ++topologyReads;
        if (connectionLossAtTopologyRead == topologyReads) {
            alive = false;
            return {{}, rfm::ssh::RemoteStorageError::ConnectionLost, false};
        }
        rfm::core::StorageTopologyNode node;
        if (path.endsWith(QStringLiteral("/usb1")) || path.endsWith(QStringLiteral("/1-1"))) {
            node.subsystem = QStringLiteral("usb");
        } else {
            node.subsystem = QStringLiteral("block");
        }
        node.deviceModel = QStringLiteral("Fixture disk");
        return {node, rfm::ssh::RemoteStorageError::None,
                unreliableAtTopologyRead != topologyReads};
    }

    rfm::ssh::RemoteStorageStringResult deviceIdentity(const QString& device) override
    {
        ++identityReads;
        return {device, rfm::ssh::RemoteStorageError::None};
    }

    rfm::ssh::RemoteStorageSizeResult storageSize(const QString&) override
    {
        ++sizeReads;
        return {4096, rfm::ssh::RemoteStorageError::None};
    }

    bool connectionAlive() const override { return alive; }

    QByteArray mountInfo;
    rfm::ssh::RemoteStorageError mountInfoError{rfm::ssh::RemoteStorageError::None};
    qsizetype mountInfoFragmentSize{4096};
    qsizetype mountInfoOffset{0};
    QList<QPair<QString, QString>> labels;
    qsizetype labelIndex{0};
    int connectionLossAtTopologyRead{-1};
    int unreliableAtTopologyRead{-1};
    int fileReads{0};
    int mountInfoOpens{0};
    int mountInfoReads{0};
    int mountInfoCloses{0};
    int linkReads{0};
    int labelReads{0};
    int labelCloses{0};
    int topologyReads{0};
    int identityReads{0};
    int sizeReads{0};
    bool labelsAvailable{false};
    bool mountInfoOpen{false};
    bool labelsOpen{false};
    bool alive{true};
};

rfm::ssh::RemoteStorageScanStep finishScan(rfm::ssh::RemoteStorageScanner& scanner,
                                           int maximumSteps = 1000)
{
    rfm::ssh::RemoteStorageScanStep result;
    for (int step = 0; step < maximumSteps; ++step) {
        result = scanner.step();
        if (result.status != rfm::ssh::RemoteStorageScanStatus::Pending) {
            return result;
        }
    }
    return {rfm::ssh::RemoteStorageScanStatus::Failed,
            QStringLiteral("The scanner did not terminate within the test bound.")};
}

QByteArray blockMount(quint64 id, const QByteArray& deviceNumber, const QByteArray& mountPoint,
                      const QByteArray& device)
{
    return QByteArray::number(id) + " 1 " + deviceNumber + " / " + mountPoint +
           " rw,relatime - ext4 " + device + " rw\n";
}

} // namespace

class RemoteStorageScannerTest final : public QObject
{
    Q_OBJECT

  private slots:
    void yieldsAndCompletesAClassifiedScan();
    void streamsShortMountInfoReadsAndCancelsBetweenSteps();
    void cancellationDropsAnyPartialSnapshot();
    void connectionLossDropsAnyPartialSnapshot();
    void disappearingTopologyStaysUnknown();
    void rejectsTruncatedAndExcessiveMountInformation();
    void capsLabelsAndTopologyWorkConservatively();
    void exposesMountInfoFingerprintAfterSuccessfulScan();
    void keepsFuseVolumesNavigableWhenNoBlockDeviceExists();
    void filtersPseudoMountsBeforeApplyingTheMountLimit();
    void sharesLsblkParsingAndAddsUnmountedDevices();
    void keepsMountedDiscoveryWhenLsblkIsUnavailable();
};

void RemoteStorageScannerTest::yieldsAndCompletesAClassifiedScan()
{
    auto reader = std::make_unique<FakeRemoteStorageReader>();
    auto* const observed = reader.get();
    reader->mountInfo = blockMount(24, "8:1", "/media/usb", "/dev/sdb1");
    reader->labelsAvailable = true;
    reader->labels = {{QStringLiteral("BACKUP"), QStringLiteral("/dev/sdb1")}};
    rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 42);

    QCOMPARE(scanner.requestId(), quint64{42});
    QCOMPARE(scanner.step().status, rfm::ssh::RemoteStorageScanStatus::Pending);
    QCOMPARE(observed->mountInfoOpens, 1);
    QCOMPARE(observed->mountInfoReads, 0);
    QCOMPARE(observed->linkReads, 0);
    QCOMPARE(scanner.step().status, rfm::ssh::RemoteStorageScanStatus::Pending);
    QCOMPARE(observed->mountInfoReads, 1);
    QCOMPARE(observed->linkReads, 0);

    QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Completed);
    const QList<rfm::core::StorageVolume> volumes = scanner.takeVolumes();
    QCOMPARE(volumes.size(), 1);
    QCOMPARE(volumes.constFirst().rootPath, QStringLiteral("/media/usb"));
    QCOMPARE(volumes.constFirst().displayName, QStringLiteral("BACKUP"));
    QCOMPARE(volumes.constFirst().kind, rfm::core::StorageKind::External);
    QCOMPARE(volumes.constFirst().bytesTotal, quint64{4096});
}

void RemoteStorageScannerTest::streamsShortMountInfoReadsAndCancelsBetweenSteps()
{
    {
        auto reader = std::make_unique<FakeRemoteStorageReader>();
        auto* const observed = reader.get();
        reader->mountInfo = blockMount(24, "8:1", "/media/usb", "/dev/sdb1");
        reader->mountInfoFragmentSize = 3;
        rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 43);

        QCOMPARE(scanner.step().status, rfm::ssh::RemoteStorageScanStatus::Pending);
        for (int expectedReads = 1; expectedReads <= 3; ++expectedReads) {
            QCOMPARE(scanner.step().status, rfm::ssh::RemoteStorageScanStatus::Pending);
            QCOMPARE(observed->mountInfoReads, expectedReads);
        }
        scanner.cancel();
        QCOMPARE(observed->mountInfoCloses, 1);
        QCOMPARE(scanner.step().status, rfm::ssh::RemoteStorageScanStatus::Cancelled);
        QCOMPARE(observed->mountInfoReads, 3);
        QVERIFY(scanner.takeVolumes().isEmpty());
    }

    {
        auto reader = std::make_unique<FakeRemoteStorageReader>();
        auto* const observed = reader.get();
        reader->mountInfo = blockMount(24, "8:1", "/media/usb", "/dev/sdb1");
        reader->mountInfoFragmentSize = 2;
        rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 44);

        QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Completed);
        QVERIFY(observed->mountInfoReads > 10);
        QCOMPARE(observed->mountInfoCloses, 1);
        QCOMPARE(scanner.takeVolumes().size(), 1);
    }
}

void RemoteStorageScannerTest::cancellationDropsAnyPartialSnapshot()
{
    auto reader = std::make_unique<FakeRemoteStorageReader>();
    reader->mountInfo = blockMount(24, "0:42", "/network", "server:/share");
    rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 7);
    QCOMPARE(scanner.step().status, rfm::ssh::RemoteStorageScanStatus::Pending);
    scanner.cancel();
    QCOMPARE(scanner.step().status, rfm::ssh::RemoteStorageScanStatus::Cancelled);
    QVERIFY(scanner.takeVolumes().isEmpty());
}

void RemoteStorageScannerTest::connectionLossDropsAnyPartialSnapshot()
{
    auto reader = std::make_unique<FakeRemoteStorageReader>();
    reader->mountInfo = QByteArrayLiteral("23 1 0:42 / /network rw - nfs server:/share rw\n") +
                        blockMount(24, "8:1", "/media/usb", "/dev/sdb1");
    reader->connectionLossAtTopologyRead = 1;
    rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 8);

    const rfm::ssh::RemoteStorageScanStep result = finishScan(scanner);
    QCOMPARE(result.status, rfm::ssh::RemoteStorageScanStatus::ConnectionLost);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(scanner.takeVolumes().isEmpty());
}

void RemoteStorageScannerTest::disappearingTopologyStaysUnknown()
{
    {
        auto reader = std::make_unique<FakeRemoteStorageReader>();
        reader->mountInfo = blockMount(24, "8:1", "/media/removed", "/dev/sdb1");
        reader->unreliableAtTopologyRead = 1;
        rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 45);

        QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Completed);
        const QList<rfm::core::StorageVolume> volumes = scanner.takeVolumes();
        QCOMPARE(volumes.size(), 1);
        QCOMPARE(volumes.constFirst().kind, rfm::core::StorageKind::Unknown);
    }

    {
        auto reader = std::make_unique<FakeRemoteStorageReader>();
        reader->mountInfo = blockMount(24, "8:1", "/media/usb", "/dev/sdb1");
        reader->unreliableAtTopologyRead = 4;
        rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 46);

        QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Completed);
        const QList<rfm::core::StorageVolume> volumes = scanner.takeVolumes();
        QCOMPARE(volumes.size(), 1);
        QCOMPARE(volumes.constFirst().kind, rfm::core::StorageKind::External);
    }
}

void RemoteStorageScannerTest::rejectsTruncatedAndExcessiveMountInformation()
{
    {
        auto reader = std::make_unique<FakeRemoteStorageReader>();
        auto* const observed = reader.get();
        reader->mountInfo = QByteArray(1024 * 1024 + 1, 'x');
        rfm::ssh::RemoteStorageScanner::Limits limits;
        rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 9, limits);
        QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Failed);
        QCOMPARE(observed->mountInfoReads, 257);
        QCOMPARE(observed->mountInfoCloses, 1);
        QVERIFY(scanner.takeVolumes().isEmpty());
    }
    {
        auto reader = std::make_unique<FakeRemoteStorageReader>();
        reader->mountInfo = blockMount(24, "8:1", "/one", "/dev/sda1") +
                            blockMount(25, "8:2", "/two", "/dev/sda2") +
                            blockMount(26, "8:3", "/three", "/dev/sda3");
        rfm::ssh::RemoteStorageScanner::Limits limits;
        limits.maximumMounts = 2;
        rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 10, limits);
        QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Failed);
        QVERIFY(scanner.takeVolumes().isEmpty());
    }
}

void RemoteStorageScannerTest::capsLabelsAndTopologyWorkConservatively()
{
    auto reader = std::make_unique<FakeRemoteStorageReader>();
    auto* const observed = reader.get();
    reader->mountInfo = blockMount(24, "8:1", "/media/usb", "/dev/sdb1") +
                        blockMount(25, "8:17", "/media/second", "/dev/sdc1");
    reader->labelsAvailable = true;
    reader->labels = {{QStringLiteral("ONE"), QStringLiteral("/dev/other1")},
                      {QStringLiteral("TWO"), QStringLiteral("/dev/other2")},
                      {QStringLiteral("THREE"), QStringLiteral("/dev/sdb1")}};
    rfm::ssh::RemoteStorageScanner::Limits limits;
    limits.maximumLabels = 2;
    limits.maximumTopologyNodes = 1;
    rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 11, limits);

    QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Completed);
    QCOMPARE(observed->labelReads, 2);
    QCOMPARE(observed->topologyReads, 1);
    QVERIFY(!observed->labelsOpen);
    const QList<rfm::core::StorageVolume> volumes = scanner.takeVolumes();
    QCOMPARE(volumes.size(), 2);
    QCOMPARE(volumes.at(0).kind, rfm::core::StorageKind::Unknown);
    QCOMPARE(volumes.at(1).kind, rfm::core::StorageKind::Unknown);
}

void RemoteStorageScannerTest::exposesMountInfoFingerprintAfterSuccessfulScan()
{
    auto reader = std::make_unique<FakeRemoteStorageReader>();
    reader->mountInfo = blockMount(24, "8:1", "/data", "/dev/sda1");
    rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 12);

    QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Completed);
    const QByteArray fingerprint = scanner.mountInfoFingerprint();
    QVERIFY(!fingerprint.isEmpty());
    QCOMPARE(fingerprint.size(), 32);
}

void RemoteStorageScannerTest::keepsFuseVolumesNavigableWhenNoBlockDeviceExists()
{
    auto reader = std::make_unique<FakeRemoteStorageReader>();
    reader->mountInfo = "24 1 0:44 / /media/ntfs rw - fuseblk ntfs-3g rw\n"
                        "25 1 0:45 / /home/alice/cloud rw - fuse.rclone rclone rw\n"
                        "26 1 0:46 / /mnt/share rw - nfs server:/share rw\n";
    rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 13);

    QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Completed);
    const QList<rfm::core::StorageVolume> volumes = scanner.takeVolumes();
    QCOMPARE(volumes.size(), 3);
    QCOMPARE(volumes.at(0).fileSystemType, QByteArrayLiteral("fuse.rclone"));
    QCOMPARE(volumes.at(0).kind, rfm::core::StorageKind::Unknown);
    QCOMPARE(volumes.at(1).fileSystemType, QByteArrayLiteral("fuseblk"));
    QCOMPARE(volumes.at(1).kind, rfm::core::StorageKind::Unknown);
    QCOMPARE(volumes.at(2).kind, rfm::core::StorageKind::Network);
}

void RemoteStorageScannerTest::filtersPseudoMountsBeforeApplyingTheMountLimit()
{
    auto reader = std::make_unique<FakeRemoteStorageReader>();
    for (int index = 1; index <= 160; ++index) {
        reader->mountInfo += QByteArray::number(index) + " 1 0:" + QByteArray::number(index) +
                             " / /run/container/" + QByteArray::number(index) +
                             " rw - tmpfs tmpfs rw\n";
    }
    reader->mountInfo += blockMount(200, "8:1", "/data", "/dev/sda1");
    rfm::ssh::RemoteStorageScanner::Limits limits;
    limits.maximumMounts = 1;
    rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 14, limits);

    QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Completed);
    const QList<rfm::core::StorageVolume> volumes = scanner.takeVolumes();
    QCOMPARE(volumes.size(), 1);
    QCOMPARE(volumes.constFirst().rootPath, QStringLiteral("/data"));
}

void RemoteStorageScannerTest::sharesLsblkParsingAndAddsUnmountedDevices()
{
    const QByteArray json = R"json({"blockdevices":[
        {"path":"/dev/sda","name":"sda","type":"disk","fstype":null,"size":10000,
         "children":[
           {"path":"/dev/sda1","name":"sda1","pkname":"/dev/sda","type":"part",
            "fstype":"ext4","label":"ROOT","size":7000,"mountpoints":["/"],"ro":false},
           {"path":"/dev/sda2","name":"sda2","pkname":"/dev/sda","type":"part",
            "fstype":"swap","size":3000,"mountpoints":[null]}]},
        {"path":"/dev/sdb","name":"sdb","type":"disk","fstype":"ext4","label":"USB",
         "size":4096,"mountpoints":[null],"tran":"usb","rm":true},
        {"path":"/dev/loop0","name":"loop0","type":"loop","fstype":"squashfs",
         "size":1024,"mountpoints":[null]}
    ]})json";
    const QList<rfm::core::LinuxBlockDevice> devices = rfm::core::parseLinuxBlockDevices(json);
    QCOMPARE(devices.size(), 5);
    QCOMPARE(devices.at(1).parentDevice, QStringLiteral("/dev/sda"));
    QCOMPARE(devices.at(1).mountPoint, QStringLiteral("/"));

    rfm::core::StorageVolume root;
    root.displayName = QStringLiteral("System");
    root.rootPath = QStringLiteral("/");
    root.device = QStringLiteral("/dev/sda1");
    root.fileSystemType = QByteArrayLiteral("ext4");
    root.kind = rfm::core::StorageKind::System;
    const QList<rfm::core::StorageVolume> volumes =
        rfm::core::mergeLinuxBlockDevices({root}, devices);
    QCOMPARE(volumes.size(), 2);
    QVERIFY(std::ranges::any_of(volumes, [](const rfm::core::StorageVolume& volume) {
        return volume.device == QStringLiteral("/dev/sda1") && volume.mounted &&
               volume.rootPath == QStringLiteral("/") &&
               volume.kind == rfm::core::StorageKind::System;
    }));
    QVERIFY(std::ranges::any_of(volumes, [](const rfm::core::StorageVolume& volume) {
        return volume.device == QStringLiteral("/dev/sdb") && !volume.mounted &&
               volume.kind == rfm::core::StorageKind::External;
    }));
}

void RemoteStorageScannerTest::keepsMountedDiscoveryWhenLsblkIsUnavailable()
{
    auto reader = std::make_unique<FakeRemoteStorageReader>();
    reader->mountInfo =
        blockMount(24, "8:1", "/", "/dev/sda1") + blockMount(25, "8:17", "/media/usb", "/dev/sdb1");
    rfm::ssh::RemoteStorageScanner scanner(std::move(reader), 15);
    QCOMPARE(finishScan(scanner).status, rfm::ssh::RemoteStorageScanStatus::Completed);
    const QList<rfm::core::StorageVolume> volumes = scanner.takeVolumes();
    QCOMPARE(volumes.size(), 2);
    QVERIFY(std::ranges::any_of(volumes, [](const rfm::core::StorageVolume& volume) {
        return volume.rootPath == QStringLiteral("/") && volume.mounted;
    }));
}

QTEST_MAIN(RemoteStorageScannerTest)
#include "test_remote_storage_scanner.moc"
