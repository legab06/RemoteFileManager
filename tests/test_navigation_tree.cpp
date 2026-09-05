#include "remotefilemanager/app/NavigationTree.hpp"

#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>

#include <utility>

class NavigationTreeTest final : public QObject
{
    Q_OBJECT

  private slots:
    void buildsMachinesProfilesAndMachineScopedVolumes();
    void organizesLocalPlacesUnderHome();
    void classifiesLocalizedMoviesPathWithoutClassifyingVideosName();
    void showsRealRemoteHomeChildrenAndFiltersHidden();
    void loadsLocalChildrenOnlyWhenExpanded();
    void refreshesDirectoryChildrenWithoutLosingValidTreeState();
    void exposesOnlyTheActiveServerFileTree();
    void omitsEmptyExternalDevicesCategory();
    void deduplicatesAndNavigatesExternalDevice();
    void exposesContextActionsForItemUnderCursor();
    void deduplicatesLocalBlockAliasesByDeviceNumber();
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

QTreeWidgetItem* categoryNamed(QTreeWidget* tree, const QString& name)
{
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        QTreeWidgetItem* const item = tree->topLevelItem(index);
        if (item->text(0) == name) {
            return item;
        }
    }
    return nullptr;
}

QTreeWidgetItem* localMachineItem(QTreeWidget* tree)
{
    return childNamed(categoryNamed(tree, QStringLiteral("Local")), QStringLiteral("This Computer"));
}

QTreeWidgetItem* remoteCategoryItem(QTreeWidget* tree)
{
    return categoryNamed(tree, QStringLiteral("Distant"));
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

QList<rfm::app::NavigationTree::ContextAction>
contextActionsFor(rfm::app::NavigationTree& navigation, QTreeWidgetItem* item)
{
    navigation.tree()->scrollToItem(item);
    QApplication::processEvents();
    return navigation.contextActionsAt(navigation.tree()->visualItemRect(item).center());
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
    QTreeWidgetItem* const localCategory = categoryNamed(tree, QStringLiteral("Local"));
    QTreeWidgetItem* const remoteCategory = remoteCategoryItem(tree);
    QTreeWidgetItem* const localMachine = localMachineItem(tree);
    QVERIFY(localCategory != nullptr);
    QVERIFY(remoteCategory != nullptr);
    QVERIFY(localMachine != nullptr);
    QCOMPARE(tree->topLevelItem(0), localCategory);
    QCOMPARE(tree->topLevelItem(1), remoteCategory);
    QCOMPARE(localMachine->parent(), localCategory);
    QCOMPARE(tree->columnCount(), 2);
    QCOMPARE(tree->header()->sectionResizeMode(0), QHeaderView::Stretch);
    QCOMPARE(tree->header()->sectionResizeMode(1), QHeaderView::ResizeToContents);
    QVERIFY(tree->itemWidget(localCategory, 1) == nullptr);
    auto* const addServer =
        qobject_cast<QToolButton*>(tree->itemWidget(remoteCategory, 1));
    QVERIFY(addServer != nullptr);
    QCOMPARE(addServer->toolTip(), QStringLiteral("New connection"));
    QCOMPARE(addServer->accessibleName(), QStringLiteral("New connection"));
    QSignalSpy newConnections(&navigation, &rfm::app::NavigationTree::newConnectionRequested);
    addServer->click();
    QCOMPARE(newConnections.size(), 1);
    QVERIFY(!(localCategory->flags() & Qt::ItemIsSelectable));
    QVERIFY(!(remoteCategory->flags() & Qt::ItemIsSelectable));
    QVERIFY(!tree->rootIsDecorated());
    QVERIFY(localCategory->isExpanded());
    QVERIFY(remoteCategory->isExpanded());
    QVERIFY(localMachine->childIndicatorPolicy() != QTreeWidgetItem::DontShowIndicator);
    QTreeWidgetItem* const home = childNamed(localMachine, QStringLiteral("Home"));
    QVERIFY(home != nullptr);
    QVERIFY(home->childIndicatorPolicy() != QTreeWidgetItem::DontShowIndicator);
    QSignalSpy localActivations(&navigation, &rfm::app::NavigationTree::localLocationActivated);
    QSignalSpy remoteActivations(&navigation, &rfm::app::NavigationTree::remoteLocationActivated);
    tree->setCurrentItem(localCategory);
    QVERIFY(navigation.selectedProfileId().isEmpty());
    navigation.activateSelectedItem();
    QCOMPARE(localActivations.size(), 0);
    QCOMPARE(remoteActivations.size(), 0);
    tree->setCurrentItem(remoteCategory);
    QVERIFY(navigation.selectedProfileId().isEmpty());
    navigation.activateSelectedItem();
    QCOMPARE(localActivations.size(), 0);
    QCOMPARE(remoteActivations.size(), 0);

    navigation.setStorageVolumes(
        {storageVolume(QStringLiteral("Fixture volume"), temporary.path(),
                       rfm::core::StorageKind::Internal),
         storageVolume(QStringLiteral("USB fixture"), externalTemporary.path(),
                       rfm::core::StorageKind::External, true),
         storageVolume(QStringLiteral("Unknown fixture"), unknownTemporary.path(),
                       rfm::core::StorageKind::Unknown)});
    QTreeWidgetItem* const volumes = childNamed(localMachine, QStringLiteral("Volumes"));
    QTreeWidgetItem* const externalDevices =
        childNamed(localMachine, QStringLiteral("External devices"));
    QVERIFY(volumes != nullptr);
    QVERIFY(externalDevices != nullptr);
    QCOMPARE(volumes->childCount(), 2);
    QSet<QString> regularNames;
    for (int index = 0; index < volumes->childCount(); ++index) {
        regularNames.insert(volumes->child(index)->text(0));
    }
    QCOMPARE(regularNames, QSet<QString>({temporary.path(), unknownTemporary.path()}));
    QVERIFY(volumes->parent() == localMachine);
    QCOMPARE(externalDevices->childCount(), 1);
    QCOMPARE(externalDevices->child(0)->text(0), externalTemporary.path());
    QVERIFY(externalDevices->parent() == localMachine);

    navigation.setProfiles({{QStringLiteral("NAS"), QStringLiteral("nas.test"),
                             QStringLiteral("alice"), 22, QStringLiteral("nas-id")}});
    QCOMPARE(remoteCategory->childCount(), 1);
    QCOMPARE(remoteCategory->child(0)->text(0), QStringLiteral("NAS"));
    QTreeWidgetItem* const server = remoteCategory->child(0);
    auto* const editServer = qobject_cast<QToolButton*>(tree->itemWidget(server, 1));
    QVERIFY(editServer != nullptr);
    QCOMPARE(editServer->toolTip(), QStringLiteral("Edit server"));
    QCOMPARE(editServer->accessibleName(), QStringLiteral("Edit server"));
    QCOMPARE(editServer->property("profileId").toString(), QStringLiteral("nas-id"));
    QSignalSpy edits(&navigation, &rfm::app::NavigationTree::editProfileRequested);
    editServer->click();
    QCOMPARE(edits.size(), 1);
    QCOMPARE(edits.constFirst().constFirst().toString(), QStringLiteral("nas-id"));
    QVERIFY(server->childIndicatorPolicy() != QTreeWidgetItem::DontShowIndicator);

    navigation.resize(240, 500);
    navigation.show();
    QApplication::processEvents();
    QVERIFY(addServer->isVisibleTo(tree));
    QVERIFY(editServer->isVisibleTo(tree));
    QVERIFY(tree->columnWidth(1) >= editServer->sizeHint().width());
    tree->setCurrentItem(localMachine);
    QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                      Qt::NoModifier, tree->visualItemRect(localCategory).center());
    QCOMPARE(tree->currentItem(), localMachine);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton,
                       Qt::NoModifier, tree->visualItemRect(localCategory).center());
    QVERIFY(localCategory->isExpanded());
    QVERIFY(!tree->visualItemRect(localMachine).isEmpty());
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton,
                       Qt::NoModifier, tree->visualItemRect(remoteCategory).center());
    QVERIFY(remoteCategory->isExpanded());
    QVERIFY(!tree->visualItemRect(server).isEmpty());

    localCategory->setExpanded(false);
    remoteCategory->setExpanded(false);
    QVERIFY(localCategory->isExpanded());
    QVERIFY(remoteCategory->isExpanded());
}

void NavigationTreeTest::organizesLocalPlacesUnderHome()
{
    rfm::app::NavigationTree navigation;
    QTreeWidgetItem* const machine = localMachineItem(navigation.tree());
    QTreeWidgetItem* const home = childNamed(machine, QStringLiteral("Home"));
    QTreeWidgetItem* const volumes = childNamed(machine, QStringLiteral("Volumes"));
    QVERIFY(home != nullptr);
    QVERIFY(volumes != nullptr);
    QVERIFY(!home->icon(0).isNull());
    QVERIFY(!volumes->icon(0).isNull());
    QVERIFY(childNamed(machine, QStringLiteral("Documents")) == nullptr);
    QVERIFY(childNamed(machine, QStringLiteral("Downloads")) == nullptr);

    const QList<QStandardPaths::StandardLocation> locations{
        QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation,
        QStandardPaths::DownloadLocation, QStandardPaths::MusicLocation,
        QStandardPaths::PicturesLocation, QStandardPaths::MoviesLocation};
    for (const auto location : locations) {
        const QString path = QStandardPaths::writableLocation(location);
        if (!path.isEmpty() && QFileInfo(path).isDir() && QDir::cleanPath(path) != QDir::homePath()) {
            bool found = false;
            for (int index = 0; index < home->childCount(); ++index) {
                if (home->child(index)->data(0, Qt::UserRole + 1).toString() ==
                    QDir::cleanPath(path)) {
                    QVERIFY(!home->child(index)->icon(0).isNull());
                    found = true;
                }
            }
            QVERIFY(found);
        }
    }
}

void NavigationTreeTest::classifiesLocalizedMoviesPathWithoutClassifyingVideosName()
{
    const QString homePath = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    const QString moviesPath = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QFileInfo homeInfo(homePath);
    const QFileInfo moviesInfo(moviesPath);
    if (homePath.isEmpty() || !homeInfo.isDir() || moviesPath.isEmpty() || !moviesInfo.isDir() ||
        moviesInfo.dir().canonicalPath() != homeInfo.canonicalFilePath() ||
        moviesInfo.fileName().toCaseFolded() == QStringLiteral("videos")) {
        QSKIP("MoviesLocation is not a localized direct child of Home on this platform");
    }

    rfm::app::NavigationTree navigation;
    navigation.setLocalDirectory(
        homePath, {{moviesInfo.fileName(), 0, {}, true, false, false},
                   {QStringLiteral("videos"), 0, {}, true, false, false}});
    QTreeWidgetItem* const home =
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("Home"));
    QVERIFY(home != nullptr);
    QTreeWidgetItem* const movies = childNamed(home, moviesInfo.fileName());
    QTreeWidgetItem* const genericVideos = childNamed(home, QStringLiteral("videos"));
    QVERIFY(movies != nullptr);
    QVERIFY(genericVideos != nullptr);

    QFileIconProvider provider;
    const QIcon folder = provider.icon(QFileIconProvider::Folder);
    const QImage expectedMovies = QIcon::fromTheme(QStringLiteral("folder-videos"), folder)
                                      .pixmap(16, 16)
                                      .toImage();
    const QImage expectedFolder = QIcon::fromTheme(QStringLiteral("folder"), folder)
                                      .pixmap(16, 16)
                                      .toImage();
    QCOMPARE(movies->icon(0).pixmap(16, 16).toImage(), expectedMovies);
    QCOMPARE(genericVideos->icon(0).pixmap(16, 16).toImage(), expectedFolder);
}

void NavigationTreeTest::showsRealRemoteHomeChildrenAndFiltersHidden()
{
    rfm::app::NavigationTree navigation;
    navigation.setActiveServer(remoteMachine(QStringLiteral("remote-id")),
                               QStringLiteral("/home/alice"));
    QTreeWidgetItem* const server = remoteCategoryItem(navigation.tree())->child(0);
    QTreeWidgetItem* const home = childNamed(server, QStringLiteral("Home"));
    QVERIFY(home != nullptr);
    QVERIFY(childNamed(server, QStringLiteral("/")) != nullptr);
    QVERIFY(childNamed(server, QStringLiteral("Volumes")) != nullptr);

    navigation.setRemoteDirectory(
        QStringLiteral("remote-id"), QStringLiteral("/home/alice"),
        {{QStringLiteral("Documents"), 0, {}, true, false, false},
         {QStringLiteral(".ssh"), 0, {}, true, false, true},
         {QStringLiteral("ordinary"), 0, {}, true, false, false}});
    QCOMPARE(home->childCount(), 3);
    QVERIFY(!childNamed(home, QStringLiteral("Documents"))->icon(0).isNull());
    QVERIFY(childNamed(home, QStringLiteral(".ssh"))->isHidden());
    navigation.setShowHiddenFiles(true);
    QVERIFY(!childNamed(home, QStringLiteral(".ssh"))->isHidden());
    navigation.setShowHiddenFiles(false);
    QVERIFY(childNamed(home, QStringLiteral(".ssh"))->isHidden());
}

void NavigationTreeTest::loadsLocalChildrenOnlyWhenExpanded()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::NavigationTree navigation;
    navigation.setStorageVolumes({storageVolume(QStringLiteral("Fixture"), temporary.path(),
                                                rfm::core::StorageKind::Internal)});
    QTreeWidgetItem* const volumes =
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("Volumes"));
    QVERIFY(volumes != nullptr);
    QTreeWidgetItem* const fixture = volumes->child(0);
    QVERIFY(!navigation.hasLoadedLocalDirectory(temporary.path()));
    QVERIFY(!navigation.hasLoadedLocalDirectory(
        QDir(temporary.path()).filePath(QStringLiteral("not-represented"))));
    QCOMPARE(fixture->childCount(), 1);
    QCOMPARE(fixture->child(0)->text(0), QStringLiteral("Expand to load"));

    QSignalSpy expansion(&navigation, &rfm::app::NavigationTree::localDirectoryExpansionRequested);
    fixture->setExpanded(true);
    QCOMPARE(expansion.size(), 1);
    QVERIFY(navigation.hasLoadedLocalDirectory(temporary.path()));
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

void NavigationTreeTest::refreshesDirectoryChildrenWithoutLosingValidTreeState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::NavigationTree navigation;
    navigation.setStorageVolumes({storageVolume(QStringLiteral("Fixture"), temporary.path(),
                                                rfm::core::StorageKind::Internal)});
    QTreeWidgetItem* const volumes =
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("Volumes"));
    QVERIFY(volumes != nullptr);
    QTreeWidgetItem* const fixture = volumes->child(0);
    fixture->setExpanded(true);
    navigation.setLocalDirectory(temporary.path(),
                                 {{QStringLiteral("kept"), 0, {}, true, false},
                                  {QStringLiteral("removed"), 0, {}, true, false}});

    QTreeWidgetItem* const kept = childNamed(fixture, QStringLiteral("kept"));
    QVERIFY(kept != nullptr);
    kept->setExpanded(true);
    navigation.setLocalDirectory(QDir(temporary.path()).filePath(QStringLiteral("kept")),
                                 {{QStringLiteral("nested"), 0, {}, true, false}});
    QTreeWidgetItem* const nested = childNamed(kept, QStringLiteral("nested"));
    QVERIFY(nested != nullptr);
    navigation.tree()->setCurrentItem(nested);

    navigation.setLocalDirectory(temporary.path(), {{QStringLiteral("added"), 0, {}, true, false},
                                                    {QStringLiteral("kept"), 0, {}, true, false}});

    QCOMPARE(childNamed(fixture, QStringLiteral("kept")), kept);
    QCOMPARE(childNamed(kept, QStringLiteral("nested")), nested);
    QVERIFY(childNamed(fixture, QStringLiteral("added")) != nullptr);
    QVERIFY(childNamed(fixture, QStringLiteral("removed")) == nullptr);
    QVERIFY(fixture->isExpanded());
    QVERIFY(kept->isExpanded());
    QCOMPARE(navigation.tree()->currentItem(), nested);
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
    QTreeWidgetItem* const servers = remoteCategoryItem(navigation.tree());
    QCOMPARE(servers->childCount(), 2);
    QVERIFY(servers->child(0)->text(0).contains(QStringLiteral("Connected")));
    QVERIFY(servers->child(0)->childCount() >= 1);
    QVERIFY(qobject_cast<QToolButton*>(navigation.tree()->itemWidget(servers->child(0), 1)) !=
            nullptr);
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
    QVERIFY(childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices")) !=
            nullptr);
    navigation.setStorageVolumes({storageVolume(QStringLiteral("Internal"), temporary.path(),
                                                rfm::core::StorageKind::Internal)});
    QVERIFY(childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices")) ==
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
    QTreeWidgetItem* const machine = localMachineItem(navigation.tree());
    QTreeWidgetItem* const volumes = childNamed(machine, QStringLiteral("Volumes"));
    QTreeWidgetItem* const externalDevices =
        childNamed(machine, QStringLiteral("External devices"));
    QVERIFY(volumes != nullptr);
    QVERIFY(externalDevices != nullptr);
    QCOMPARE(volumes->childCount(), 0);
    QCOMPARE(externalDevices->childCount(), 1);
    QCOMPARE(externalDevices->child(0)->text(0), temporary.path());

    QSignalSpy activated(&navigation, &rfm::app::NavigationTree::localLocationActivated);
    QVERIFY(QMetaObject::invokeMethod(navigation.tree(), "itemActivated", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, externalDevices->child(0)),
                                      Q_ARG(int, 0)));
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.constFirst().constFirst().toString(), QDir(temporary.path()).absolutePath());
}

void NavigationTreeTest::exposesContextActionsForItemUnderCursor()
{
    using ContextAction = rfm::app::NavigationTree::ContextAction;
    QTemporaryDir mountedPath;
    QVERIFY(mountedPath.isValid());
    rfm::app::NavigationTree navigation;
    navigation.resize(640, 700);
    navigation.setProfiles({{QStringLiteral("Offline"), QStringLiteral("offline.test"),
                             QStringLiteral("alice"), 22, QStringLiteral("offline-id")},
                            {QStringLiteral("Online"), QStringLiteral("online.test"),
                             QStringLiteral("bob"), 22, QStringLiteral("online-id")}});
    navigation.setActiveServer({QStringLiteral("online-id"), QStringLiteral("Online"),
                                QStringLiteral("online.test"), QStringLiteral("bob"), 22,
                                QStringLiteral("online-id")},
                               QStringLiteral("/home/bob"));

    rfm::core::StorageVolume mounted;
    mounted.displayName = QStringLiteral("Mounted");
    mounted.rootPath = mountedPath.path();
    mounted.device = QStringLiteral("/dev/sdz1");
    mounted.deviceNumber = QStringLiteral("65:1");
    mounted.kind = rfm::core::StorageKind::External;
    mounted.mounted = true;
    rfm::core::StorageVolume available = mounted;
    available.displayName = QStringLiteral("Available");
    available.rootPath.clear();
    available.device = QStringLiteral("/dev/sdz2");
    available.deviceNumber = QStringLiteral("65:2");
    available.mounted = false;
    navigation.setStorageVolumes({mounted, available});
    navigation.tree()->expandAll();
    navigation.show();
    QApplication::processEvents();

    QTreeWidgetItem* const machine = localMachineItem(navigation.tree());
    QTreeWidgetItem* const servers = remoteCategoryItem(navigation.tree());
    QTreeWidgetItem* const offline = servers->child(0);
    QTreeWidgetItem* const online = servers->child(1);
    QTreeWidgetItem* const localFolder = machine->child(0);
    QTreeWidgetItem* const remoteFolder = childNamed(online, QStringLiteral("Home"));
    QTreeWidgetItem* const mountedItem = volumeItemByDevice(machine, mounted.device);
    QTreeWidgetItem* const availableItem = volumeItemByDevice(machine, available.device);
    QVERIFY(remoteFolder != nullptr);
    QVERIFY(mountedItem != nullptr);
    QVERIFY(availableItem != nullptr);

    navigation.tree()->setCurrentItem(online);
    QCOMPARE(contextActionsFor(navigation, offline),
             QList<ContextAction>({ContextAction::Connect, ContextAction::Properties,
                                   ContextAction::RemoveServer}));
    QCOMPARE(contextActionsFor(navigation, online),
             QList<ContextAction>({ContextAction::Disconnect, ContextAction::Properties,
                                   ContextAction::RemoveServer}));
    QCOMPARE(contextActionsFor(navigation, localFolder),
             QList<ContextAction>({ContextAction::Open, ContextAction::Properties}));
    QCOMPARE(contextActionsFor(navigation, remoteFolder),
             QList<ContextAction>({ContextAction::Open, ContextAction::Properties}));
    QCOMPARE(contextActionsFor(navigation, mountedItem),
             QList<ContextAction>(
                 {ContextAction::Open, ContextAction::Unmount, ContextAction::Properties}));
    QCOMPARE(contextActionsFor(navigation, availableItem),
             QList<ContextAction>({ContextAction::Mount, ContextAction::Properties}));

    QCOMPARE(contextActionsFor(navigation, categoryNamed(navigation.tree(), QStringLiteral("Local"))),
             QList<ContextAction>{});
    QCOMPARE(contextActionsFor(navigation, machine), QList<ContextAction>{});
    QCOMPARE(contextActionsFor(navigation, servers), QList<ContextAction>{});
    QCOMPARE(contextActionsFor(navigation, childNamed(machine, QStringLiteral("Volumes"))),
             QList<ContextAction>{});
    QVERIFY(navigation.tree()->itemWidget(remoteFolder, 1) == nullptr);
    QCOMPARE(navigation.tree()->currentItem(), online);
}

void NavigationTreeTest::deduplicatesLocalBlockAliasesByDeviceNumber()
{
    QTemporaryDir mountedPath;
    QVERIFY(mountedPath.isValid());
    rfm::app::NavigationTree navigation;
    rfm::core::StorageVolume mounted = storageVolume(QStringLiteral("Archive"), mountedPath.path(),
                                                     rfm::core::StorageKind::Internal);
    mounted.device = QStringLiteral("/dev/mapper/archive");
    mounted.deviceNumber = QStringLiteral("253:3");
    mounted.mounted = true;
    rfm::core::StorageVolume alias = mounted;
    alias.rootPath.clear();
    alias.device = QStringLiteral("/dev/dm-3");
    alias.mounted = false;
    rfm::core::StorageVolume distinct = alias;
    distinct.displayName = QStringLiteral("Distinct");
    distinct.device = QStringLiteral("/dev/dm-4");
    distinct.deviceNumber = QStringLiteral("253:4");

    navigation.setStorageVolumes({mounted, alias, distinct});

    QTreeWidgetItem* const machine = localMachineItem(navigation.tree());
    QCOMPARE(volumeItemsByDevice(machine, mounted.device).size(), 1);
    QCOMPARE(volumeItemsByDevice(machine, alias.device).size(), 0);
    QCOMPARE(volumeItemsByDevice(machine, distinct.device).size(), 1);
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

    QTreeWidgetItem* const localMachine = localMachineItem(navigation.tree());
    QTreeWidgetItem* const remoteMachine = remoteCategoryItem(navigation.tree())->child(0);
    QCOMPARE(childNamed(localMachine, QStringLiteral("Volumes"))->childCount(), 0);
    QCOMPARE(childNamed(remoteMachine, QStringLiteral("Volumes"))->childCount(), 1);
    QCOMPARE(childNamed(remoteMachine, QStringLiteral("External devices"))->childCount(), 1);

    navigation.setRemoteStorageVolumes(
        QStringLiteral("remote-id"),
        {storageVolume(QStringLiteral("Replacement"), QStringLiteral("/data"),
                       rfm::core::StorageKind::Internal)});
    QTreeWidgetItem* const refreshedRemote = remoteCategoryItem(navigation.tree())->child(0);
    QTreeWidgetItem* const volumes = childNamed(refreshedRemote, QStringLiteral("Volumes"));
    QVERIFY(volumes != nullptr);
    QCOMPARE(volumes->childCount(), 1);
    QCOMPARE(volumes->child(0)->text(0), QStringLiteral("/data"));
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
    QTreeWidgetItem* const server = remoteCategoryItem(navigation.tree())->child(0);
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
    QTreeWidgetItem* const server = remoteCategoryItem(navigation.tree())->child(0);
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

    QTreeWidgetItem* const machine = localMachineItem(navigation.tree());
    QTreeWidgetItem* externalDevices = childNamed(machine, QStringLiteral("External devices"));
    QVERIFY(externalDevices != nullptr);
    QCOMPARE(externalDevices->childCount(), 1);
    QTreeWidgetItem* entry = externalDevices->child(0);
    QCOMPARE(entry->text(0), mountPoint.path());
    QTextDocument fullDocument;
    fullDocument.setHtml(entry->toolTip(0));
    const QString fullToolTip = fullDocument.toPlainText();
    QVERIFY(fullToolTip.startsWith(mountPoint.path() + QChar{'\n'}));
    QVERIFY(fullToolTip.contains(QStringLiteral("Label: PHOTOS")));
    QVERIFY(fullToolTip.contains(QStringLiteral("Device: /dev/sdb1")));
    QVERIFY(fullToolTip.contains(QStringLiteral("Mount point: %1").arg(mountPoint.path())));
    QVERIFY(fullToolTip.contains(QStringLiteral("Filesystem: exfat")));
    QVERIFY(fullToolTip.contains(QStringLiteral("Model: SanDisk Cruzer Glide")));
    QVERIFY(fullToolTip.contains(QStringLiteral("Size:")));
    QVERIFY(fullToolTip.split(QChar{'\n'}).size() <= 8);

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
    QCOMPARE(entry->text(0), mountPoint.path());
    QTextDocument partialDocument;
    partialDocument.setHtml(entry->toolTip(0));
    const QString partialToolTip = partialDocument.toPlainText();
    QVERIFY(partialToolTip.startsWith(mountPoint.path() + QChar{'\n'}));
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
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("Volumes"));
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
    QTreeWidgetItem* const server = remoteCategoryItem(navigation.tree())->child(0);
    QTreeWidgetItem* const home = childNamed(server, QStringLiteral("Home"));
    home->setExpanded(true);
    navigation.setRemoteDirectory(QStringLiteral("remote-id"), QStringLiteral("/home/alice"),
                                  {{QStringLiteral("projects"), 0, {}, true, false}});
    QTreeWidgetItem* const remoteChild = home->child(0);
    navigation.tree()->setCurrentItem(remoteChild);
    navigation.setRemoteStorageVolumes(
        QStringLiteral("remote-id"), {storageVolume(QStringLiteral("Data"), QStringLiteral("/data"),
                                                    rfm::core::StorageKind::Internal)});
    QCOMPARE(remoteCategoryItem(navigation.tree())->child(0), server);
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
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices"));
    QCOMPARE(external->childCount(), 2);
    QCOMPARE(external->child(0)->text(0), first.path());
    QCOMPARE(external->child(1)->text(0), second.path());
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
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices"));
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
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices"))
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
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("Volumes"))->child(0);
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
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices"));
    navigation.tree()->setCurrentItem(external->child(0));
    navigation.setLocalVolumeOperation(volume.device, rfm::core::VolumeOperation::Mount);
    QVERIFY(external->child(0)->text(0).contains(QStringLiteral("Mounting")));
    QVERIFY(!mountButton->isEnabled());

    navigation.setLocalVolumeOperation(volume.device, std::nullopt);
    volume.rootPath = mountedPath.path();
    volume.mounted = true;
    navigation.setStorageVolumes({volume});
    external = childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices"));
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
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices"));
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
    external = childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices"));
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
        childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices"));
    navigation.tree()->setCurrentItem(external->child(0));
    navigation.setLocalVolumeOperation(volume.device, rfm::core::VolumeOperation::Mount);

    navigation.setStorageVolumes({});
    navigation.setLocalVolumeOperation(volume.device, std::nullopt);

    QVERIFY(childNamed(localMachineItem(navigation.tree()), QStringLiteral("External devices")) ==
            nullptr);
    QVERIFY(!navigation.selectedLocalStorageVolume().has_value());
}

QTEST_MAIN(NavigationTreeTest)
#include "test_navigation_tree.moc"
