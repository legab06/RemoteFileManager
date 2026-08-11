#include "remotefilemanager/core/LocalFileSystem.hpp"

#include "../src/core/LocalStorageTopology.hpp"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

namespace
{

class FakeSysfsTopologyReader final : public rfm::core::detail::SysfsTopologyReader
{
  public:
    rfm::core::detail::SysfsReadStatus nodeStatus(const QString& path) override
    {
        return nodeStatuses.value(path, rfm::core::detail::SysfsReadStatus::Present);
    }

    rfm::core::detail::SysfsTextResult readText(const QString& path, qsizetype) override
    {
        return textResults.value(path, {{}, rfm::core::detail::SysfsReadStatus::NotPresent});
    }

    rfm::core::detail::SysfsLinkResult readLink(const QString& path) override
    {
        return linkResults.value(path, {{}, rfm::core::detail::SysfsReadStatus::NotPresent});
    }

    QHash<QString, rfm::core::detail::SysfsReadStatus> nodeStatuses;
    QHash<QString, rfm::core::detail::SysfsTextResult> textResults;
    QHash<QString, rfm::core::detail::SysfsLinkResult> linkResults;
};

rfm::core::StorageKind classifiedDevice(const rfm::core::StorageDeviceEvidence& device)
{
    return rfm::core::classifyStorage({false, false, device.blockDevice, device.virtualBlockDevice,
                                       device.externalTransport, device.topologyComplete});
}

} // namespace

class LocalFileSystemTest final : public QObject
{
    Q_OBJECT

  private slots:
    void listsOnlyImmediateEntriesFromTemporaryDirectory();
    void rejectsMissingDirectory();
    void classifiesInternalStorage();
    void classifiesDirectUsbStorage();
    void classifiesExternalStorageIndependentlyFromRemovableFlag();
    void classifiesUsbBehindBlockAndScsiParents();
    void continuesAcrossMissingSysfsSubsystems();
    void classifiesUsbPartitionFromCompleteAncestry();
    void classifiesUnknownStorageConservatively();
    void keepsIncompleteBlockTopologyUnknown();
    void keepsSysfsErrorsConservativeAndOptionalAbsenceReliable();
    void selectsHumanStorageNameByPriority();
    void keepsRemoteUsbClassificationAfterMetadataEnrichment();
    void parsesLinuxMountInformation();
    void preservesNavigableFuseMountsAndFiltersPseudoFileSystems();
    void filtersBindMountsAndKeepsVisibleOvermount();
    void filtersVisiblePseudoOvermountsWithoutRestoringHiddenStorage();
    void selectsOvermountsFromParentRelationships();
    void preservesDistinctBtrfsAndAmbiguousAttachments();
    void reportsPortableMountedVolumes();
    void buildsVolumesAndFingerprintFromOneSnapshot();
};

void LocalFileSystemTest::listsOnlyImmediateEntriesFromTemporaryDirectory()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("folder")));
    QVERIFY(QDir(root.filePath(QStringLiteral("folder"))).mkdir(QStringLiteral("nested")));
    QFile file(root.filePath(QStringLiteral("sample.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("content"), qint64{7});
    file.close();

    const rfm::core::LocalDirectoryResult result =
        rfm::core::LocalFileSystem::listDirectory(temporary.path());
    QVERIFY2(result.succeeded(), qPrintable(result.error));
    QCOMPARE(result.entries.size(), 2);
    QCOMPARE(result.entries.at(0).name, QStringLiteral("folder"));
    QVERIFY(result.entries.at(0).directory);
    QCOMPARE(result.entries.at(1).name, QStringLiteral("sample.txt"));
    QVERIFY(!result.entries.at(1).directory);
    QCOMPARE(result.entries.at(1).size, quint64{7});
    QVERIFY(std::ranges::none_of(result.entries, [](const rfm::core::RemoteEntry& entry) {
        return entry.name == QStringLiteral("nested");
    }));
}

void LocalFileSystemTest::rejectsMissingDirectory()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString missing = QDir(temporary.path()).filePath(QStringLiteral("missing"));
    const rfm::core::LocalDirectoryResult result =
        rfm::core::LocalFileSystem::listDirectory(missing);
    QVERIFY(!result.succeeded());
    QVERIFY(!result.error.isEmpty());
}

void LocalFileSystemTest::classifiesInternalStorage()
{
    const rfm::core::StorageDeviceEvidence device = rfm::core::storageDeviceEvidence(
        {{QStringLiteral("block"), false}, {QStringLiteral("scsi"), false}}, true, false, true);
    rfm::core::StorageClassificationEvidence evidence{false,
                                                      false,
                                                      device.blockDevice,
                                                      device.virtualBlockDevice,
                                                      device.externalTransport,
                                                      device.topologyComplete};
    QCOMPARE(rfm::core::classifyStorage(evidence), rfm::core::StorageKind::Internal);

    evidence.systemVolume = true;
    QCOMPARE(rfm::core::classifyStorage(evidence), rfm::core::StorageKind::System);
}

void LocalFileSystemTest::classifiesDirectUsbStorage()
{
    const rfm::core::StorageDeviceEvidence device = rfm::core::storageDeviceEvidence(
        {{QStringLiteral("block"), false}, {QStringLiteral("usb"), false}}, true, false, true);
    QVERIFY(device.externalTransport);
    QCOMPARE(
        rfm::core::classifyStorage({false, false, device.blockDevice, device.virtualBlockDevice,
                                    device.externalTransport, device.topologyComplete}),
        rfm::core::StorageKind::External);
}

void LocalFileSystemTest::classifiesExternalStorageIndependentlyFromRemovableFlag()
{
    rfm::core::StorageClassificationEvidence evidence;
    evidence.blockDevice = true;
    evidence.externalTransport = true;
    rfm::core::StorageVolume usbSsd;
    usbSsd.kind = rfm::core::classifyStorage(evidence);
    usbSsd.removable = false;
    QCOMPARE(usbSsd.kind, rfm::core::StorageKind::External);
    QVERIFY(!usbSsd.removable);

    rfm::core::StorageVolume removableButUnclassified;
    removableButUnclassified.kind = rfm::core::classifyStorage({});
    removableButUnclassified.removable = true;
    QCOMPARE(removableButUnclassified.kind, rfm::core::StorageKind::Unknown);
    QVERIFY(removableButUnclassified.removable);

    rfm::core::StorageVolume ejectableInternal;
    ejectableInternal.kind = rfm::core::StorageKind::Internal;
    ejectableInternal.removable = true;
    ejectableInternal.ejectable = true;
    QCOMPARE(ejectableInternal.kind, rfm::core::StorageKind::Internal);
    QVERIFY(ejectableInternal.removable);
    QVERIFY(ejectableInternal.ejectable);
}

void LocalFileSystemTest::classifiesUsbBehindBlockAndScsiParents()
{
    const QStringList subsystemChain{QStringLiteral("block"), QStringLiteral("block"),
                                     QStringLiteral("scsi"),  QStringLiteral("scsi"),
                                     QStringLiteral("scsi"),  QStringLiteral("usb")};
    QVERIFY(rfm::core::hasExternalStorageTransport(subsystemChain));

    rfm::core::StorageClassificationEvidence evidence;
    evidence.blockDevice = true;
    evidence.topologyComplete = true;
    evidence.externalTransport = rfm::core::hasExternalStorageTransport(subsystemChain);
    QCOMPARE(rfm::core::classifyStorage(evidence), rfm::core::StorageKind::External);
}

void LocalFileSystemTest::continuesAcrossMissingSysfsSubsystems()
{
    const QList<rfm::core::StorageTopologyNode> ancestry{{QStringLiteral("block"), false},
                                                         {QStringLiteral("block"), false},
                                                         {{}, false},
                                                         {QStringLiteral("scsi"), false},
                                                         {QStringLiteral("scsi"), false},
                                                         {QStringLiteral("scsi"), false},
                                                         {QStringLiteral("usb"), false},
                                                         {QStringLiteral("usb"), false},
                                                         {QStringLiteral("usb"), false}};
    const rfm::core::StorageDeviceEvidence device =
        rfm::core::storageDeviceEvidence(ancestry, true, false, true);
    QVERIFY(device.externalTransport);
    QCOMPARE(
        rfm::core::classifyStorage({false, false, device.blockDevice, device.virtualBlockDevice,
                                    device.externalTransport, device.topologyComplete}),
        rfm::core::StorageKind::External);

    const rfm::core::StorageDeviceEvidence multipleMissing = rfm::core::storageDeviceEvidence(
        {{{}, false}, {{}, false}, {QStringLiteral("usb"), false}}, true, false, true);
    QVERIFY(multipleMissing.externalTransport);
}

void LocalFileSystemTest::classifiesUsbPartitionFromCompleteAncestry()
{
    const rfm::core::StorageDeviceEvidence partition =
        rfm::core::storageDeviceEvidence({{QStringLiteral("block"), false},
                                          {QStringLiteral("block"), false},
                                          {{}, false},
                                          {QStringLiteral("scsi"), false},
                                          {QStringLiteral("usb"), false}},
                                         true, false, true);
    QVERIFY(partition.externalTransport);
    QVERIFY(!partition.removable);
    QCOMPARE(rfm::core::classifyStorage({false, false, partition.blockDevice,
                                         partition.virtualBlockDevice, partition.externalTransport,
                                         partition.topologyComplete}),
             rfm::core::StorageKind::External);
}

void LocalFileSystemTest::classifiesUnknownStorageConservatively()
{
    QCOMPARE(rfm::core::classifyStorage({}), rfm::core::StorageKind::Unknown);

    rfm::core::StorageClassificationEvidence networkStorage;
    networkStorage.networkFileSystem = true;
    QCOMPARE(rfm::core::classifyStorage(networkStorage), rfm::core::StorageKind::Network);

    rfm::core::StorageClassificationEvidence virtualDevice;
    virtualDevice.blockDevice = true;
    virtualDevice.virtualBlockDevice = true;
    virtualDevice.externalTransport = true;
    QCOMPARE(rfm::core::classifyStorage(virtualDevice), rfm::core::StorageKind::Unknown);
}

void LocalFileSystemTest::keepsIncompleteBlockTopologyUnknown()
{
    const rfm::core::StorageDeviceEvidence absentTopology =
        rfm::core::storageDeviceEvidence({}, true, false, false);
    QCOMPARE(rfm::core::classifyStorage(
                 {false, false, absentTopology.blockDevice, absentTopology.virtualBlockDevice,
                  absentTopology.externalTransport, absentTopology.topologyComplete}),
             rfm::core::StorageKind::Unknown);

    const rfm::core::StorageDeviceEvidence interruptedTopology = rfm::core::storageDeviceEvidence(
        {{QStringLiteral("block"), false}, {QStringLiteral("scsi"), false}}, true, false, false);
    QCOMPARE(rfm::core::classifyStorage({false, false, interruptedTopology.blockDevice,
                                         interruptedTopology.virtualBlockDevice,
                                         interruptedTopology.externalTransport,
                                         interruptedTopology.topologyComplete}),
             rfm::core::StorageKind::Unknown);
}

void LocalFileSystemTest::keepsSysfsErrorsConservativeAndOptionalAbsenceReliable()
{
    using rfm::core::detail::SysfsLinkResult;
    using rfm::core::detail::SysfsReadStatus;
    using rfm::core::detail::SysfsTextResult;
    const QString devicePath = QStringLiteral("/sys/devices/pci/block/sdb");
    const QString subsystemPath = devicePath + QStringLiteral("/subsystem");
    const QString removablePath = devicePath + QStringLiteral("/removable");

    {
        FakeSysfsTopologyReader reader;
        reader.linkResults.insert(subsystemPath,
                                  SysfsLinkResult{{}, SysfsReadStatus::PermissionDenied});
        const rfm::core::StorageDeviceEvidence evidence =
            rfm::core::detail::collectLinuxStorageDetails(devicePath, reader);
        QVERIFY(!evidence.topologyComplete);
        QCOMPARE(classifiedDevice(evidence), rfm::core::StorageKind::Unknown);
    }

    {
        FakeSysfsTopologyReader reader;
        reader.textResults.insert(removablePath, SysfsTextResult{{}, SysfsReadStatus::IoError});
        const rfm::core::StorageDeviceEvidence evidence =
            rfm::core::detail::collectLinuxStorageDetails(devicePath, reader);
        QVERIFY(!evidence.topologyComplete);
        QCOMPARE(classifiedDevice(evidence), rfm::core::StorageKind::Unknown);
    }

    {
        FakeSysfsTopologyReader reader;
        const rfm::core::StorageDeviceEvidence evidence =
            rfm::core::detail::collectLinuxStorageDetails(devicePath, reader);
        QVERIFY(evidence.topologyComplete);
        QCOMPARE(classifiedDevice(evidence), rfm::core::StorageKind::Internal);
    }

    {
        FakeSysfsTopologyReader reader;
        reader.linkResults.insert(subsystemPath,
                                  SysfsLinkResult{{}, SysfsReadStatus::PermissionDenied});
        reader.textResults.insert(
            removablePath, SysfsTextResult{QByteArrayLiteral("0\n"), SysfsReadStatus::Present});
        reader.linkResults.insert(
            QStringLiteral("/sys/devices/pci/subsystem"),
            SysfsLinkResult{QStringLiteral("/sys/bus/usb"), SysfsReadStatus::Present});
        const rfm::core::StorageDeviceEvidence evidence =
            rfm::core::detail::collectLinuxStorageDetails(devicePath, reader);
        QVERIFY(!evidence.topologyComplete);
        QVERIFY(evidence.externalTransport);
        QVERIFY(!evidence.removable);
        QCOMPARE(classifiedDevice(evidence), rfm::core::StorageKind::External);
    }
}

void LocalFileSystemTest::selectsHumanStorageNameByPriority()
{
    QCOMPARE(rfm::core::storageDisplayName(QStringLiteral("PHOTOS"), QStringLiteral("Cruzer Glide"),
                                           QStringLiteral("/dev/sdb1"),
                                           QStringLiteral("/media/usb")),
             QStringLiteral("PHOTOS"));
    QCOMPARE(rfm::core::storageDisplayName({}, QStringLiteral("Cruzer Glide"),
                                           QStringLiteral("/dev/sdb1"),
                                           QStringLiteral("/media/usb")),
             QStringLiteral("Cruzer Glide"));
    QCOMPARE(rfm::core::storageDisplayName({}, {}, QStringLiteral("/dev/sdb1"),
                                           QStringLiteral("/media/usb")),
             QStringLiteral("sdb1"));
    QCOMPARE(rfm::core::storageDisplayName({}, {}, {}, QStringLiteral("/media/usb")),
             QStringLiteral("/media/usb"));
}

void LocalFileSystemTest::keepsRemoteUsbClassificationAfterMetadataEnrichment()
{
    const rfm::core::StorageDeviceEvidence device = rfm::core::storageDeviceEvidence(
        {{QStringLiteral("block"), false, {}},
         {{}, false, {}},
         {QStringLiteral("scsi"), false, QStringLiteral("Cruzer Glide")},
         {QStringLiteral("usb"), false, {}}},
        true, false, true);
    const rfm::core::LinuxMountInfo mount{QStringLiteral("/media/usb"),
                                          QStringLiteral("/dev/sdb1"),
                                          QByteArrayLiteral("exfat"),
                                          QStringLiteral("8:17"),
                                          false,
                                          24,
                                          1,
                                          QStringLiteral("/"),
                                          {}};
    const rfm::core::StorageVolume volume =
        rfm::core::makeStorageVolume(mount, device, QStringLiteral("PHOTOS"), 1234);
    QCOMPARE(volume.kind, rfm::core::StorageKind::External);
    QCOMPARE(volume.displayName, QStringLiteral("PHOTOS"));
    QCOMPARE(volume.fileSystemLabel, QStringLiteral("PHOTOS"));
    QCOMPARE(volume.deviceModel, QStringLiteral("Cruzer Glide"));
    QCOMPARE(volume.rootPath, QStringLiteral("/media/usb"));
}

void LocalFileSystemTest::parsesLinuxMountInformation()
{
    const QByteArray fixture =
        "24 1 8:1 / / rw,relatime - ext4 /dev/sda1 rw\n"
        "25 24 8:17 / /media/My\\040USB rw,nosuid shared:7 master:1 - vfat /dev/sdb1 rw\n"
        "26 24 0:44 / /proc rw,nosuid - proc proc rw\n"
        "27 24 0:45 / /mnt/share\\011tab ro,relatime - nfs server:/share\\134name ro\n"
        "28 24 8:33 / /partial rw - ext4 /dev/sdc1\n"
        "invalid partial line\n";
    const QList<rfm::core::LinuxMountInfo> mounts = rfm::core::parseLinuxMountInfo(fixture);
    QCOMPARE(mounts.size(), 3);
    QCOMPARE(mounts.at(0).rootPath, QStringLiteral("/"));
    QCOMPARE(mounts.at(1).rootPath, QStringLiteral("/media/My USB"));
    QCOMPARE(mounts.at(1).deviceNumber, QStringLiteral("8:17"));
    QCOMPARE(mounts.at(1).mountRoot, QStringLiteral("/"));
    QCOMPARE(mounts.at(1).optionalFields,
             QList<QByteArray>({QByteArrayLiteral("shared:7"), QByteArrayLiteral("master:1")}));
    QCOMPARE(mounts.at(2).rootPath, QStringLiteral("/mnt/share\ttab"));
    QCOMPARE(mounts.at(2).device, QStringLiteral("server:/share\\name"));
    QCOMPARE(mounts.at(2).fileSystemType, QByteArrayLiteral("nfs"));
    QVERIFY(mounts.at(2).readOnly);
}

void LocalFileSystemTest::preservesNavigableFuseMountsAndFiltersPseudoFileSystems()
{
    const QByteArray fixture = "24 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
                               "25 24 0:44 / /media/ntfs rw - fuseblk ntfs-3g rw\n"
                               "26 24 0:45 / /home/alice/cloud rw - fuse.rclone rclone rw\n"
                               "27 24 0:46 / /mnt/nfs rw - nfs server:/export rw\n"
                               "28 24 0:47 / /mnt/cifs rw - cifs //server/share rw\n"
                               "29 24 0:48 / /proc rw - proc proc rw\n"
                               "30 24 0:49 / /sys rw - sysfs sysfs rw\n"
                               "31 24 0:50 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n"
                               "32 24 0:51 / /run/user/1000/doc rw - fuse.portal portal rw\n"
                               "33 24 0:52 / /run rw - tmpfs tmpfs rw\n"
                               "34 24 0:53 / /dev/shm rw - tmpfs shm rw\n"
                               "35 24 8:2 / /srv rw - xfs /dev/sdb1 rw\n"
                               "36 24 8:3 /@ /work rw - btrfs /dev/sdc1 rw\n";

    const QList<rfm::core::LinuxMountInfo> mounts = rfm::core::parseLinuxMountInfo(fixture);
    QCOMPARE(mounts.size(), 7);
    const auto hasType = [&mounts](const QByteArray& fileSystemType) {
        return std::ranges::any_of(mounts,
                                   [&fileSystemType](const rfm::core::LinuxMountInfo& mount) {
                                       return mount.fileSystemType == fileSystemType;
                                   });
    };
    QVERIFY(hasType(QByteArrayLiteral("fuse.rclone")));
    QVERIFY(hasType(QByteArrayLiteral("fuseblk")));
    QVERIFY(hasType(QByteArrayLiteral("nfs")));
    QVERIFY(hasType(QByteArrayLiteral("cifs")));
    QVERIFY(hasType(QByteArrayLiteral("ext4")));
    QVERIFY(hasType(QByteArrayLiteral("xfs")));
    QVERIFY(hasType(QByteArrayLiteral("btrfs")));
    QVERIFY(std::ranges::none_of(mounts, [](const rfm::core::LinuxMountInfo& mount) {
        return mount.fileSystemType == QByteArrayLiteral("proc") ||
               mount.fileSystemType == QByteArrayLiteral("sysfs") ||
               mount.fileSystemType == QByteArrayLiteral("cgroup2") ||
               mount.fileSystemType == QByteArrayLiteral("tmpfs") ||
               mount.fileSystemType == QByteArrayLiteral("fuse.portal");
    }));
}

void LocalFileSystemTest::buildsVolumesAndFingerprintFromOneSnapshot()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    const QList<rfm::core::LocalStorageMount> firstSnapshot{
        {first.path(), QStringLiteral("/dev/first"), QByteArrayLiteral("ext4"),
         QStringLiteral("First"), 1024, false, true, true}};
    const QList<rfm::core::LocalStorageMount> secondSnapshot{
        firstSnapshot.constFirst(),
        {second.path(), QStringLiteral("/dev/second"), QByteArrayLiteral("vfat"),
         QStringLiteral("Second"), 2048, false, true, true}};

    const rfm::core::LocalStorageSnapshot firstResult =
        rfm::core::LocalFileSystem::makeStorageSnapshot(firstSnapshot);
    const rfm::core::LocalStorageSnapshot unchangedResult =
        rfm::core::LocalFileSystem::makeStorageSnapshot(firstSnapshot);
    const rfm::core::LocalStorageSnapshot secondResult =
        rfm::core::LocalFileSystem::makeStorageSnapshot(secondSnapshot);
    QCOMPARE(firstResult.volumes.size(), 1);
    QCOMPARE(firstResult.volumes.constFirst().rootPath, first.path());
    QCOMPARE(firstResult.fingerprint, unchangedResult.fingerprint);
    QCOMPARE(secondResult.volumes.size(), 2);
    QVERIFY(firstResult.fingerprint != secondResult.fingerprint);
}

void LocalFileSystemTest::filtersBindMountsAndKeepsVisibleOvermount()
{
    const QByteArray fixture = "30 1 8:2 / /data rw - ext4 /dev/sda2 rw\n"
                               "31 1 8:2 /projects /srv/data rw - ext4 /dev/sda2 rw\n"
                               "40 1 8:3 / /mnt/stack rw - ext4 /dev/sda3 rw\n"
                               "41 40 8:17 / /mnt/stack ro shared:9 - exfat /dev/sdb1 ro\n";
    const QList<rfm::core::LinuxMountInfo> mounts = rfm::core::parseLinuxMountInfo(fixture);
    QCOMPARE(mounts.size(), 2);
    QCOMPARE(mounts.at(0).rootPath, QStringLiteral("/data"));
    QCOMPARE(mounts.at(0).mountRoot, QStringLiteral("/"));
    QCOMPARE(mounts.at(1).rootPath, QStringLiteral("/mnt/stack"));
    QCOMPARE(mounts.at(1).device, QStringLiteral("/dev/sdb1"));
    QCOMPARE(mounts.at(1).mountId, quint64{41});
    QVERIFY(mounts.at(1).readOnly);
}

void LocalFileSystemTest::filtersVisiblePseudoOvermountsWithoutRestoringHiddenStorage()
{
    const QByteArray tmpfsOvermount = "40 1 8:17 / /mnt/data rw - ext4 /dev/sdb1 rw\n"
                                      "41 40 0:50 / /mnt/data rw - tmpfs tmpfs rw\n";
    const QList<rfm::core::LinuxMountInfo> tmpfsMounts =
        rfm::core::parseLinuxMountInfo(tmpfsOvermount);
    QVERIFY(tmpfsMounts.isEmpty());

    const QByteArray portalOvermount = "50 1 8:18 / /some/path rw - ext4 /dev/sdc1 rw\n"
                                       "51 50 0:51 / /some/path rw - fuse.portal portal rw\n";
    const QList<rfm::core::LinuxMountInfo> portalMounts =
        rfm::core::parseLinuxMountInfo(portalOvermount);
    QVERIFY(portalMounts.isEmpty());

    const QByteArray storageOvermount = "60 1 0:52 / /mnt/real rw - tmpfs tmpfs rw\n"
                                        "61 60 8:19 / /mnt/real rw - xfs /dev/sdd1 rw\n";
    const QList<rfm::core::LinuxMountInfo> storageMounts =
        rfm::core::parseLinuxMountInfo(storageOvermount);
    QCOMPARE(storageMounts.size(), 1);
    QCOMPARE(storageMounts.constFirst().fileSystemType, QByteArrayLiteral("xfs"));
    QCOMPARE(storageMounts.constFirst().device, QStringLiteral("/dev/sdd1"));
}

void LocalFileSystemTest::selectsOvermountsFromParentRelationships()
{
    const QByteArray fixture = "150 1 8:1 / /mnt/two rw - ext4 /dev/old-two rw\n"
                               "42 150 8:2 / /mnt/two ro - xfs /dev/new-two ro\n"
                               "300 1 8:3 / /mnt/three rw - ext4 /dev/old-three rw\n"
                               "90 300 8:4 / /mnt/three rw - xfs /dev/middle-three rw\n"
                               "7 90 8:5 / /mnt/three ro - exfat /dev/top-three ro\n"
                               "500 501 8:6 / /mnt/ambiguous rw - ext4 /dev/first-ambiguous rw\n"
                               "501 500 8:7 / /mnt/ambiguous rw - xfs /dev/second-ambiguous rw\n";
    const QList<rfm::core::LinuxMountInfo> mounts = rfm::core::parseLinuxMountInfo(fixture);

    QCOMPARE(mounts.size(), 3);
    QCOMPARE(mounts.at(0).rootPath, QStringLiteral("/mnt/ambiguous"));
    QCOMPARE(mounts.at(0).mountId, quint64{500});
    QCOMPARE(mounts.at(1).rootPath, QStringLiteral("/mnt/three"));
    QCOMPARE(mounts.at(1).mountId, quint64{7});
    QCOMPARE(mounts.at(2).rootPath, QStringLiteral("/mnt/two"));
    QCOMPARE(mounts.at(2).mountId, quint64{42});
}

void LocalFileSystemTest::preservesDistinctBtrfsAndAmbiguousAttachments()
{
    const QByteArray fixture = "60 1 259:2 /@ / rw - btrfs /dev/nvme0n1p2 rw\n"
                               "61 60 259:2 /@home /home rw - btrfs /dev/nvme0n1p2 rw\n"
                               "70 1 8:9 /alpha /mnt/alpha rw - ext4 /dev/sdd1 rw\n"
                               "71 1 8:9 /beta /mnt/beta rw - ext4 /dev/sdd1 rw\n"
                               "80 1 8:10 / /mnt/first rw - ext4 /dev/sde1 rw\n"
                               "81 1 8:10 / /mnt/second rw - ext4 /dev/sde1 rw\n";
    const QList<rfm::core::LinuxMountInfo> mounts = rfm::core::parseLinuxMountInfo(fixture);

    QCOMPARE(mounts.size(), 6);
    QCOMPARE(mounts.at(0).rootPath, QStringLiteral("/"));
    QCOMPARE(mounts.at(0).mountRoot, QStringLiteral("/@"));
    QCOMPARE(mounts.at(1).rootPath, QStringLiteral("/home"));
    QCOMPARE(mounts.at(1).mountRoot, QStringLiteral("/@home"));
    QCOMPARE(mounts.at(2).rootPath, QStringLiteral("/mnt/alpha"));
    QCOMPARE(mounts.at(3).rootPath, QStringLiteral("/mnt/beta"));
    QCOMPARE(mounts.at(4).rootPath, QStringLiteral("/mnt/first"));
    QCOMPARE(mounts.at(5).rootPath, QStringLiteral("/mnt/second"));
}

void LocalFileSystemTest::reportsPortableMountedVolumes()
{
    const QList<rfm::core::StorageVolume> volumes = rfm::core::LocalFileSystem::mountedVolumes();
    QSet<QString> roots;
    for (const rfm::core::StorageVolume& volume : volumes) {
        QVERIFY(!volume.displayName.isEmpty());
        QVERIFY(!volume.rootPath.isEmpty());
        QVERIFY(QFileInfo(volume.rootPath).isAbsolute());
        QVERIFY(!roots.contains(volume.rootPath));
        roots.insert(volume.rootPath);
    }
}

QTEST_GUILESS_MAIN(LocalFileSystemTest)
#include "test_local_file_system.moc"
