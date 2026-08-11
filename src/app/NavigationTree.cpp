#include "remotefilemanager/app/NavigationTree.hpp"

#include "remotefilemanager/core/BrowserLocation.hpp"
#include "remotefilemanager/core/RemotePath.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFont>
#include <QHash>
#include <QHeaderView>
#include <QLocale>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QTextDocument>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <utility>

namespace rfm::app
{
namespace
{

QTreeWidgetItem* createItem(QTreeWidgetItem* parent, const QString& text,
                            NavigationTree::NodeKind kind, const QIcon& icon = {})
{
    auto* const item = new QTreeWidgetItem(parent, {text});
    item->setData(0, Qt::UserRole, static_cast<int>(kind));
    if (!icon.isNull()) {
        item->setIcon(0, icon);
    }
    return item;
}

QString safePresentationText(const QString& value)
{
    QString safe = value;
    for (qsizetype index = 0; index < safe.size(); ++index) {
        const QChar character = safe.at(index);
        const QChar::Category category = character.category();
        if (category == QChar::Other_Control || category == QChar::Separator_Line ||
            category == QChar::Separator_Paragraph) {
            safe[index] = QChar{' '};
        }
    }
    return safe.simplified();
}

QString availableMetadata(const QString& value)
{
    const QString trimmed = safePresentationText(value);
    return trimmed.compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) == 0 ? QString{}
                                                                                : trimmed;
}

QString storageToolTip(const rfm::core::StorageVolume& volume)
{
    QStringList lines;
    auto append = [&lines](const QString& label, const QString& value) {
        const QString available = availableMetadata(value);
        if (!available.isEmpty()) {
            lines.push_back(label.isEmpty() ? available
                                            : QStringLiteral("%1: %2").arg(label, available));
        }
    };
    append({}, volume.displayName);
    append(QCoreApplication::translate("NavigationTree", "Device"), volume.device);
    append(QCoreApplication::translate("NavigationTree", "Mount point"), volume.rootPath);
    append(QCoreApplication::translate("NavigationTree", "Filesystem"),
           QString::fromLatin1(volume.fileSystemType).trimmed());
    append(QCoreApplication::translate("NavigationTree", "Model"), volume.deviceModel);
    if (volume.bytesTotal > 0 &&
        volume.bytesTotal <= static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        append(QCoreApplication::translate("NavigationTree", "Size"),
               QLocale{}.formattedDataSize(static_cast<qint64>(volume.bytesTotal), 1,
                                           QLocale::DataSizeTraditionalFormat));
    }
    return Qt::convertFromPlainText(lines.join(QChar{'\n'}));
}

QString shortStorageIdentity(const rfm::core::StorageVolume& volume, bool local)
{
    QString identity = rfm::core::RemotePath::fileName(volume.device.trimmed());
    if (identity.isEmpty()) {
        identity = local ? QFileInfo(volume.rootPath).fileName()
                         : rfm::core::RemotePath::fileName(volume.rootPath);
    }
    if (identity.isEmpty()) {
        identity = safePresentationText(volume.rootPath);
    }
    return safePresentationText(identity);
}

QStringList storageDisplayNames(const QList<rfm::core::StorageVolume>& volumes, bool local)
{
    QStringList baseNames;
    QHash<QString, int> occurrences;
    for (const rfm::core::StorageVolume& volume : volumes) {
        QString base = safePresentationText(volume.displayName);
        if (base.isEmpty()) {
            base = safePresentationText(volume.rootPath);
        }
        baseNames.push_back(base);
        ++occurrences[base.toCaseFolded()];
    }

    QStringList names;
    QSet<QString> usedNames;
    for (qsizetype index = 0; index < volumes.size(); ++index) {
        const QString& base = baseNames.at(index);
        QString name = base;
        if (occurrences.value(base.toCaseFolded()) > 1) {
            const QString shortIdentity = shortStorageIdentity(volumes.at(index), local);
            name =
                shortIdentity.isEmpty() ? base : QStringLiteral("%1 (%2)").arg(base, shortIdentity);
            if (usedNames.contains(name.toCaseFolded())) {
                name = QStringLiteral("%1 (%2)").arg(
                    base, safePresentationText(volumes.at(index).rootPath));
            }
        }
        QString uniqueName = name;
        int suffix = 2;
        while (usedNames.contains(uniqueName.toCaseFolded())) {
            uniqueName = QStringLiteral("%1 [%2]").arg(name).arg(suffix++);
        }
        usedNames.insert(uniqueName.toCaseFolded());
        names.push_back(uniqueName);
    }
    return names;
}

} // namespace

NavigationTree::NavigationTree(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("navigationTreeWidget"));
    auto* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("navigationTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setAnimated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(true);
    layout->addWidget(m_tree);

    buildLocalMachine();
    m_serversItem = createItem(m_tree->invisibleRootItem(), tr("Servers"), NodeKind::Servers,
                               style()->standardIcon(QStyle::SP_DriveNetIcon));
    m_localMachineItem->setExpanded(true);
    m_serversItem->setExpanded(true);

    connect(m_tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item, int) { activateItem(item); });
    connect(m_tree, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem* item) { expandItem(item); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem*, QTreeWidgetItem*) { emit selectedProfileChanged(); });
}

QTreeWidget* NavigationTree::tree() const { return m_tree; }

QString NavigationTree::selectedProfileId() const
{
    QTreeWidgetItem* item = m_tree->currentItem();
    while (item != nullptr) {
        if (itemKind(item) == NodeKind::ServerProfile) {
            return item->data(0, SavedProfileIdRole).toString();
        }
        item = item->parent();
    }
    return {};
}

QString NavigationTree::profileIdAt(const QPoint& viewportPosition) const
{
    QTreeWidgetItem* item = m_tree->itemAt(viewportPosition);
    while (item != nullptr) {
        if (itemKind(item) == NodeKind::ServerProfile) {
            return item->data(0, SavedProfileIdRole).toString();
        }
        item = item->parent();
    }
    return {};
}

void NavigationTree::setProfiles(const QList<rfm::core::ConnectionProfile>& profiles)
{
    m_profiles = profiles;
    rebuildServers();
}

void NavigationTree::setActiveServer(RemoteMachineDescriptor machine, const QString& initialPath)
{
    const QString previousMachineId = m_activeMachine.id;
    if (!previousMachineId.isEmpty() && previousMachineId != machine.id &&
        m_remoteStorageProfileId == previousMachineId) {
        m_remoteStorageProfileId = machine.id;
    }
    m_activeMachine = std::move(machine);
    m_activeInitialPath = rfm::core::RemotePath::normalize(initialPath);
    rebuildServers();
}

void NavigationTree::clearActiveServer()
{
    m_activeMachine = {};
    m_activeInitialPath.clear();
    m_remoteStorageProfileId.clear();
    m_remoteStorageVolumes.clear();
    rebuildServers();
}

void NavigationTree::setStorageVolumes(const QList<rfm::core::StorageVolume>& volumes)
{
    synchronizeStorageBranches(m_localMachineItem, m_volumesItem, m_externalDevicesItem,
                               uniqueStorageVolumes(volumes, true), true,
                               QString::fromLatin1(rfm::core::LocalMachineId));
}

void NavigationTree::setRemoteStorageVolumes(const QString& profileId,
                                             const QList<rfm::core::StorageVolume>& volumes)
{
    if (profileId.isEmpty() || profileId != m_activeMachine.id) {
        return;
    }
    m_remoteStorageProfileId = profileId;
    m_remoteStorageVolumes = volumes;
    QTreeWidgetItem* const machineItem = serverItem(profileId);
    QTreeWidgetItem* const volumesItem = directChild(machineItem, NodeKind::Volumes);
    QTreeWidgetItem* externalDevicesItem = directChild(machineItem, NodeKind::ExternalDevices);
    synchronizeStorageBranches(machineItem, volumesItem, externalDevicesItem,
                               uniqueStorageVolumes(volumes, false), false, profileId);
}

QList<rfm::core::StorageVolume>
NavigationTree::uniqueStorageVolumes(const QList<rfm::core::StorageVolume>& volumes,
                                     bool local) const
{
    QList<rfm::core::StorageVolume> uniqueVolumes;
    QHash<QString, qsizetype> indexes;
    for (const rfm::core::StorageVolume& volume : volumes) {
        const QString identity = local ? normalizedLocalPath(volume.rootPath)
                                       : rfm::core::RemotePath::normalize(volume.rootPath);
        if (identity.isEmpty() || (!local && !identity.startsWith(QChar{'/'}))) {
            continue;
        }
        const auto existing = indexes.constFind(identity);
        if (existing == indexes.cend()) {
            indexes.insert(identity, uniqueVolumes.size());
            uniqueVolumes.push_back(volume);
        } else if (volume.kind == rfm::core::StorageKind::External &&
                   uniqueVolumes.at(existing.value()).kind != rfm::core::StorageKind::External) {
            uniqueVolumes[existing.value()] = volume;
        }
    }
    return uniqueVolumes;
}

void NavigationTree::synchronizeStorageBranches(QTreeWidgetItem* machineItem,
                                                QTreeWidgetItem* volumesItem,
                                                QTreeWidgetItem*& externalDevicesItem,
                                                const QList<rfm::core::StorageVolume>& volumes,
                                                bool local, const QString& machineId)
{
    if (machineItem == nullptr || volumesItem == nullptr) {
        return;
    }

    QList<rfm::core::StorageVolume> regularVolumes;
    QList<rfm::core::StorageVolume> externalVolumes;
    for (const rfm::core::StorageVolume& volume : volumes) {
        (volume.kind == rfm::core::StorageKind::External ? externalVolumes : regularVolumes)
            .push_back(volume);
    }

    if (!externalVolumes.isEmpty() && externalDevicesItem == nullptr) {
        externalDevicesItem =
            createItem(machineItem, tr("External devices"), NodeKind::ExternalDevices,
                       QIcon::fromTheme(QStringLiteral("drive-removable-media-usb"),
                                        style()->standardIcon(QStyle::SP_DriveHDIcon)));
    }

    QHash<QString, QTreeWidgetItem*> existingItems;
    QList<QTreeWidgetItem*> duplicateItems;
    auto collectExisting = [&](QTreeWidgetItem* parent) {
        if (parent == nullptr) {
            return;
        }
        for (int index = 0; index < parent->childCount(); ++index) {
            QTreeWidgetItem* const item = parent->child(index);
            const QString identity = item->data(0, PathRole).toString();
            if (identity.isEmpty()) {
                continue;
            }
            if (existingItems.contains(identity)) {
                duplicateItems.push_back(item);
            } else {
                existingItems.insert(identity, item);
            }
        }
    };
    collectExisting(volumesItem);
    collectExisting(externalDevicesItem);
    qDeleteAll(duplicateItems);

    auto synchronizeCategory = [&](QTreeWidgetItem* parent,
                                   const QList<rfm::core::StorageVolume>& categoryVolumes,
                                   bool external) {
        if (parent == nullptr) {
            return;
        }
        const QStringList displayNames = storageDisplayNames(categoryVolumes, local);
        for (qsizetype index = 0; index < categoryVolumes.size(); ++index) {
            const rfm::core::StorageVolume& volume = categoryVolumes.at(index);
            const QString path = local ? normalizedLocalPath(volume.rootPath)
                                       : rfm::core::RemotePath::normalize(volume.rootPath);
            QTreeWidgetItem* item = existingItems.take(path);
            const bool newlyCreated = item == nullptr;
            const QIcon icon =
                external ? QIcon::fromTheme(QStringLiteral("drive-removable-media"),
                                            style()->standardIcon(QStyle::SP_DriveHDIcon))
                         : (volume.kind == rfm::core::StorageKind::Network
                                ? QIcon::fromTheme(QStringLiteral("folder-remote"),
                                                   style()->standardIcon(QStyle::SP_DriveNetIcon))
                                : style()->standardIcon(QStyle::SP_DriveHDIcon));
            if (newlyCreated) {
                item =
                    createItem(parent, displayNames.at(index),
                               local ? NodeKind::LocalLocation : NodeKind::RemoteDirectory, icon);
                addLazyPlaceholder(item);
            } else {
                item->setText(0, displayNames.at(index));
                item->setData(
                    0, KindRole,
                    static_cast<int>(local ? NodeKind::LocalLocation : NodeKind::RemoteDirectory));
                item->setIcon(0, icon);
            }
            item->setData(0, PathRole, path);
            item->setData(0, ProfileIdRole, local ? QString{} : machineId);
            item->setToolTip(0, storageToolTip(volume));

            if (item->parent() != parent) {
                QTreeWidgetItem* const oldParent = item->parent();
                oldParent->takeChild(oldParent->indexOfChild(item));
                parent->insertChild(static_cast<int>(index), item);
            } else if (parent->indexOfChild(item) != static_cast<int>(index)) {
                parent->takeChild(parent->indexOfChild(item));
                parent->insertChild(static_cast<int>(index), item);
            }
        }
    };

    synchronizeCategory(volumesItem, regularVolumes, false);
    synchronizeCategory(externalDevicesItem, externalVolumes, true);
    qDeleteAll(existingItems.values());

    if (externalVolumes.isEmpty() && externalDevicesItem != nullptr) {
        delete externalDevicesItem;
        externalDevicesItem = nullptr;
    }
    volumesItem->setData(0, LoadedRole, true);
}

QTreeWidgetItem* NavigationTree::directChild(QTreeWidgetItem* parent, NodeKind kind) const
{
    if (parent == nullptr) {
        return nullptr;
    }
    for (int index = 0; index < parent->childCount(); ++index) {
        if (itemKind(parent->child(index)) == kind) {
            return parent->child(index);
        }
    }
    return nullptr;
}

QTreeWidgetItem* NavigationTree::serverItem(const QString& machineId) const
{
    if (m_serversItem == nullptr || machineId.isEmpty()) {
        return nullptr;
    }
    for (int index = 0; index < m_serversItem->childCount(); ++index) {
        QTreeWidgetItem* const item = m_serversItem->child(index);
        if (itemKind(item) == NodeKind::ServerProfile &&
            item->data(0, ProfileIdRole).toString() == machineId) {
            return item;
        }
    }
    return nullptr;
}

void NavigationTree::setLocalDirectory(const QString& path,
                                       const QList<rfm::core::RemoteEntry>& entries)
{
    const QString normalizedPath = normalizedLocalPath(path);
    QList<QTreeWidgetItem*> items = matchingItems(NodeKind::LocalLocation, normalizedPath);
    items += matchingItems(NodeKind::LocalDirectory, normalizedPath);
    for (QTreeWidgetItem* const item : std::as_const(items)) {
        replaceDirectoryChildren(item, entries, true, {});
    }
}

void NavigationTree::setRemoteDirectory(const QString& profileId, const QString& path,
                                        const QList<rfm::core::RemoteEntry>& entries)
{
    if (profileId.isEmpty() || profileId != m_activeMachine.id) {
        return;
    }
    const QString normalizedPath = rfm::core::RemotePath::normalize(path);
    for (QTreeWidgetItem* const item :
         matchingItems(NodeKind::RemoteDirectory, normalizedPath, profileId)) {
        replaceDirectoryChildren(item, entries, false, profileId);
    }
}

void NavigationTree::setDirectoryError(bool local, const QString& profileId, const QString& path,
                                       const QString& error)
{
    const QString normalizedPath =
        local ? normalizedLocalPath(path) : rfm::core::RemotePath::normalize(path);
    QList<QTreeWidgetItem*> items = matchingItems(
        local ? NodeKind::LocalDirectory : NodeKind::RemoteDirectory, normalizedPath, profileId);
    if (local) {
        items += matchingItems(NodeKind::LocalLocation, normalizedPath);
    }
    for (QTreeWidgetItem* const item : std::as_const(items)) {
        qDeleteAll(item->takeChildren());
        auto* const errorItem = createItem(item, tr("Unavailable"), NodeKind::Placeholder,
                                           style()->standardIcon(QStyle::SP_MessageBoxWarning));
        errorItem->setToolTip(0, error);
        errorItem->setDisabled(true);
        item->setData(0, LoadedRole, false);
    }
}

void NavigationTree::buildLocalMachine()
{
    m_localMachineItem =
        createItem(m_tree->invisibleRootItem(), tr("This Computer"), NodeKind::LocalMachine,
                   style()->standardIcon(QStyle::SP_ComputerIcon));
    QFileIconProvider icons;
    auto addLocation = [this, &icons](const QString& name, const QString& path) {
        if (path.isEmpty() || !QFileInfo(path).isDir()) {
            return;
        }
        auto* const item = createItem(m_localMachineItem, name, NodeKind::LocalLocation,
                                      icons.icon(QFileIconProvider::Folder));
        item->setData(0, PathRole, normalizedLocalPath(path));
        item->setToolTip(0, path);
        addLazyPlaceholder(item);
    };
    addLocation(tr("Home"), QDir::homePath());
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (normalizedLocalPath(documents) != normalizedLocalPath(QDir::homePath())) {
        addLocation(tr("Documents"), documents);
    }
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (normalizedLocalPath(downloads) != normalizedLocalPath(QDir::homePath()) &&
        normalizedLocalPath(downloads) != normalizedLocalPath(documents)) {
        addLocation(tr("Downloads"), downloads);
    }
    m_volumesItem = createItem(m_localMachineItem, tr("Volumes"), NodeKind::Volumes,
                               style()->standardIcon(QStyle::SP_DriveHDIcon));
}

void NavigationTree::rebuildServers()
{
    const QString selectedId = selectedProfileId();
    qDeleteAll(m_serversItem->takeChildren());
    auto addServer = [this, &selectedId](const QString& machineId, const QString& savedProfileId,
                                         const QString& displayName, const QString& username,
                                         const QString& host, quint16 port, bool active) {
        const QString label = active ? tr("%1 — Connected").arg(displayName) : displayName;
        auto* const item = createItem(
            m_serversItem, label, NodeKind::ServerProfile,
            style()->standardIcon(active ? QStyle::SP_DialogApplyButton : QStyle::SP_ComputerIcon));
        item->setData(0, ProfileIdRole, machineId);
        item->setData(0, SavedProfileIdRole, savedProfileId);
        item->setData(0, ActiveRole, active);
        item->setToolTip(
            0, Qt::convertFromPlainText(tr("%1@%2:%3").arg(username, host, QString::number(port))));
        if (active) {
            QFont font = item->font(0);
            font.setBold(true);
            item->setFont(0, font);
            const QString homePath =
                m_activeInitialPath.isEmpty() ? QStringLiteral(".") : m_activeInitialPath;
            auto* const home = createItem(item, tr("Home"), NodeKind::RemoteDirectory,
                                          style()->standardIcon(QStyle::SP_DirHomeIcon));
            home->setData(0, PathRole, homePath);
            home->setData(0, ProfileIdRole, machineId);
            home->setToolTip(0, Qt::convertFromPlainText(homePath));
            addLazyPlaceholder(home);
            if (homePath != QStringLiteral("/")) {
                auto* const root = createItem(item, QStringLiteral("/"), NodeKind::RemoteDirectory,
                                              style()->standardIcon(QStyle::SP_DriveHDIcon));
                root->setData(0, PathRole, QStringLiteral("/"));
                root->setData(0, ProfileIdRole, machineId);
                addLazyPlaceholder(root);
            }
            auto* const volumesItem = createItem(item, tr("Volumes"), NodeKind::Volumes,
                                                 style()->standardIcon(QStyle::SP_DriveHDIcon));
            QTreeWidgetItem* externalDevicesItem = nullptr;
            if (m_remoteStorageProfileId == machineId) {
                synchronizeStorageBranches(item, volumesItem, externalDevicesItem,
                                           uniqueStorageVolumes(m_remoteStorageVolumes, false),
                                           false, machineId);
            }
            item->setExpanded(true);
        }
        if (savedProfileId == selectedId) {
            m_tree->setCurrentItem(item);
        }
    };

    bool activeMachineAdded = false;
    for (const rfm::core::ConnectionProfile& profile : std::as_const(m_profiles)) {
        const bool active = m_activeMachine.isValid() &&
                            profile.id == m_activeMachine.savedProfileId &&
                            profile.host.trimmed() == m_activeMachine.host.trimmed() &&
                            profile.username.trimmed() == m_activeMachine.username.trimmed() &&
                            profile.port == m_activeMachine.port;
        addServer(active ? m_activeMachine.id : profile.id, profile.id,
                  active ? m_activeMachine.displayName : profile.effectiveDisplayName(),
                  active ? m_activeMachine.username : profile.username,
                  active ? m_activeMachine.host : profile.host,
                  active ? m_activeMachine.port : profile.port, active);
        activeMachineAdded = activeMachineAdded || active;
    }
    if (m_activeMachine.isValid() && !activeMachineAdded) {
        const QString displayName =
            safePresentationText(m_activeMachine.displayName).isEmpty()
                ? QStringLiteral("%1@%2").arg(m_activeMachine.username, m_activeMachine.host)
                : safePresentationText(m_activeMachine.displayName);
        addServer(m_activeMachine.id, {}, displayName, m_activeMachine.username,
                  m_activeMachine.host, m_activeMachine.port, true);
    }
}

void NavigationTree::activateItem(QTreeWidgetItem* item)
{
    if (item == nullptr) {
        return;
    }
    const NodeKind kind = itemKind(item);
    const QString path = item->data(0, PathRole).toString();
    if ((kind == NodeKind::LocalLocation || kind == NodeKind::LocalDirectory) && !path.isEmpty()) {
        emit localLocationActivated(path);
    } else if (kind == NodeKind::RemoteDirectory && !path.isEmpty()) {
        emit remoteLocationActivated(item->data(0, ProfileIdRole).toString(), path);
    }
}

void NavigationTree::expandItem(QTreeWidgetItem* item)
{
    if (item == nullptr || item->data(0, LoadedRole).toBool()) {
        return;
    }
    const NodeKind kind = itemKind(item);
    const QString path = item->data(0, PathRole).toString();
    if (kind != NodeKind::LocalLocation && kind != NodeKind::LocalDirectory &&
        kind != NodeKind::RemoteDirectory) {
        return;
    }
    qDeleteAll(item->takeChildren());
    auto* const loading = createItem(item, tr("Loading…"), NodeKind::Placeholder);
    loading->setDisabled(true);
    item->setData(0, LoadedRole, true);
    if (kind == NodeKind::RemoteDirectory) {
        emit remoteDirectoryExpansionRequested(item->data(0, ProfileIdRole).toString(), path);
    } else {
        emit localDirectoryExpansionRequested(path);
    }
}

void NavigationTree::addLazyPlaceholder(QTreeWidgetItem* item)
{
    item->setData(0, LoadedRole, false);
    auto* const placeholder = createItem(item, tr("Expand to load"), NodeKind::Placeholder);
    placeholder->setDisabled(true);
}

void NavigationTree::replaceDirectoryChildren(QTreeWidgetItem* item,
                                              const QList<rfm::core::RemoteEntry>& entries,
                                              bool local, const QString& profileId)
{
    qDeleteAll(item->takeChildren());
    QFileIconProvider icons;
    for (const rfm::core::RemoteEntry& entry : entries) {
        if (!entry.directory) {
            continue;
        }
        auto* const child = createItem(item, entry.name,
                                       local ? NodeKind::LocalDirectory : NodeKind::RemoteDirectory,
                                       icons.icon(QFileIconProvider::Folder));
        const QString parentPath = item->data(0, PathRole).toString();
        const QString childPath = local ? QDir(parentPath).filePath(entry.name)
                                        : rfm::core::RemotePath::join(parentPath, entry.name);
        child->setData(0, PathRole,
                       local ? normalizedLocalPath(childPath)
                             : rfm::core::RemotePath::normalize(childPath));
        child->setData(0, ProfileIdRole, profileId);
        addLazyPlaceholder(child);
    }
    item->setData(0, LoadedRole, true);
}

QList<QTreeWidgetItem*> NavigationTree::matchingItems(NodeKind kind, const QString& path,
                                                      const QString& profileId) const
{
    QList<QTreeWidgetItem*> matches;
    QList<QTreeWidgetItem*> pending;
    for (int index = 0; index < m_tree->topLevelItemCount(); ++index) {
        pending.push_back(m_tree->topLevelItem(index));
    }
    while (!pending.isEmpty()) {
        QTreeWidgetItem* const item = pending.takeLast();
        if (itemKind(item) == kind && item->data(0, PathRole).toString() == path &&
            (profileId.isEmpty() || item->data(0, ProfileIdRole).toString() == profileId)) {
            matches.push_back(item);
        }
        for (int index = 0; index < item->childCount(); ++index) {
            pending.push_back(item->child(index));
        }
    }
    return matches;
}

NavigationTree::NodeKind NavigationTree::itemKind(const QTreeWidgetItem* item)
{
    return static_cast<NodeKind>(item->data(0, KindRole).toInt());
}

QString NavigationTree::normalizedLocalPath(const QString& path)
{
    return path.isEmpty() ? QString{} : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace rfm::app
