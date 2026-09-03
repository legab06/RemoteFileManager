#include "remotefilemanager/core/LocalFileSystem.hpp"

#include "../src/core/LocalStorageTopology.hpp"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSignalSpy>
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
    void countsOnlyImmediateDirectoryEntries();
    void rejectsMissingDirectory();
    void createsLocalFoldersWithValidation();
    void renamesLocalFilesAndFoldersWithValidation();
    void removesLocalSelectionsRecursivelyWithoutFollowingSymlinks();
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
    void filtersPseudoFileSystemsAtRoot();
    void filtersBindMountsAndKeepsVisibleOvermount();
    void filtersVisiblePseudoOvermountsWithoutRestoringHiddenStorage();
    void selectsOvermountsFromParentRelationships();
    void deduplicatesMultipleAttachmentsOfOneStorage();
    void reportsPortableMountedVolumes();
    void buildsVolumesAndFingerprintFromOneSnapshot();
    void filtersTechnicalMountsAndDeduplicatesMountedDevices();
    void discoversUnmountedUsbPartitionWithoutTechnicalDuplicates();
    void keepsMountedVolumeWhenBlockDiscoveryReportsItToo();
    void deduplicatesBlockAliasesByDeviceNumber();
    void matchesLocalPathsOnComponentBoundaries();
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
    QVERIFY(result.entries.at(0).modifiedAt.isValid());
    QCOMPARE(result.entries.at(1).name, QStringLiteral("sample.txt"));
    QVERIFY(!result.entries.at(1).directory);
    QCOMPARE(result.entries.at(1).size, quint64{7});
    QVERIFY(result.entries.at(1).modifiedAt.isValid());
    QVERIFY(std::ranges::none_of(result.entries, [](const rfm::core::RemoteEntry& entry) {
        return entry.name == QStringLiteral("nested");
    }));
}

void LocalFileSystemTest::countsOnlyImmediateDirectoryEntries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("folder")));
    QVERIFY(root.mkdir(QStringLiteral("other")));
    QFile file(root.filePath(QStringLiteral("file.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QVERIFY(QDir(root.filePath(QStringLiteral("folder"))).mkdir(QStringLiteral("nested")));

    rfm::core::LocalFileSystemWorker worker;
    QSignalSpy counted(&worker, &rfm::core::LocalFileSystemWorker::directoryCounted);
    QSignalSpy failed(&worker, &rfm::core::LocalFileSystemWorker::directoryCountFailed);
    worker.countDirectoryEntries(41, temporary.path());
    QCOMPARE(counted.size(), 1);
    QCOMPARE(counted.constFirst().at(0).toULongLong(), quint64{41});
    QCOMPARE(counted.constFirst().at(1).toString(), temporary.path());
    QCOMPARE(counted.constFirst().at(2).toULongLong(), quint64{3});
    QCOMPARE(failed.size(), 0);

    worker.countDirectoryEntries(42, root.filePath(QStringLiteral("missing")));
    QCOMPARE(failed.size(), 1);
    QCOMPARE(failed.constFirst().at(0).toULongLong(), quint64{42});
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

void LocalFileSystemTest::createsLocalFoldersWithValidation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const rfm::core::LocalFileOperationResult created =
        rfm::core::LocalFileSystem::executeOperation(
            {1,
             rfm::core::LocalFileOperationKind::CreateDirectory,
             temporary.path(),
             {},
             QStringLiteral("created")});
    QVERIFY2(created.allSucceeded(), qPrintable(created.items.constFirst().error));
    QVERIFY(QFileInfo(QDir(temporary.path()).filePath(QStringLiteral("created"))).isDir());

    const rfm::core::LocalFileOperationResult duplicate =
        rfm::core::LocalFileSystem::executeOperation(
            {2,
             rfm::core::LocalFileOperationKind::CreateDirectory,
             temporary.path(),
             {},
             QStringLiteral("created")});
    QVERIFY(!duplicate.allSucceeded());
    QVERIFY(!duplicate.items.constFirst().error.isEmpty());

    const rfm::core::LocalFileOperationResult traversal =
        rfm::core::LocalFileSystem::executeOperation(
            {3,
             rfm::core::LocalFileOperationKind::CreateDirectory,
             temporary.path(),
             {},
             QStringLiteral("../escaped")});
    QVERIFY(!traversal.allSucceeded());
    QVERIFY(!QFileInfo(QDir(temporary.path()).filePath(QStringLiteral("../escaped"))).exists());
    QVERIFY(!rfm::core::LocalFileSystem::isValidName(QStringLiteral("child/name")));
#ifdef Q_OS_WIN
    QVERIFY(!rfm::core::LocalFileSystem::isValidName(QStringLiteral("child\\name")));
#else
    QVERIFY(rfm::core::LocalFileSystem::isValidName(QStringLiteral("child\\name")));
#endif
    QVERIFY(!rfm::core::LocalFileSystem::isValidName(QStringLiteral("..")));
    QVERIFY(!rfm::core::LocalFileSystem::isValidName(QStringLiteral("   ")));
}

void LocalFileSystemTest::renamesLocalFilesAndFoldersWithValidation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir parent(temporary.path());
    QFile file(parent.filePath(QStringLiteral("file.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QVERIFY(parent.mkdir(QStringLiteral("folder")));

    const auto rename = [&parent](quint64 id, const QString& source, const QString& newName) {
        return rfm::core::LocalFileSystem::executeOperation(
            {id,
             rfm::core::LocalFileOperationKind::Rename,
             parent.absolutePath(),
             {source},
             newName});
    };
    const rfm::core::LocalFileOperationResult renamedFile =
        rename(1, parent.filePath(QStringLiteral("file.txt")), QStringLiteral("renamed.txt"));
    QVERIFY2(renamedFile.allSucceeded(), qPrintable(renamedFile.items.constFirst().error));
    QVERIFY(QFileInfo(parent.filePath(QStringLiteral("renamed.txt"))).isFile());

    const rfm::core::LocalFileOperationResult renamedFolder =
        rename(2, parent.filePath(QStringLiteral("folder")), QStringLiteral("renamed-folder"));
    QVERIFY2(renamedFolder.allSucceeded(), qPrintable(renamedFolder.items.constFirst().error));
    QVERIFY(QFileInfo(parent.filePath(QStringLiteral("renamed-folder"))).isDir());

    QFile collision(parent.filePath(QStringLiteral("collision.txt")));
    QVERIFY(collision.open(QIODevice::WriteOnly));
    collision.close();
    const rfm::core::LocalFileOperationResult duplicate =
        rename(3, parent.filePath(QStringLiteral("renamed.txt")), QStringLiteral("collision.txt"));
    QVERIFY(!duplicate.allSucceeded());
    QVERIFY(QFileInfo(parent.filePath(QStringLiteral("renamed.txt"))).exists());

    const rfm::core::LocalFileOperationResult traversal =
        rename(4, parent.filePath(QStringLiteral("renamed.txt")), QStringLiteral("../escaped"));
    QVERIFY(!traversal.allSucceeded());
    QVERIFY(!QFileInfo(parent.filePath(QStringLiteral("../escaped"))).exists());

    const QString linkPath = parent.filePath(QStringLiteral("file-link"));
    if (QFile::link(parent.filePath(QStringLiteral("renamed.txt")), linkPath)) {
        const rfm::core::LocalFileOperationResult renamedLink =
            rename(5, linkPath, QStringLiteral("renamed-link"));
        QVERIFY2(renamedLink.allSucceeded(), qPrintable(renamedLink.items.constFirst().error));
        QVERIFY(QFileInfo(parent.filePath(QStringLiteral("renamed-link"))).exists() ||
                QFileInfo(parent.filePath(QStringLiteral("renamed-link"))).isSymbolicLink());
        QVERIFY(QFileInfo(parent.filePath(QStringLiteral("renamed.txt"))).exists());
    }

    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    QFile outsideFile(QDir(outside.path()).filePath(QStringLiteral("outside.txt")));
    QVERIFY(outsideFile.open(QIODevice::WriteOnly));
    outsideFile.close();
    const rfm::core::LocalFileOperationResult outsideSource =
        rename(6, outsideFile.fileName(), QStringLiteral("stolen.txt"));
    QVERIFY(!outsideSource.allSucceeded());
    QVERIFY(outsideFile.exists());
}

void LocalFileSystemTest::removesLocalSelectionsRecursivelyWithoutFollowingSymlinks()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir parent(temporary.path());
    QFile first(parent.filePath(QStringLiteral("first.txt")));
    QVERIFY(first.open(QIODevice::WriteOnly));
    first.close();
    QVERIFY(parent.mkdir(QStringLiteral("empty")));
    QVERIFY(parent.mkpath(QStringLiteral("recursive/nested")));
    QFile nested(parent.filePath(QStringLiteral("recursive/nested/data.txt")));
    QVERIFY(nested.open(QIODevice::WriteOnly));
    nested.close();

    const rfm::core::LocalFileOperationResult removed =
        rfm::core::LocalFileSystem::executeOperation(
            {1,
             rfm::core::LocalFileOperationKind::Remove,
             parent.absolutePath(),
             {first.fileName(), parent.filePath(QStringLiteral("empty")),
              parent.filePath(QStringLiteral("recursive"))},
             {}});
    QVERIFY2(removed.allSucceeded(), qPrintable(removed.items.constFirst().error));
    QVERIFY(!QFileInfo(first.fileName()).exists());
    QVERIFY(!QFileInfo(parent.filePath(QStringLiteral("empty"))).exists());
    QVERIFY(!QFileInfo(parent.filePath(QStringLiteral("recursive"))).exists());

    QVERIFY(parent.mkpath(QStringLiteral("target")));
    QFile target(parent.filePath(QStringLiteral("target/survives.txt")));
    QVERIFY(target.open(QIODevice::WriteOnly));
    target.close();
    QVERIFY(parent.mkdir(QStringLiteral("with-link")));
    const QString linkPath = parent.filePath(QStringLiteral("with-link/target-link"));
    if (!QFile::link(parent.filePath(QStringLiteral("target")), linkPath)) {
        QSKIP("Symbolic links are not available in this test environment.");
    }
    const rfm::core::LocalFileOperationResult removedLinkTree =
        rfm::core::LocalFileSystem::executeOperation(
            {2,
             rfm::core::LocalFileOperationKind::Remove,
             parent.absolutePath(),
             {parent.filePath(QStringLiteral("with-link"))},
             {}});
    QVERIFY2(removedLinkTree.allSucceeded(), qPrintable(removedLinkTree.items.constFirst().error));
    QVERIFY(QFileInfo(target.fileName()).exists());
    QVERIFY(!QFileInfo(linkPath).exists());

    const rfm::core::LocalFileOperationResult missing =
        rfm::core::LocalFileSystem::executeOperation({3,
                                                      rfm::core::LocalFileOperationKind::Remove,
                                                      parent.absolutePath(),
                                                      {parent.filePath(QStringLiteral("missing"))},
                                                      {}});
    QVERIFY(!missing.allSucceeded());
    QVERIFY(!missing.items.constFirst().error.isEmpty());
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
             QStringLiteral("/media/usb"));
    QCOMPARE(rfm::core::storageDisplayName({}, QStringLiteral("Cruzer Glide"),
                                           QStringLiteral("/dev/sdb1"),
                                           QStringLiteral("/media/usb")),
             QStringLiteral("/media/usb"));
    QCOMPARE(rfm::core::storageDisplayName({}, {}, QStringLiteral("/dev/sdb1"),
                                           QStringLiteral("/media/usb")),
             QStringLiteral("/media/usb"));
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
    QCOMPARE(volume.displayName, QStringLiteral("/media/usb"));
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
    const QByteArray fixture =
        "24 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
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
        "37 24 0:54 / /containers rw - overlay overlay rw\n"
        "38 24 0:55 / /proc/sys/fs/binfmt_misc rw - binfmt_misc binfmt_misc rw\n"
        "39 24 0:56 / /technical rw - fuse none rw\n"
        "40 24 7:0 / /snap/example rw - squashfs /dev/loop0 ro\n"
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
               mount.fileSystemType == QByteArrayLiteral("overlay") ||
               mount.fileSystemType == QByteArrayLiteral("binfmt_misc") ||
               mount.fileSystemType == QByteArrayLiteral("tmpfs") ||
               mount.fileSystemType == QByteArrayLiteral("fuse.portal") ||
               mount.device == QStringLiteral("none") ||
               mount.device == QStringLiteral("/dev/loop0");
    }));
}

void LocalFileSystemTest::filtersPseudoFileSystemsAtRoot()
{
    const QByteArray tmpfsRoot = "24 1 0:44 / / rw - tmpfs tmpfs rw\n"
                                 "25 24 0:45 / /run rw - tmpfs tmpfs rw\n"
                                 "26 24 0:46 / /dev/shm rw - tmpfs shm rw\n";
    const QList<rfm::core::LinuxMountInfo> tmpfsMounts = rfm::core::parseLinuxMountInfo(tmpfsRoot);
    QVERIFY(tmpfsMounts.isEmpty());

    const QList<rfm::core::LinuxMountInfo> ramfsMounts =
        rfm::core::parseLinuxMountInfo("30 1 0:47 / / rw - ramfs ramfs rw\n");
    QVERIFY(ramfsMounts.isEmpty());

    const QList<rfm::core::LinuxMountInfo> ext4Mounts =
        rfm::core::parseLinuxMountInfo("40 1 8:1 / / rw - ext4 /dev/sda1 rw\n");
    QCOMPARE(ext4Mounts.size(), 1);
    QCOMPARE(ext4Mounts.constFirst().fileSystemType, QByteArrayLiteral("ext4"));

    const QList<rfm::core::LinuxMountInfo> overlayRoot =
        rfm::core::parseLinuxMountInfo("41 1 0:48 / / rw - overlay overlay rw\n");
    QVERIFY(overlayRoot.isEmpty());
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

void LocalFileSystemTest::filtersTechnicalMountsAndDeduplicatesMountedDevices()
{
    QTemporaryDir duplicateAttachment;
    QTemporaryDir distinctStorage;
    QVERIFY(duplicateAttachment.isValid());
    QVERIFY(distinctStorage.isValid());

    const QList<rfm::core::LocalStorageMount> mounts{
        {QDir::rootPath(), QStringLiteral("/dev/fixture-root"), QByteArrayLiteral("btrfs"),
         QStringLiteral("ROOT"), 1024, false, true, true},
        {duplicateAttachment.path(), QStringLiteral("/dev/fixture-root"),
         QByteArrayLiteral("btrfs"), QStringLiteral("SNAPSHOT"), 1024, false, true, true},
        {distinctStorage.path(), QStringLiteral("/dev/fixture-data"), QByteArrayLiteral("ext4"),
         QStringLiteral("DATA"), 2048, false, true, true},
        {QDir::tempPath(), QStringLiteral("tmpfs"), QByteArrayLiteral("tmpfs"), {}, 1024, false,
         true, true},
        {duplicateAttachment.path(), QStringLiteral("none"), QByteArrayLiteral("fuse"), {},
         1024, false, true, true}};

    const rfm::core::LocalStorageSnapshot snapshot =
        rfm::core::LocalFileSystem::makeStorageSnapshot(mounts);

    QCOMPARE(snapshot.volumes.size(), 2);
    QVERIFY(std::ranges::any_of(snapshot.volumes, [](const rfm::core::StorageVolume& volume) {
        return volume.rootPath == QStringLiteral("/") &&
               volume.device == QStringLiteral("/dev/fixture-root");
    }));
    QVERIFY(std::ranges::any_of(snapshot.volumes, [&distinctStorage](
                                                 const rfm::core::StorageVolume& volume) {
        return volume.rootPath == distinctStorage.path() &&
               volume.device == QStringLiteral("/dev/fixture-data");
    }));
}

void LocalFileSystemTest::discoversUnmountedUsbPartitionWithoutTechnicalDuplicates()
{
    const QByteArray fixture = R"json({
        "blockdevices": [
            {
                "path": "/dev/sde", "pkname": null, "type": "disk",
                "fstype": "vfat", "label": "PARENT", "size": 123000,
                "mountpoint": null, "ro": false, "rm": true, "tran": "usb",
                "model": "USB Reader",
                "children": [
                    {
                        "path": "/dev/sde1", "pkname": "/dev/sde", "type": "part",
                        "fstype": "vfat", "label": "PHOTOS", "size": 120000,
                        "mountpoint": null, "ro": false, "rm": true, "tran": null,
                        "model": null
                    },
                    {
                        "path": "/dev/sde2", "pkname": "/dev/sde", "type": "part",
                        "fstype": "swap", "label": null, "size": 3000,
                        "mountpoint": null, "ro": false, "rm": true, "tran": "usb",
                        "model": "USB Reader"
                    }
                ]
            }
        ]
    })json";

    const QList<rfm::core::LocalBlockDevice> devices =
        rfm::core::LocalFileSystem::parseLinuxBlockDevices(fixture);
    QCOMPARE(devices.size(), 3);
    const rfm::core::LocalStorageSnapshot snapshot =
        rfm::core::LocalFileSystem::makeStorageSnapshot({}, devices);

    QCOMPARE(snapshot.volumes.size(), 1);
    const rfm::core::StorageVolume& volume = snapshot.volumes.constFirst();
    QCOMPARE(volume.device, QStringLiteral("/dev/sde1"));
    QCOMPARE(volume.displayName, QStringLiteral("PHOTOS"));
    QCOMPARE(volume.fileSystemType, QByteArrayLiteral("vfat"));
    QCOMPARE(volume.bytesTotal, quint64{120000});
    QCOMPARE(volume.kind, rfm::core::StorageKind::External);
    QCOMPARE(volume.deviceModel, QStringLiteral("USB Reader"));
    QVERIFY(volume.removable);
    QVERIFY(!volume.mounted);
    QVERIFY(volume.rootPath.isEmpty());
}

void LocalFileSystemTest::keepsMountedVolumeWhenBlockDiscoveryReportsItToo()
{
    QTemporaryDir mountPoint;
    QVERIFY(mountPoint.isValid());
    const QList<rfm::core::LocalStorageMount> mounts{
        {mountPoint.path(), QStringLiteral("/dev/sde1"), QByteArrayLiteral("vfat"),
         QStringLiteral("PHOTOS"), 120000, false, true, true}};
    const QList<rfm::core::LocalBlockDevice> devices{
        {QStringLiteral("/dev/sde1"), QStringLiteral("/dev/sde"), QStringLiteral("part"),
         QByteArrayLiteral("vfat"), QStringLiteral("PHOTOS"), mountPoint.path(),
         QStringLiteral("usb"), QStringLiteral("USB Reader"), 120000, true, false}};

    const rfm::core::LocalStorageSnapshot snapshot =
        rfm::core::LocalFileSystem::makeStorageSnapshot(mounts, devices);

    QCOMPARE(snapshot.volumes.size(), 1);
    QCOMPARE(snapshot.volumes.constFirst().device, QStringLiteral("/dev/sde1"));
    QCOMPARE(snapshot.volumes.constFirst().rootPath, mountPoint.path());
    QVERIFY(snapshot.volumes.constFirst().mounted);
}

void LocalFileSystemTest::deduplicatesBlockAliasesByDeviceNumber()
{
    QTemporaryDir mountPoint;
    QVERIFY(mountPoint.isValid());
    const QList<rfm::core::LocalStorageMount> mounts{
        {mountPoint.path(), QStringLiteral("/dev/mapper/archive"), QByteArrayLiteral("ext4"),
         QStringLiteral("ARCHIVE"), 120000, false, true, true, QStringLiteral("253:3")}};
    const QByteArray blockFixture = R"json({"blockdevices": [
        {"path": "/dev/dm-3", "type": "dm", "fstype": "ext4", "label": "ARCHIVE",
         "size": 120000, "mountpoints": [], "ro": false, "rm": false, "maj:min": "253:3"},
        {"path": "/dev/dm-4", "type": "dm", "fstype": "ext4", "label": "DISTINCT",
         "size": 240000, "mountpoints": [], "ro": false, "rm": false, "maj:min": "253:4"}
    ]})json";
    const QList<rfm::core::LocalBlockDevice> devices =
        rfm::core::LocalFileSystem::parseLinuxBlockDevices(blockFixture);
    QCOMPARE(devices.size(), 2);

    const rfm::core::LocalStorageSnapshot snapshot =
        rfm::core::LocalFileSystem::makeStorageSnapshot(mounts, devices);

    QCOMPARE(snapshot.volumes.size(), 2);
    QCOMPARE(std::ranges::count_if(snapshot.volumes,
                                   [](const rfm::core::StorageVolume& volume) {
                                       return volume.deviceNumber == QStringLiteral("253:3");
                                   }),
             1);
    QVERIFY(std::ranges::any_of(snapshot.volumes, [](const rfm::core::StorageVolume& volume) {
        return volume.device == QStringLiteral("/dev/mapper/archive") && volume.mounted;
    }));
    QVERIFY(std::ranges::any_of(snapshot.volumes, [](const rfm::core::StorageVolume& volume) {
        return volume.deviceNumber == QStringLiteral("253:4") && !volume.mounted;
    }));
}

void LocalFileSystemTest::matchesLocalPathsOnComponentBoundaries()
{
    QVERIFY(
        rfm::core::localPathIsAtOrBelow(QStringLiteral("/mnt/disk"), QStringLiteral("/mnt/disk/")));
    QVERIFY(rfm::core::localPathIsAtOrBelow(QStringLiteral("/mnt/disk/Films/../Music"),
                                            QStringLiteral("/mnt/disk")));
    QVERIFY(!rfm::core::localPathIsAtOrBelow(QStringLiteral("/mnt/disk2/Films"),
                                             QStringLiteral("/mnt/disk")));
    QVERIFY(!rfm::core::localPathIsAtOrBelow({}, QStringLiteral("/mnt/disk")));
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

void LocalFileSystemTest::deduplicatesMultipleAttachmentsOfOneStorage()
{
    const QByteArray fixture = "60 1 259:2 /@ / rw - btrfs /dev/nvme0n1p2 rw\n"
                               "61 60 259:2 /@home /home rw - btrfs /dev/nvme0n1p2 rw\n"
                               "70 1 8:9 /alpha /mnt/alpha rw - ext4 /dev/sdd1 rw\n"
                               "71 1 8:9 /beta /mnt/beta rw - ext4 /dev/sdd1 rw\n"
                               "80 1 8:10 / /mnt/first rw - ext4 /dev/sde1 rw\n"
                               "81 1 8:10 / /mnt/second rw - ext4 /dev/sde1 rw\n";
    const QList<rfm::core::LinuxMountInfo> mounts = rfm::core::parseLinuxMountInfo(fixture);

    QCOMPARE(mounts.size(), 3);
    QCOMPARE(mounts.at(0).rootPath, QStringLiteral("/"));
    QCOMPARE(mounts.at(0).mountRoot, QStringLiteral("/@"));
    QCOMPARE(mounts.at(1).rootPath, QStringLiteral("/mnt/beta"));
    QCOMPARE(mounts.at(2).rootPath, QStringLiteral("/mnt/first"));
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
