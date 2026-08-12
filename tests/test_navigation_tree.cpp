#include "remotefilemanager/app/NavigationTree.hpp"

#include <QDir>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTreeWidget>

#include <utility>

class NavigationTreeTest final : public QObject
{
    Q_OBJECT

  private slots:
    void buildsMachinesProfilesAndMachineScopedVolumes();
    void loadsLocalChildrenOnlyWhenExpanded();
    void exposesOnlyTheActiveServerFileTree();
    void omitsEmptyExternalDevicesCategory();
    void deduplicatesAndNavigatesExternalDevice();
    void refreshesRemoteStorageWithoutMixingMachines();
    void preservesRemoteMountpointsForOneDevice();
    void showsHumanMetadataAndRefreshesWithoutChangingNavigation();
    void preservesLoadedTreeStateDuringStorageRefresh();
    void escapesTooltipMetadataAndDisambiguatesLabels();
    void exposesOnlyAppropriateLocalVolumeActions();
    void showsPerVolumeBusyStates();
    void opensVolumeOnlyFromRefreshedMountPoint();
    void toleratesVolumeDisappearingDuringOperation();
    void exposesRemoteVolumeActionsInTheActiveServerNamespace();
};

namespace
{

rfm::core::StorageVolume storageVolume(const QString& name, const QString& rootPath,
                                       rfm::core::StorageKind kind, bool removable = false)
{
    rfm::core::StorageVolume volume;
    volume.displayName = name;
    volume.rootPath = rootPath;
    volume.kind = kind;
    volume.removable = removable;
    return volume;
}

QTreeWidgetItem* childNamed(QTreeWidgetItem* parent, const QString& name)
{
    for (int index = 0; index < parent->childCount(); ++index) {
        if (parent->child(index)->text(0) == name) {
            return parent->child(index);
        }
    }
    return nullptr;
}

QTreeWidgetItem* volumeItemByDevice(QTreeWidgetItem* root, const QString& device)
{
    if (root == nullptr) {
        return nullptr;
    }
    if (root->data(0, Qt::UserRole + 6).isValid() &&
        root->data(0, Qt::UserRole + 6).value<rfm::core::StorageVolume>().device == device) {
        return root;
    }
    for (int index = 0; index < root->childCount(); ++index) {
        if (QTreeWidgetItem* const match = volumeItemByDevice(root->child(index), device)) {
            return match;
        }
    }
    return nullptr;
}

QList<QTreeWidgetItem*> volumeItemsByDevice(QTreeWidgetItem* root, const QString& device)
{
    QList<QTreeWidgetItem*> matches;
    if (root == nullptr) {
        return matches;
    }
    if (root->data(0, Qt::UserRole + 6).isValid() &&
        root->data(0, Qt::UserRole + 6).value<rfm::core::StorageVolume>().device == device) {
        matches.push_back(root);
    }
    for (int index = 0; index < root->childCount(); ++index) {
        matches.append(volumeItemsByDevice(root->child(index), device));
    }
    return matches;
}

rfm::app::RemoteMachineDescriptor remoteMachine(const QString& id,
                                                const QString& name = QStringLiteral("Remote"),
                                                const QString& host = QStringLiteral("remote.test"))
{
    return {id, name, host, QStringLiteral("alice"), 22, id};
}

} // namespace

void NavigationTreeTest::buildsMachinesProfilesAndMachineScopedVolumes()
{
    QTemporaryDir temporary;
    QTemporaryDir externalTemporary;
    QTemporaryDir unknownTemporary;
    QVERIFY(temporary.isValid());
    QVERIFY(externalTemporary.isValid());
    QVERIFY(unknownTemporary.isValid());
    rfm::app::NavigationTree navigation;
    QTreeWidget* const tree = navigation.tree();
    QCOMPARE(tree->topLevelItemCount(), 2);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("This Computer"));
    QCOMPARE(tree->topLevelItem(1)->text(0), QStringLiteral("Servers"));

    navigation.setStorageVolumes(
        {storageVolume(QStringLiteral("Fixture volume"), temporary.path(),
                       rfm::core::StorageKind::Internal),
         storageVolume(QStringLiteral("USB fixture"), externalTemporary.path(),
                       rfm::core::StorageKind::External, true),
         storageVolume(QStringLiteral("Unknown fixture"), unknownTemporary.path(),
                       rfm::core::StorageKind::Unknown)});
    QTreeWidgetItem* const volumes = childNamed(tree->topLevelItem(0), QStringLiteral("Volumes"));
    QTreeWidgetItem* const externalDevices =
        childNamed(tree->topLevelItem(0), QStringLiteral("External devices"));
    QVERIFY(volumes != nullptr);
    QVERIFY(externalDevices != nullptr);
    QCOMPARE(volumes->childCount(), 2);
    QCOMPARE(volumes->child(0)->text(0), QStringLiteral("Fixture volume"));
    QCOMPARE(volumes->child(1)->text(0), QStringLiteral("Unknown fixture"));
    QVERIFY(volumes->parent() == tree->topLevelItem(0));
    QCOMPARE(externalDevices->childCount(), 1);
    QCOMPARE(externalDevices->child(0)->text(0), QStringLiteral("USB fixture"));
    QVERIFY(externalDevices->parent() == tree->topLevelItem(0));

    navigation.setProfiles({{QStringLiteral("NAS"), QStringLiteral("nas.test"),
                             QStringLiteral("alice"), 22, QStringLiteral("nas-id")}});
    QCOMPARE(tree->topLevelItem(1)->childCount(), 1);
    QCOMPARE(tree->topLevelItem(1)->child(0)->text(0), QStringLiteral("NAS"));
}

void NavigationTreeTest::loadsLocalChildrenOnlyWhenExpanded()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::NavigationTree navigation;
    navigation.setStorageVolumes({storageVolume(QStringLiteral("Fixture"), temporary.path(),
                                                rfm::core::StorageKind::Internal)});
    QTreeWidgetItem* const volumes =
        childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("Volumes"));
    QVERIFY(volumes != nullptr);
    QTreeWidgetItem* const fixture = volumes->child(0);
    QCOMPARE(fixture->childCount(), 1);
    QCOMPARE(fixture->child(0)->text(0), QStringLiteral("Expand to load"));

    QSignalSpy expansion(&navigation, &rfm::app::NavigationTree::localDirectoryExpansionRequested);
    fixture->setExpanded(true);
    QCOMPARE(expansion.size(), 1);
    QCOMPARE(expansion.constFirst().constFirst().toString(), QDir(temporary.path()).absolutePath());

    navigation.setLocalDirectory(temporary.path(),
                                 {{QStringLiteral("child"), 0, {}, true, false},
                                  {QStringLiteral("ignored.txt"), 1, {}, false, false}});
    QCOMPARE(fixture->childCount(), 1);
    QCOMPARE(fixture->child(0)->text(0), QStringLiteral("child"));
    QCOMPARE(fixture->child(0)->childCount(), 1);
    fixture->setExpanded(false);
    QVERIFY(!fixture->isExpanded());
    fixture->setExpanded(true);
    QCOMPARE(expansion.size(), 1);
}

void NavigationTreeTest::exposesOnlyTheActiveServerFileTree()
{
    rfm::app::NavigationTree navigation;
    navigation.setProfiles({{QStringLiteral("Active"), QStringLiteral("active.test"),
                             QStringLiteral("alice"), 22, QStringLiteral("active-id")},
                            {QStringLiteral("Offline"), QStringLiteral("offline.test"),
                             QStringLiteral("bob"), 22, QStringLiteral("offline-id")}});
    navigation.setActiveServer(remoteMachine(QStringLiteral("active-id"), QStringLiteral("Active"),
                                             QStringLiteral("active.test")),
                               QStringLiteral("/home/alice"));
    QTreeWidgetItem* const servers = navigation.tree()->topLevelItem(1);
    QCOMPARE(servers->childCount(), 2);
    QVERIFY(servers->child(0)->text(0).contains(QStringLiteral("Connected")));
    QVERIFY(servers->child(0)->childCount() >= 1);
    QCOMPARE(servers->child(1)->childCount(), 0);

    QSignalSpy expansion(&navigation, &rfm::app::NavigationTree::remoteDirectoryExpansionRequested);
    servers->child(0)->child(0)->setExpanded(true);
    QCOMPARE(expansion.size(), 1);
    QCOMPARE(expansion.constFirst().at(0).toString(), QStringLiteral("active-id"));
    QCOMPARE(expansion.constFirst().at(1).toString(), QStringLiteral("/home/alice"));

    navigation.setProfiles({{QStringLiteral("Edited saved name"), QStringLiteral("changed.test"),
                             QStringLiteral("mallory"), 2200, QStringLiteral("active-id")},
                            {QStringLiteral("Offline"), QStringLiteral("offline.test"),
                             QStringLiteral("bob"), 22, QStringLiteral("offline-id")}});
    QCOMPARE(servers->childCount(), 3);
    QCOMPARE(servers->child(0)->text(0), QStringLiteral("Edited saved name"));
    QVERIFY(!servers->child(0)->data(0, Qt::UserRole + 4).toBool());
    QTreeWidgetItem* const activeAfterEdit = servers->child(2);
    QVERIFY(activeAfterEdit->text(0).startsWith(QStringLiteral("Active")));
    QVERIFY(activeAfterEdit->toolTip(0).contains(QStringLiteral("alice@active.test:22")));
    QVERIFY(childNamed(activeAfterEdit, QStringLiteral("Home")) != nullptr);

    navigation.setProfiles({{QStringLiteral("Offline"), QStringLiteral("offline.test"),
                             QStringLiteral("bob"), 22, QStringLiteral("offline-id")}});
    QCOMPARE(servers->childCount(), 2);
    QTreeWidgetItem* const temporaryActive = servers->child(1);
    QVERIFY(temporaryActive->text(0).contains(QStringLiteral("Connected")));
    QVERIFY(childNamed(temporaryActive, QStringLiteral("Home")) != nullptr);
    QVERIFY(childNamed(temporaryActive, QStringLiteral("/")) != nullptr);
    QVERIFY(childNamed(temporaryActive, QStringLiteral("Volumes")) != nullptr);

    navigation.clearActiveServer();
    QCOMPARE(servers->childCount(), 1);
    QCOMPARE(servers->child(0)->childCount(), 0);
}

void NavigationTreeTest::omitsEmptyExternalDevicesCategory()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::NavigationTree navigation;
    navigation.setStorageVolumes({storageVolume(
        QStringLiteral("Temporary external"), temporary.path(), rfm::core::StorageKind::External)});
    QVERIFY(childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices")) !=
            nullptr);
    navigation.setStorageVolumes({storageVolume(QStringLiteral("Internal"), temporary.path(),
                                                rfm::core::StorageKind::Internal)});
    QVERIFY(childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices")) ==
            nullptr);
}

void NavigationTreeTest::deduplicatesAndNavigatesExternalDevice()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::NavigationTree navigation;
    navigation.setStorageVolumes({storageVolume(QStringLiteral("Duplicate internal"),
                                                temporary.path(), rfm::core::StorageKind::Internal),
                                  storageVolume(QStringLiteral("USB SSD"), temporary.path(),
                                                rfm::core::StorageKind::External, false)});
    QTreeWidgetItem* const machine = navigation.tree()->topLevelItem(0);
    QTreeWidgetItem* const volumes = childNamed(machine, QStringLiteral("Volumes"));
    QTreeWidgetItem* const externalDevices =
        childNamed(machine, QStringLiteral("External devices"));
    QVERIFY(volumes != nullptr);
    QVERIFY(externalDevices != nullptr);
    QCOMPARE(volumes->childCount(), 0);
    QCOMPARE(externalDevices->childCount(), 1);
    QCOMPARE(externalDevices->child(0)->text(0), QStringLiteral("USB SSD"));

    QSignalSpy activated(&navigation, &rfm::app::NavigationTree::localLocationActivated);
    QVERIFY(QMetaObject::invokeMethod(navigation.tree(), "itemActivated", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, externalDevices->child(0)),
                                      Q_ARG(int, 0)));
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.constFirst().constFirst().toString(), QDir(temporary.path()).absolutePath());
}

void NavigationTreeTest::refreshesRemoteStorageWithoutMixingMachines()
{
    rfm::app::NavigationTree navigation;
    navigation.setProfiles({{QStringLiteral("Remote"), QStringLiteral("remote.test"),
                             QStringLiteral("alice"), 22, QStringLiteral("remote-id")}});
    navigation.setActiveServer(remoteMachine(QStringLiteral("remote-id")),
                               QStringLiteral("/home/alice"));
    navigation.setRemoteStorageVolumes(
        QStringLiteral("remote-id"),
        {storageVolume(QStringLiteral("Remote internal"), QStringLiteral("/srv"),
                       rfm::core::StorageKind::Internal),
         storageVolume(QStringLiteral("Remote USB"), QStringLiteral("/media/usb"),
                       rfm::core::StorageKind::External)});

    QTreeWidgetItem* const localMachine = navigation.tree()->topLevelItem(0);
    QTreeWidgetItem* const remoteMachine = navigation.tree()->topLevelItem(1)->child(0);
    QCOMPARE(childNamed(localMachine, QStringLiteral("Volumes"))->childCount(), 0);
    QCOMPARE(childNamed(remoteMachine, QStringLiteral("Volumes"))->childCount(), 1);
    QCOMPARE(childNamed(remoteMachine, QStringLiteral("External devices"))->childCount(), 1);

    navigation.setRemoteStorageVolumes(
        QStringLiteral("remote-id"),
        {storageVolume(QStringLiteral("Replacement"), QStringLiteral("/data"),
                       rfm::core::StorageKind::Internal)});
    QTreeWidgetItem* const refreshedRemote = navigation.tree()->topLevelItem(1)->child(0);
    QTreeWidgetItem* const volumes = childNamed(refreshedRemote, QStringLiteral("Volumes"));
    QVERIFY(volumes != nullptr);
    QCOMPARE(volumes->childCount(), 1);
    QCOMPARE(volumes->child(0)->text(0), QStringLiteral("Replacement"));
    QVERIFY(childNamed(refreshedRemote, QStringLiteral("External devices")) == nullptr);

    QSignalSpy activated(&navigation, &rfm::app::NavigationTree::remoteLocationActivated);
    navigation.tree()->setCurrentItem(volumes->child(0));
    QVERIFY(QMetaObject::invokeMethod(navigation.tree(), "itemActivated", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, volumes->child(0)), Q_ARG(int, 0)));
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.constFirst().at(0).toString(), QStringLiteral("remote-id"));
    QCOMPARE(activated.constFirst().at(1).toString(), QStringLiteral("/data"));
}

void NavigationTreeTest::preservesRemoteMountpointsForOneDevice()
{
    rfm::app::NavigationTree navigation;
    navigation.setProfiles({{QStringLiteral("Remote"), QStringLiteral("remote.test"),
                             QStringLiteral("alice"), 22, QStringLiteral("remote-id")}});
    navigation.setActiveServer(remoteMachine(QStringLiteral("remote-id")),
                               QStringLiteral("/home/alice"));

    rfm::core::StorageVolume first;
    first.displayName = QStringLiteral("Btrfs data");
    first.device = QStringLiteral("/dev/sda2");
    first.rootPath = QStringLiteral("/mnt/data-a");
    first.fileSystemType = QByteArrayLiteral("btrfs");
    first.kind = rfm::core::StorageKind::Internal;
    first.mounted = true;
    rfm::core::StorageVolume second = first;
    second.rootPath = QStringLiteral("/mnt/data-b");
    rfm::core::StorageVolume duplicate = first;

    navigation.setRemoteStorageVolumes(QStringLiteral("remote-id"), {first, second, duplicate});
    QTreeWidgetItem* const server = navigation.tree()->topLevelItem(1)->child(0);
    QList<QTreeWidgetItem*> items = volumeItemsByDevice(server, first.device);
    QCOMPARE(items.size(), 2);
    QSet<QString> paths;
    for (QTreeWidgetItem* const item : std::as_const(items)) {
        paths.insert(item->data(0, Qt::UserRole + 6).value<rfm::core::StorageVolume>().rootPath);
    }
    QCOMPARE(paths, QSet<QString>({QStringLiteral("/mnt/data-a"), QStringLiteral("/mnt/data-b")}));

    navigation.setVolumeOperation(QStringLiteral("remote-id"), first.device,
                                  rfm::core::VolumeOperation::Unmount);
    items = volumeItemsByDevice(server, first.device);
    auto* const unmountButton =
        navigation.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"));
    QVERIFY(unmountButton != nullptr);
    for (QTreeWidgetItem* const item : std::as_const(items)) {
        QVERIFY(item->text(0).contains(QStringLiteral("Unmounting")));
        navigation.tree()->setCurrentItem(item);
        QVERIFY(!unmountButton->isEnabled());
    }
    navigation.setVolumeOperation(QStringLiteral("remote-id"), first.device, std::nullopt);

    rfm::core::StorageVolume changed = first;
    changed.rootPath = QStringLiteral("/mnt/data-c");
    navigation.setRemoteStorageVolumes(QStringLiteral("remote-id"), {changed});
    items = volumeItemsByDevice(server, first.device);
    QCOMPARE(items.size(), 1);
    QCOMPARE(
        items.constFirst()->data(0, Qt::UserRole + 6).value<rfm::core::StorageVolume>().rootPath,
        QStringLiteral("/mnt/data-c"));

    rfm::core::StorageVolume available = first;
    available.rootPath.clear();
    available.mounted = false;
    rfm::core::StorageVolume duplicateAvailable = available;
    duplicateAvailable.displayName = QStringLiteral("Duplicate available device");
    navigation.setRemoteStorageVolumes(QStringLiteral("remote-id"),
                                       {available, duplicateAvailable});
    QCOMPARE(volumeItemsByDevice(server, first.device).size(), 1);
}

void NavigationTreeTest::exposesRemoteVolumeActionsInTheActiveServerNamespace()
{
    rfm::app::NavigationTree navigation;
    navigation.setProfiles({{QStringLiteral("Remote"), QStringLiteral("remote.test"),
                             QStringLiteral("alice"), 22, QStringLiteral("remote-id")}});
    navigation.setActiveServer(remoteMachine(QStringLiteral("remote-id")),
                               QStringLiteral("/home/alice"));
    rfm::core::StorageVolume available;
    available.displayName = QStringLiteral("Remote USB");
    available.device = QStringLiteral("/dev/sdb1");
    available.fileSystemType = QByteArrayLiteral("ext4");
    available.kind = rfm::core::StorageKind::External;
    available.mounted = false;
    rfm::core::StorageVolume mounted = available;
    mounted.displayName = QStringLiteral("Remote data");
    mounted.device = QStringLiteral("/dev/sdc1");
    mounted.rootPath = QStringLiteral("/mnt/data");
    mounted.mounted = true;
    rfm::core::StorageVolume system = mounted;
    system.displayName = QStringLiteral("Remote system");
    system.device = QStringLiteral("/dev/sda1");
    system.rootPath = QStringLiteral("/");
    system.kind = rfm::core::StorageKind::System;
    navigation.setRemoteStorageVolumes(QStringLiteral("remote-id"), {available, mounted, system});

    auto* const mountButton =
        navigation.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    auto* const openButton = navigation.findChild<QPushButton*>(QStringLiteral("openVolumeButton"));
    auto* const unmountButton =
        navigation.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"));
    QTreeWidgetItem* const server = navigation.tree()->topLevelItem(1)->child(0);
    QTreeWidgetItem* const external = childNamed(server, QStringLiteral("External devices"));
    QTreeWidgetItem* const volumes = childNamed(server, QStringLiteral("Volumes"));
    QVERIFY(external != nullptr);
    QVERIFY(volumes != nullptr);
    QSignalSpy mounts(&navigation, &rfm::app::NavigationTree::remoteVolumeMountRequested);
    QSignalSpy unmounts(&navigation, &rfm::app::NavigationTree::remoteVolumeUnmountRequested);
    QSignalSpy opens(&navigation, &rfm::app::NavigationTree::remoteLocationActivated);

    navigation.tree()->setCurrentItem(external->child(0));
    QVERIFY(mountButton->isVisibleTo(&navigation));
    QVERIFY(mountButton->isEnabled());
    mountButton->click();
    QCOMPARE(mounts.size(), 1);
    QCOMPARE(mounts.constFirst().at(0).toString(), QStringLiteral("remote-id"));
    navigation.setVolumeOperation(QStringLiteral("remote-id"), QStringLiteral("/dev/sdb1"),
                                  rfm::core::VolumeOperation::Mount);
    QVERIFY(!mountButton->isEnabled());

    QTreeWidgetItem* const mountedItem = volumeItemByDevice(server, QStringLiteral("/dev/sdc1"));
    QTreeWidgetItem* const systemItem = volumeItemByDevice(server, QStringLiteral("/dev/sda1"));
    QVERIFY(mountedItem != nullptr);
    QVERIFY(systemItem != nullptr);
    navigation.tree()->setCurrentItem(mountedItem);
    QVERIFY(openButton->isVisibleTo(&navigation));
    QVERIFY(unmountButton->isVisibleTo(&navigation));
    openButton->click();
    QCOMPARE(opens.size(), 1);
    QCOMPARE(opens.constFirst().at(0).toString(), QStringLiteral("remote-id"));
    QCOMPARE(opens.constFirst().at(1).toString(), QStringLiteral("/mnt/data"));
    unmountButton->click();
    QCOMPARE(unmounts.size(), 1);

    navigation.tree()->setCurrentItem(systemItem);
    QVERIFY(openButton->isVisibleTo(&navigation));
    QVERIFY(!unmountButton->isVisibleTo(&navigation));
}

void NavigationTreeTest::showsHumanMetadataAndRefreshesWithoutChangingNavigation()
{
    QTemporaryDir mountPoint;
    QVERIFY(mountPoint.isValid());
    rfm::app::NavigationTree navigation;
    rfm::core::StorageVolume volume;
    volume.rootPath = mountPoint.path();
    volume.device = QStringLiteral("/dev/sdb1");
    volume.fileSystemType = QByteArrayLiteral("exfat");
    volume.bytesTotal = 123ULL * 1024ULL * 1024ULL * 1024ULL;
    volume.kind = rfm::core::StorageKind::External;
    volume.fileSystemLabel = QStringLiteral("PHOTOS");
    volume.deviceModel = QStringLiteral("SanDisk Cruzer Glide");
    volume.displayName = rfm::core::storageDisplayName(volume.fileSystemLabel, volume.deviceModel,
                                                       volume.device, volume.rootPath);
    navigation.setStorageVolumes({volume});

    QTreeWidgetItem* const machine = navigation.tree()->topLevelItem(0);
    QTreeWidgetItem* externalDevices = childNamed(machine, QStringLiteral("External devices"));
    QVERIFY(externalDevices != nullptr);
    QCOMPARE(externalDevices->childCount(), 1);
    QTreeWidgetItem* entry = externalDevices->child(0);
    QCOMPARE(entry->text(0), QStringLiteral("PHOTOS"));
    QTextDocument fullDocument;
    fullDocument.setHtml(entry->toolTip(0));
    const QString fullToolTip = fullDocument.toPlainText();
    QVERIFY(fullToolTip.startsWith(QStringLiteral("PHOTOS\n")));
    QVERIFY(fullToolTip.contains(QStringLiteral("Device: /dev/sdb1")));
    QVERIFY(fullToolTip.contains(QStringLiteral("Mount point: %1").arg(mountPoint.path())));
    QVERIFY(fullToolTip.contains(QStringLiteral("Filesystem: exfat")));
    QVERIFY(fullToolTip.contains(QStringLiteral("Model: SanDisk Cruzer Glide")));
    QVERIFY(fullToolTip.contains(QStringLiteral("Size:")));
    QVERIFY(fullToolTip.split(QChar{'\n'}).size() <= 7);

    QSignalSpy activated(&navigation, &rfm::app::NavigationTree::localLocationActivated);
    QVERIFY(QMetaObject::invokeMethod(navigation.tree(), "itemActivated", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, entry), Q_ARG(int, 0)));
    QCOMPARE(activated.constFirst().constFirst().toString(),
             QDir(mountPoint.path()).absolutePath());

    volume.fileSystemLabel.clear();
    volume.deviceModel = QStringLiteral("Replacement model");
    volume.fileSystemType = QByteArrayLiteral("Unknown");
    volume.bytesTotal = 0;
    volume.displayName = rfm::core::storageDisplayName(volume.fileSystemLabel, volume.deviceModel,
                                                       volume.device, volume.rootPath);
    navigation.setStorageVolumes({volume});
    externalDevices = childNamed(machine, QStringLiteral("External devices"));
    QVERIFY(externalDevices != nullptr);
    QCOMPARE(externalDevices->childCount(), 1);
    entry = externalDevices->child(0);
    QCOMPARE(entry->text(0), QStringLiteral("Replacement model"));
    QTextDocument partialDocument;
    partialDocument.setHtml(entry->toolTip(0));
    const QString partialToolTip = partialDocument.toPlainText();
    QVERIFY(partialToolTip.startsWith(QStringLiteral("Replacement model\n")));
    QVERIFY(!partialToolTip.contains(QStringLiteral("Filesystem:")));
    QVERIFY(!partialToolTip.contains(QStringLiteral("Size:")));
    QVERIFY(!partialToolTip.contains(QStringLiteral("Unknown")));
    QVERIFY(!partialToolTip.contains(QStringLiteral("0 B")));
    QVERIFY(!partialToolTip.contains(QStringLiteral("\n\n")));
}

void NavigationTreeTest::preservesLoadedTreeStateDuringStorageRefresh()
{
    QTemporaryDir localMount;
    QVERIFY(localMount.isValid());
    rfm::app::NavigationTree navigation;
    rfm::core::StorageVolume local = storageVolume(
        QStringLiteral("Local volume"), localMount.path(), rfm::core::StorageKind::Internal);
    navigation.setStorageVolumes({local});
    QTreeWidgetItem* const localVolumes =
        childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("Volumes"));
    QTreeWidgetItem* const localItem = localVolumes->child(0);
    localItem->setExpanded(true);
    navigation.setLocalDirectory(localMount.path(), {{QStringLiteral("kept"), 0, {}, true, false}});
    QTreeWidgetItem* const localChild = localItem->child(0);
    navigation.tree()->setCurrentItem(localChild);
    local.displayName = QStringLiteral("Renamed local volume");
    navigation.setStorageVolumes({local});
    QCOMPARE(localVolumes->child(0), localItem);
    QVERIFY(localItem->isExpanded());
    QCOMPARE(localItem->child(0), localChild);
    QCOMPARE(navigation.tree()->currentItem(), localChild);

    navigation.setProfiles({{QStringLiteral("Remote"), QStringLiteral("remote.test"),
                             QStringLiteral("alice"), 22, QStringLiteral("remote-id")}});
    navigation.setActiveServer(remoteMachine(QStringLiteral("remote-id")),
                               QStringLiteral("/home/alice"));
    QTreeWidgetItem* const server = navigation.tree()->topLevelItem(1)->child(0);
    QTreeWidgetItem* const home = childNamed(server, QStringLiteral("Home"));
    home->setExpanded(true);
    navigation.setRemoteDirectory(QStringLiteral("remote-id"), QStringLiteral("/home/alice"),
                                  {{QStringLiteral("projects"), 0, {}, true, false}});
    QTreeWidgetItem* const remoteChild = home->child(0);
    navigation.tree()->setCurrentItem(remoteChild);
    navigation.setRemoteStorageVolumes(
        QStringLiteral("remote-id"), {storageVolume(QStringLiteral("Data"), QStringLiteral("/data"),
                                                    rfm::core::StorageKind::Internal)});
    QCOMPARE(navigation.tree()->topLevelItem(1)->child(0), server);
    QVERIFY(home->isExpanded());
    QCOMPARE(home->child(0), remoteChild);
    QCOMPARE(navigation.tree()->currentItem(), remoteChild);
}

void NavigationTreeTest::escapesTooltipMetadataAndDisambiguatesLabels()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    rfm::app::NavigationTree navigation;
    rfm::core::StorageVolume firstVolume =
        storageVolume(QStringLiteral("BACKUP"), first.path(), rfm::core::StorageKind::External);
    firstVolume.device = QStringLiteral("/dev/sdb1");
    firstVolume.deviceModel = QStringLiteral("Model <qt><b> & <tag>\nFilesystem: fake");
    rfm::core::StorageVolume secondVolume =
        storageVolume(QStringLiteral("BACKUP"), second.path(), rfm::core::StorageKind::External);
    secondVolume.device = QStringLiteral("/dev/sdc1");
    navigation.setStorageVolumes({firstVolume, secondVolume});

    QTreeWidgetItem* const external =
        childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices"));
    QCOMPARE(external->childCount(), 2);
    QCOMPARE(external->child(0)->text(0), QStringLiteral("BACKUP (sdb1)"));
    QCOMPARE(external->child(1)->text(0), QStringLiteral("BACKUP (sdc1)"));
    QVERIFY(external->child(0)->toolTip(0).contains(QStringLiteral("&lt;qt&gt;")));
    QVERIFY(external->child(0)->toolTip(0).contains(QStringLiteral("&amp;")));
    QTextDocument document;
    document.setHtml(external->child(0)->toolTip(0));
    const QString plainText = document.toPlainText();
    QVERIFY(plainText.contains(QStringLiteral("Model: Model <qt><b> & <tag> Filesystem: fake")));
    QVERIFY(!plainText.contains(QStringLiteral("<tag>\nFilesystem: fake")));

    QSignalSpy activated(&navigation, &rfm::app::NavigationTree::localLocationActivated);
    QVERIFY(QMetaObject::invokeMethod(navigation.tree(), "itemActivated", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, external->child(0)), Q_ARG(int, 0)));
    QVERIFY(QMetaObject::invokeMethod(navigation.tree(), "itemActivated", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, external->child(1)), Q_ARG(int, 0)));
    QCOMPARE(activated.size(), 2);
    QCOMPARE(activated.at(0).constFirst().toString(), QDir(first.path()).absolutePath());
    QCOMPARE(activated.at(1).constFirst().toString(), QDir(second.path()).absolutePath());
}

void NavigationTreeTest::exposesOnlyAppropriateLocalVolumeActions()
{
    rfm::app::NavigationTree navigation;
    auto* const mountButton =
        navigation.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    auto* const openButton = navigation.findChild<QPushButton*>(QStringLiteral("openVolumeButton"));
    auto* const unmountButton =
        navigation.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"));
    QVERIFY(mountButton != nullptr);
    QVERIFY(openButton != nullptr);
    QVERIFY(unmountButton != nullptr);

    rfm::core::StorageVolume available;
    available.displayName = QStringLiteral("USB available");
    available.device = QStringLiteral("/dev/sde1");
    available.fileSystemType = QByteArrayLiteral("vfat");
    available.kind = rfm::core::StorageKind::External;
    available.mounted = false;
    navigation.setStorageVolumes({available});
    QTreeWidgetItem* const external =
        childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices"));
    navigation.tree()->setCurrentItem(external->child(0));
    QVERIFY(mountButton->isVisibleTo(&navigation));
    QVERIFY(mountButton->isEnabled());
    QVERIFY(!openButton->isVisibleTo(&navigation));
    QVERIFY(!unmountButton->isVisibleTo(&navigation));

    QTemporaryDir mountedPath;
    QVERIFY(mountedPath.isValid());
    rfm::core::StorageVolume mounted = available;
    mounted.rootPath = mountedPath.path();
    mounted.mounted = true;
    navigation.setStorageVolumes({mounted});
    QTreeWidgetItem* mountedItem =
        childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices"))
            ->child(0);
    navigation.tree()->setCurrentItem(mountedItem);
    QVERIFY(!mountButton->isVisibleTo(&navigation));
    QVERIFY(openButton->isVisibleTo(&navigation));
    QVERIFY(unmountButton->isVisibleTo(&navigation));
    QSignalSpy unmounts(&navigation, &rfm::app::NavigationTree::localVolumeUnmountRequested);
    unmountButton->click();
    QCOMPARE(unmounts.size(), 1);

    mounted.rootPath = QStringLiteral("/");
    mounted.kind = rfm::core::StorageKind::System;
    navigation.setStorageVolumes({mounted});
    mountedItem =
        childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("Volumes"))->child(0);
    navigation.tree()->setCurrentItem(mountedItem);
    QVERIFY(openButton->isVisibleTo(&navigation));
    QVERIFY(!unmountButton->isVisibleTo(&navigation));
}

void NavigationTreeTest::showsPerVolumeBusyStates()
{
    QTemporaryDir mountedPath;
    QVERIFY(mountedPath.isValid());
    rfm::app::NavigationTree navigation;
    auto* const mountButton =
        navigation.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    auto* const unmountButton =
        navigation.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"));
    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("USB");
    volume.device = QStringLiteral("/dev/sde1");
    volume.fileSystemType = QByteArrayLiteral("vfat");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = false;
    navigation.setStorageVolumes({volume});
    QTreeWidgetItem* external =
        childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices"));
    navigation.tree()->setCurrentItem(external->child(0));
    navigation.setLocalVolumeOperation(volume.device, rfm::core::VolumeOperation::Mount);
    QVERIFY(external->child(0)->text(0).contains(QStringLiteral("Mounting")));
    QVERIFY(!mountButton->isEnabled());

    navigation.setLocalVolumeOperation(volume.device, std::nullopt);
    volume.rootPath = mountedPath.path();
    volume.mounted = true;
    navigation.setStorageVolumes({volume});
    external = childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices"));
    navigation.tree()->setCurrentItem(external->child(0));
    navigation.setLocalVolumeOperation(volume.device, rfm::core::VolumeOperation::Unmount);
    QVERIFY(external->child(0)->text(0).contains(QStringLiteral("Unmounting")));
    QVERIFY(!unmountButton->isEnabled());
}

void NavigationTreeTest::opensVolumeOnlyFromRefreshedMountPoint()
{
    QTemporaryDir actualMountPoint;
    QVERIFY(actualMountPoint.isValid());
    rfm::app::NavigationTree navigation;
    auto* const mountButton =
        navigation.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    auto* const openButton = navigation.findChild<QPushButton*>(QStringLiteral("openVolumeButton"));
    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("USB");
    volume.device = QStringLiteral("/dev/sde1");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = false;
    navigation.setStorageVolumes({volume});
    QTreeWidgetItem* external =
        childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices"));
    navigation.tree()->setCurrentItem(external->child(0));

    QSignalSpy opened(&navigation, &rfm::app::NavigationTree::localLocationActivated);
    QSignalSpy mounts(&navigation, &rfm::app::NavigationTree::localVolumeMountRequested);
    mountButton->click();
    QCOMPARE(mounts.size(), 1);
    QCOMPARE(opened.size(), 0);
    navigation.setLocalVolumeOperation(volume.device, rfm::core::VolumeOperation::Mount);
    QVERIFY(!mountButton->isEnabled());

    volume.rootPath = actualMountPoint.path();
    volume.mounted = true;
    navigation.setLocalVolumeOperation(volume.device, std::nullopt);
    navigation.setStorageVolumes({volume});
    external = childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices"));
    navigation.tree()->setCurrentItem(external->child(0));
    openButton->click();
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.constFirst().constFirst().toString(), actualMountPoint.path());
}

void NavigationTreeTest::toleratesVolumeDisappearingDuringOperation()
{
    rfm::app::NavigationTree navigation;
    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("Transient USB");
    volume.device = QStringLiteral("/dev/sde1");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = false;
    navigation.setStorageVolumes({volume});
    QTreeWidgetItem* const external =
        childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices"));
    navigation.tree()->setCurrentItem(external->child(0));
    navigation.setLocalVolumeOperation(volume.device, rfm::core::VolumeOperation::Mount);

    navigation.setStorageVolumes({});
    navigation.setLocalVolumeOperation(volume.device, std::nullopt);

    QVERIFY(childNamed(navigation.tree()->topLevelItem(0), QStringLiteral("External devices")) ==
            nullptr);
    QVERIFY(!navigation.selectedLocalStorageVolume().has_value());
}

QTEST_MAIN(NavigationTreeTest)
#include "test_navigation_tree.moc"
