#include "remotefilemanager/app/NavigationTree.hpp"

#include "remotefilemanager/core/BrowserLocation.hpp"
#include "remotefilemanager/core/RemotePath.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QStyle>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace rfm::app
{
namespace
{

enum class PlaceKind {
    Home,
    Desktop,
    Documents,
    Downloads,
    Music,
    Pictures,
    Videos,
    PublicShare,
    Templates,
    Folder
};

struct StandardLocalPlace {
    QStandardPaths::StandardLocation location;
    PlaceKind kind;
};

constexpr std::array<StandardLocalPlace, 9> standardLocalPlaces{{
    {QStandardPaths::HomeLocation, PlaceKind::Home},
    {QStandardPaths::DesktopLocation, PlaceKind::Desktop},
    {QStandardPaths::DocumentsLocation, PlaceKind::Documents},
    {QStandardPaths::DownloadLocation, PlaceKind::Downloads},
    {QStandardPaths::MusicLocation, PlaceKind::Music},
    {QStandardPaths::PicturesLocation, PlaceKind::Pictures},
    {QStandardPaths::MoviesLocation, PlaceKind::Videos},
    {QStandardPaths::PublicShareLocation, PlaceKind::PublicShare},
    {QStandardPaths::TemplatesLocation, PlaceKind::Templates},
}};

bool isCategoryItem(const QTreeWidgetItem* item)
{
    if (item == nullptr) {
        return false;
    }
    const QVariant kindData = item->data(0, Qt::UserRole);
    if (!kindData.isValid()) {
        return false;
    }
    const auto kind = static_cast<NavigationTree::NodeKind>(kindData.toInt());
    return kind == NavigationTree::NodeKind::LocalCategory ||
           kind == NavigationTree::NodeKind::RemoteCategory;
}

class PlacesTreeWidget final : public QTreeWidget
{
  public:
    explicit PlacesTreeWidget(QWidget* parent) : QTreeWidget(parent) {}

  protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (isCategoryItem(itemAt(event->position().toPoint()))) {
            event->accept();
            return;
        }
        QTreeWidget::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (isCategoryItem(itemAt(event->position().toPoint()))) {
            event->accept();
            return;
        }
        QTreeWidget::mouseDoubleClickEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (isCategoryItem(currentItem())) {
            event->accept();
            return;
        }
        QTreeWidget::keyPressEvent(event);
    }
};

QString comparableLocalPath(const QString& path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    const QString normalized =
        QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
#ifdef Q_OS_WIN
    return normalized.toCaseFolded();
#else
    return normalized;
#endif
}

QIcon placeIcon(PlaceKind kind, const QWidget* widget)
{
    QFileIconProvider provider;
    const QIcon folder = provider.icon(QFileIconProvider::Folder);
    switch (kind) {
    case PlaceKind::Home:
        return QIcon::fromTheme(QStringLiteral("user-home"),
                                widget->style()->standardIcon(QStyle::SP_DirHomeIcon));
    case PlaceKind::Desktop:
        return QIcon::fromTheme(QStringLiteral("user-desktop"),
                                widget->style()->standardIcon(QStyle::SP_DesktopIcon));
    case PlaceKind::Documents:
        return QIcon::fromTheme(QStringLiteral("folder-documents"), folder);
    case PlaceKind::Downloads:
        return QIcon::fromTheme(QStringLiteral("folder-download"), folder);
    case PlaceKind::Music:
        return QIcon::fromTheme(QStringLiteral("folder-music"), folder);
    case PlaceKind::Pictures:
        return QIcon::fromTheme(QStringLiteral("folder-pictures"), folder);
    case PlaceKind::Videos:
        return QIcon::fromTheme(QStringLiteral("folder-videos"), folder);
    case PlaceKind::PublicShare:
        return QIcon::fromTheme(QStringLiteral("folder-publicshare"), folder);
    case PlaceKind::Templates:
        return QIcon::fromTheme(QStringLiteral("folder-templates"), folder);
    case PlaceKind::Folder:
        return QIcon::fromTheme(QStringLiteral("folder"), folder);
    }
    return folder;
}

PlaceKind placeKindForRemoteName(const QString& name)
{
    const QString folded = name.toCaseFolded();
    if (folded == QStringLiteral("desktop")) return PlaceKind::Desktop;
    if (folded == QStringLiteral("documents")) return PlaceKind::Documents;
    if (folded == QStringLiteral("downloads")) return PlaceKind::Downloads;
    if (folded == QStringLiteral("music")) return PlaceKind::Music;
    if (folded == QStringLiteral("pictures")) return PlaceKind::Pictures;
    if (folded == QStringLiteral("videos") || folded == QStringLiteral("movies"))
        return PlaceKind::Videos;
    return PlaceKind::Folder;
}

PlaceKind placeKindForLocalPath(const QString& path)
{
    const QString localPath = comparableLocalPath(path);
    if (localPath.isEmpty()) {
        return PlaceKind::Folder;
    }

    QSet<QString> assignedPaths;
    for (const StandardLocalPlace& place : standardLocalPlaces) {
        const QString configuredPath = QStandardPaths::writableLocation(place.location);
        if (configuredPath.isEmpty() || !QFileInfo(configuredPath).isDir()) {
            continue;
        }
        const QString configuredLocalPath = comparableLocalPath(configuredPath);
        if (configuredLocalPath.isEmpty() || assignedPaths.contains(configuredLocalPath)) {
            continue;
        }
        assignedPaths.insert(configuredLocalPath);
        if (localPath == configuredLocalPath) {
            return place.kind;
        }
    }
    return PlaceKind::Folder;
}

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

QTreeWidgetItem* createCategory(QTreeWidgetItem* parent, const QString& text,
                                NavigationTree::NodeKind kind)
{
    auto* const item = createItem(parent, text, kind);
    QFont font = item->font(0);
    font.setBold(true);
    item->setFont(0, font);
    item->setFlags(Qt::ItemIsEnabled);
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
    append(QCoreApplication::translate("NavigationTree", "Label"), volume.fileSystemLabel);
    append(QCoreApplication::translate("NavigationTree", "Device"), volume.device);
    append(QCoreApplication::translate("NavigationTree", "Status"),
           volume.mounted ? QCoreApplication::translate("NavigationTree", "Mounted")
                          : QCoreApplication::translate("NavigationTree", "Not mounted"));
    if (volume.mounted) {
        append(QCoreApplication::translate("NavigationTree", "Mount point"), volume.rootPath);
    }
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

QString remoteStorageIdentity(const rfm::core::StorageVolume& volume)
{
    const QString device = rfm::core::RemotePath::normalize(volume.device.trimmed());
    const QString path = rfm::core::RemotePath::normalize(volume.rootPath);
    if (volume.mounted && device.startsWith(QChar{'/'}) && path.startsWith(QChar{'/'})) {
        return QStringLiteral("mounted-device\n%1\npath\n%2").arg(device, path);
    }
    if (!volume.mounted && device.startsWith(QChar{'/'})) {
        return QStringLiteral("device\n%1").arg(device);
    }
    return path.startsWith(QChar{'/'}) ? QStringLiteral("path\n%1").arg(path) : QString{};
}

QStringList storageDisplayNames(const QList<rfm::core::StorageVolume>& volumes, bool local)
{
    QStringList baseNames;
    QHash<QString, int> occurrences;
    for (const rfm::core::StorageVolume& volume : volumes) {
        // A mounted entry represents a location the user can open. Its mount
        // point is therefore the most useful primary name; disk metadata stays
        // available in the tooltip.
        QString base = volume.mounted ? safePresentationText(volume.rootPath)
                                      : safePresentationText(volume.displayName);
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
    m_tree = new PlacesTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("navigationTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(false);
    m_tree->setColumnCount(2);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setAnimated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_tree->setColumnWidth(1, 28);
    layout->addWidget(m_tree);

    auto* const volumeActions = new QHBoxLayout;
    volumeActions->setContentsMargins(0, 0, 0, 0);
    m_mountVolumeButton = new QPushButton(tr("Mount"), this);
    m_mountVolumeButton->setObjectName(QStringLiteral("mountVolumeButton"));
    m_openVolumeButton = new QPushButton(tr("Open"), this);
    m_openVolumeButton->setObjectName(QStringLiteral("openVolumeButton"));
    m_unmountVolumeButton = new QPushButton(tr("Unmount"), this);
    m_unmountVolumeButton->setObjectName(QStringLiteral("unmountVolumeButton"));
    volumeActions->addWidget(m_mountVolumeButton);
    volumeActions->addWidget(m_openVolumeButton);
    volumeActions->addWidget(m_unmountVolumeButton);
    volumeActions->addStretch();
    layout->addLayout(volumeActions);

    auto* const localCategory =
        createCategory(m_tree->invisibleRootItem(), tr("Local"), NodeKind::LocalCategory);
    m_remoteCategoryItem =
        createCategory(m_tree->invisibleRootItem(), tr("Distant"), NodeKind::RemoteCategory);
    auto* const addServerButton = new QToolButton(m_tree);
    addServerButton->setObjectName(QStringLiteral("placesNewConnectionButton"));
    addServerButton->setIcon(QIcon::fromTheme(
        QStringLiteral("list-add"), style()->standardIcon(QStyle::SP_FileDialogNewFolder)));
    addServerButton->setAutoRaise(true);
    addServerButton->setFocusPolicy(Qt::NoFocus);
    addServerButton->setToolTip(tr("New connection"));
    addServerButton->setAccessibleName(tr("New connection"));
    addServerButton->setIconSize(QSize(16, 16));
    addServerButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    connect(addServerButton, &QToolButton::clicked, this, &NavigationTree::newConnectionRequested);
    m_tree->setItemWidget(m_remoteCategoryItem, 1, addServerButton);
    buildLocalMachine();
    localCategory->addChild(m_localMachineItem);
    m_localMachineItem->setExpanded(true);
    localCategory->setExpanded(true);
    m_remoteCategoryItem->setExpanded(true);

    connect(m_tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item, int) { activateItem(item); });
    connect(m_tree, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem* item) { expandItem(item); });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [](QTreeWidgetItem* item) {
        if (isCategoryItem(item)) {
            item->setExpanded(true);
        }
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem*, QTreeWidgetItem*) {
                updateVolumeActions();
                emit selectedProfileChanged();
            });
    connect(m_mountVolumeButton, &QPushButton::clicked, this, &NavigationTree::mountSelectedVolume);
    connect(m_openVolumeButton, &QPushButton::clicked, this, [this] {
        if (const auto selected = selectedStorageVolume(); selected.has_value() &&
                                                           selected->volume.mounted &&
                                                           !selected->volume.rootPath.isEmpty()) {
            if (selected->local) {
                emit localLocationActivated(selected->volume.rootPath);
            } else {
                emit remoteLocationActivated(selected->machineId, selected->volume.rootPath);
            }
        }
    });
    connect(m_unmountVolumeButton, &QPushButton::clicked, this,
            &NavigationTree::unmountSelectedVolume);
    updateVolumeActions();
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

QList<NavigationTree::ContextAction>
NavigationTree::contextActionsAt(const QPoint& viewportPosition) const
{
    const QTreeWidgetItem* const item = m_tree->itemAt(viewportPosition);
    if (item == nullptr) {
        return {};
    }
    if (item->data(0, VolumeRole).isValid()) {
        const QString machineId = item->data(0, ProfileIdRole).toString();
        const SelectedStorageVolume selected{
            item->data(0, VolumeRole).value<rfm::core::StorageVolume>(), machineId,
            machineId == QString::fromLatin1(rfm::core::LocalMachineId)};
        if (selected.volume.mounted) {
            QList<ContextAction> actions{ContextAction::Open};
            if (canUnmountVolume(selected)) {
                actions.push_back(ContextAction::Unmount);
            }
            actions.push_back(ContextAction::Properties);
            return actions;
        }
        return {ContextAction::Mount, ContextAction::Properties};
    }

    const NodeKind kind = itemKind(item);
    if (kind == NodeKind::ServerProfile &&
        !item->data(0, SavedProfileIdRole).toString().isEmpty()) {
        return {item->data(0, ActiveRole).toBool() ? ContextAction::Disconnect
                                                   : ContextAction::Connect,
                ContextAction::Properties, ContextAction::RemoveServer};
    }
    if (kind == NodeKind::LocalLocation || kind == NodeKind::LocalDirectory ||
        kind == NodeKind::RemoteDirectory) {
        return {ContextAction::Open, ContextAction::Properties};
    }
    return {};
}

QString NavigationTree::selectedPropertiesTitle() const
{
    const QTreeWidgetItem* const item = m_tree->currentItem();
    if (item == nullptr) {
        return {};
    }
    const QString baseText = item->data(0, BaseTextRole).toString();
    return baseText.isEmpty() ? item->text(0) : baseText;
}

QString NavigationTree::selectedPropertiesText() const
{
    const QTreeWidgetItem* const item = m_tree->currentItem();
    if (item == nullptr) {
        return {};
    }
    if (item->data(0, VolumeRole).isValid()) {
        QTextDocument document;
        document.setHtml(item->toolTip(0));
        return document.toPlainText();
    }
    const NodeKind kind = itemKind(item);
    const QString path = item->data(0, PathRole).toString();
    if ((kind == NodeKind::LocalLocation || kind == NodeKind::LocalDirectory) && !path.isEmpty()) {
        return tr("Type: Local folder\nLocation: %1").arg(path);
    }
    if (kind == NodeKind::RemoteDirectory && !path.isEmpty()) {
        return tr("Type: Remote folder\nLocation: %1").arg(path);
    }
    return {};
}

void NavigationTree::selectItemAt(const QPoint& viewportPosition)
{
    if (QTreeWidgetItem* const item = m_tree->itemAt(viewportPosition)) {
        if (isCategoryItem(item)) {
            return;
        }
        m_tree->setCurrentItem(item);
    }
}

void NavigationTree::activateSelectedItem()
{
    const auto selected = selectedStorageVolume();
    if (selected.has_value() && m_volumeOperations.contains(volumeOperationIdentity(
                                    selected->machineId, selected->volume.device))) {
        return;
    }
    activateItem(m_tree->currentItem());
}

void NavigationTree::mountSelectedVolume()
{
    if (const auto selected = selectedStorageVolume(); selected.has_value()) {
        const QString localDevice = QDir::cleanPath(selected->volume.device.trimmed());
        const bool operableDevice = selected->local
                                        ? localDevice.startsWith(QStringLiteral("/dev/")) &&
                                              localDevice != QStringLiteral("/dev")
                                        : rfm::core::isSafeLinuxDevicePath(selected->volume.device);
        if (!operableDevice || selected->volume.mounted ||
            m_volumeOperations.contains(
                volumeOperationIdentity(selected->machineId, selected->volume.device))) {
            return;
        }
        if (selected->local) {
            emit localVolumeMountRequested(selected->volume);
        } else {
            emit remoteVolumeMountRequested(selected->machineId, selected->volume);
        }
    }
}

void NavigationTree::unmountSelectedVolume()
{
    if (const auto selected = selectedStorageVolume();
        selected.has_value() && canUnmountVolume(*selected)) {
        if (m_volumeOperations.contains(
                volumeOperationIdentity(selected->machineId, selected->volume.device))) {
            return;
        }
        if (selected->local) {
            emit localVolumeUnmountRequested(selected->volume);
        } else {
            emit remoteVolumeUnmountRequested(selected->machineId, selected->volume);
        }
    }
}

std::optional<rfm::core::StorageVolume> NavigationTree::selectedLocalStorageVolume() const
{
    const auto selected = selectedStorageVolume();
    if (!selected.has_value() || !selected->local) {
        return std::nullopt;
    }
    return selected->volume;
}

std::optional<SelectedStorageVolume> NavigationTree::selectedStorageVolume() const
{
    const QTreeWidgetItem* const item = m_tree->currentItem();
    if (item == nullptr || !item->data(0, VolumeRole).isValid()) {
        return std::nullopt;
    }
    const QString machineId = item->data(0, ProfileIdRole).toString();
    return SelectedStorageVolume{item->data(0, VolumeRole).value<rfm::core::StorageVolume>(),
                                 machineId,
                                 machineId == QString::fromLatin1(rfm::core::LocalMachineId)};
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
    updateVolumeActions();
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
    updateVolumeActions();
}

bool NavigationTree::hasLoadedLocalDirectory(const QString& path) const
{
    const QString normalizedPath = normalizedLocalPath(path);
    QList<QTreeWidgetItem*> items = matchingItems(NodeKind::LocalLocation, normalizedPath);
    items += matchingItems(NodeKind::LocalDirectory, normalizedPath);
    return std::ranges::any_of(items, [](const QTreeWidgetItem* item) {
        return item->data(0, LoadedRole).toBool();
    });
}

QList<rfm::core::StorageVolume>
NavigationTree::uniqueStorageVolumes(const QList<rfm::core::StorageVolume>& volumes,
                                     bool local) const
{
    QList<rfm::core::StorageVolume> uniqueVolumes;
    QHash<QString, qsizetype> indexes;
    QSet<QString> mountedDevices;
    if (local) {
        for (const rfm::core::StorageVolume& volume : volumes) {
            if (volume.mounted) {
                const QString identity = localStorageIdentity(volume);
                if (!identity.isEmpty()) {
                    mountedDevices.insert(identity);
                }
            }
        }
    }
    for (const rfm::core::StorageVolume& volume : volumes) {
        if (local && !volume.mounted && mountedDevices.contains(localStorageIdentity(volume))) {
            continue;
        }
        const QString identity =
            local ? localStorageIdentity(volume) : remoteStorageIdentity(volume);
        if (identity.isEmpty()) {
            continue;
        }
        const auto existing = indexes.constFind(identity);
        if (existing == indexes.cend()) {
            indexes.insert(identity, uniqueVolumes.size());
            uniqueVolumes.push_back(volume);
        } else {
            const rfm::core::StorageVolume& current = uniqueVolumes.at(existing.value());
            const bool preferMounted = volume.mounted && !current.mounted;
            const bool preferRoot = volume.mounted && current.mounted &&
                                    volume.rootPath == QStringLiteral("/") &&
                                    current.rootPath != QStringLiteral("/");
            const bool preferExternal = volume.kind == rfm::core::StorageKind::External &&
                                        current.kind != rfm::core::StorageKind::External;
            if (preferMounted || preferRoot || preferExternal) {
                uniqueVolumes[existing.value()] = volume;
            }
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
            const QString identity = item->data(0, StorageIdentityRole).toString();
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
            const QString identity =
                local ? localStorageIdentity(volume) : remoteStorageIdentity(volume);
            QTreeWidgetItem* item = existingItems.take(identity);
            const bool newlyCreated = item == nullptr;
            const QIcon icon =
                external ? QIcon::fromTheme(QStringLiteral("drive-removable-media"),
                                            style()->standardIcon(QStyle::SP_DriveHDIcon))
                         : (volume.kind == rfm::core::StorageKind::Network
                                ? QIcon::fromTheme(QStringLiteral("folder-remote"),
                                                   style()->standardIcon(QStyle::SP_DriveNetIcon))
                                : style()->standardIcon(QStyle::SP_DriveHDIcon));
            if (newlyCreated) {
                item = createItem(
                    parent, displayNames.at(index),
                    local ? (volume.mounted ? NodeKind::LocalLocation : NodeKind::LocalVolume)
                          : NodeKind::RemoteDirectory,
                    icon);
                if (volume.mounted) {
                    addLazyPlaceholder(item);
                }
            } else {
                item->setData(0, KindRole,
                              static_cast<int>(local ? (volume.mounted ? NodeKind::LocalLocation
                                                                       : NodeKind::LocalVolume)
                                                     : NodeKind::RemoteDirectory));
                item->setIcon(0, icon);
                if (volume.mounted && item->childCount() == 0 &&
                    !item->data(0, LoadedRole).toBool()) {
                    addLazyPlaceholder(item);
                } else if (!volume.mounted) {
                    qDeleteAll(item->takeChildren());
                    item->setData(0, LoadedRole, false);
                }
            }
            item->setData(0, BaseTextRole, displayNames.at(index));
            item->setData(0, PathRole, path);
            item->setData(0, ProfileIdRole,
                          local ? QString::fromLatin1(rfm::core::LocalMachineId) : machineId);
            item->setData(0, StorageIdentityRole, identity);
            item->setData(0, VolumeRole, QVariant::fromValue(volume));
            item->setToolTip(0, storageToolTip(volume));
            updateVolumeItemPresentation(item);

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
    if (m_remoteCategoryItem == nullptr || machineId.isEmpty()) {
        return nullptr;
    }
    for (int index = 0; index < m_remoteCategoryItem->childCount(); ++index) {
        QTreeWidgetItem* const item = m_remoteCategoryItem->child(index);
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

void NavigationTree::setLocalVolumeOperation(const QString& device,
                                             std::optional<rfm::core::VolumeOperation> operation)
{
    setVolumeOperation(QString::fromLatin1(rfm::core::LocalMachineId), device, operation);
}

void NavigationTree::setVolumeOperation(const QString& machineId, const QString& device,
                                        std::optional<rfm::core::VolumeOperation> operation)
{
    const QString identity = volumeOperationIdentity(machineId, device);
    if (identity.isEmpty()) {
        return;
    }
    if (operation.has_value()) {
        m_volumeOperations.insert(identity, *operation);
    } else {
        m_volumeOperations.remove(identity);
    }

    QList<QTreeWidgetItem*> pending;
    pending.push_back(m_localMachineItem);
    pending.push_back(m_remoteCategoryItem);
    while (!pending.isEmpty()) {
        QTreeWidgetItem* const item = pending.takeLast();
        if (item->data(0, VolumeRole).isValid()) {
            const rfm::core::StorageVolume volume =
                item->data(0, VolumeRole).value<rfm::core::StorageVolume>();
            if (volumeOperationIdentity(item->data(0, ProfileIdRole).toString(), volume.device) ==
                identity) {
                updateVolumeItemPresentation(item);
            }
        }
        for (int index = 0; index < item->childCount(); ++index) {
            pending.push_back(item->child(index));
        }
    }
    updateVolumeActions();
}

void NavigationTree::updateVolumeActions()
{
    const auto selectedVolume = selectedStorageVolume();
    const bool selected = selectedVolume.has_value();
    const rfm::core::StorageVolume volume =
        selected ? selectedVolume->volume : rfm::core::StorageVolume{};
    const QString localDevice = QDir::cleanPath(volume.device.trimmed());
    const bool operableDevice =
        selected && (selectedVolume->local ? localDevice.startsWith(QStringLiteral("/dev/")) &&
                                                 localDevice != QStringLiteral("/dev")
                                           : rfm::core::isSafeLinuxDevicePath(volume.device));
    const bool busy = selected && m_volumeOperations.contains(volumeOperationIdentity(
                                      selectedVolume->machineId, volume.device));
    const bool canMount = selected && operableDevice && !volume.mounted;
    const bool canUnmount = selected && operableDevice && canUnmountVolume(*selectedVolume);

    m_mountVolumeButton->setVisible(canMount);
    m_mountVolumeButton->setEnabled(canMount && !busy);
    m_openVolumeButton->setVisible(selected && volume.mounted);
    m_openVolumeButton->setEnabled(selected && volume.mounted && !busy);
    m_unmountVolumeButton->setVisible(canUnmount);
    m_unmountVolumeButton->setEnabled(canUnmount && !busy);
}

bool NavigationTree::canUnmountVolume(const SelectedStorageVolume& selected)
{
    const QString localDevice = QDir::cleanPath(selected.volume.device.trimmed());
    const bool operableDevice = selected.local
                                    ? localDevice.startsWith(QStringLiteral("/dev/")) &&
                                          localDevice != QStringLiteral("/dev")
                                    : rfm::core::isSafeLinuxDevicePath(selected.volume.device);
    return operableDevice && selected.volume.mounted &&
           rfm::core::RemotePath::normalize(selected.volume.rootPath) != QStringLiteral("/") &&
           selected.volume.kind != rfm::core::StorageKind::System;
}

void NavigationTree::updateVolumeItemPresentation(QTreeWidgetItem* item)
{
    if (item == nullptr || !item->data(0, VolumeRole).isValid()) {
        return;
    }
    const rfm::core::StorageVolume volume =
        item->data(0, VolumeRole).value<rfm::core::StorageVolume>();
    const QString identity =
        volumeOperationIdentity(item->data(0, ProfileIdRole).toString(), volume.device);
    const auto operation = m_volumeOperations.constFind(identity);
    if (operation == m_volumeOperations.cend()) {
        item->setText(0, item->data(0, BaseTextRole).toString());
        return;
    }
    const QString status =
        *operation == rfm::core::VolumeOperation::Mount ? tr("Mounting…") : tr("Unmounting…");
    item->setText(0, QStringLiteral("%1 — %2").arg(item->data(0, BaseTextRole).toString(), status));
}

QString NavigationTree::volumeOperationIdentity(const QString& machineId, const QString& device)
{
    const QString normalizedDevice = rfm::core::RemotePath::normalize(device.trimmed());
    if (machineId.isEmpty() || !normalizedDevice.startsWith(QStringLiteral("/dev/"))) {
        return {};
    }
    return machineId + QChar{'\n'} + normalizedDevice;
}

void NavigationTree::buildLocalMachine()
{
    m_localMachineItem =
        createItem(nullptr, tr("This Computer"), NodeKind::LocalMachine,
                   style()->standardIcon(QStyle::SP_ComputerIcon));
    auto addLocation = [this](QTreeWidgetItem* parent, const QString& name, const QString& path,
                              PlaceKind kind) -> QTreeWidgetItem* {
        if (path.isEmpty() || !QFileInfo(path).isDir()) {
            return nullptr;
        }
        auto* const item = createItem(parent, name, NodeKind::LocalLocation, placeIcon(kind, this));
        item->setData(0, PathRole, normalizedLocalPath(path));
        item->setToolTip(0, path);
        addLazyPlaceholder(item);
        return item;
    };
    QTreeWidgetItem* const home =
        addLocation(m_localMachineItem, tr("Home"),
                    QStandardPaths::writableLocation(QStandardPaths::HomeLocation), PlaceKind::Home);
    QSet<QString> paths{comparableLocalPath(
        QStandardPaths::writableLocation(QStandardPaths::HomeLocation))};
    if (home != nullptr) {
        for (const StandardLocalPlace& place : standardLocalPlaces) {
            const QString path = QStandardPaths::writableLocation(place.location);
            const QString normalized = comparableLocalPath(path);
            if (place.kind == PlaceKind::Home || normalized.isEmpty() ||
                paths.contains(normalized) || !QFileInfo(path).isDir()) {
                continue;
            }
            const QString name = QFileInfo(path).fileName();
            if (name.isEmpty()) {
                continue;
            }
            addLocation(home, name, path, place.kind);
            paths.insert(normalized);
        }
    }
    m_volumesItem = createItem(m_localMachineItem, tr("Volumes"), NodeKind::Volumes,
                               style()->standardIcon(QStyle::SP_DriveHDIcon));
}

void NavigationTree::rebuildServers()
{
    const QString selectedId = selectedProfileId();
    qDeleteAll(m_remoteCategoryItem->takeChildren());
    auto addServer = [this, &selectedId](const QString& machineId, const QString& savedProfileId,
                                         const QString& displayName, const QString& username,
                                         const QString& host, quint16 port, bool active) {
        const QString label = active ? tr("%1 — Connected").arg(displayName) : displayName;
        auto* const item = createItem(
            m_remoteCategoryItem, label, NodeKind::ServerProfile,
            style()->standardIcon(active ? QStyle::SP_DialogApplyButton : QStyle::SP_ComputerIcon));
        item->setData(0, ProfileIdRole, machineId);
        item->setData(0, SavedProfileIdRole, savedProfileId);
        item->setData(0, ActiveRole, active);
        item->setToolTip(
            0, Qt::convertFromPlainText(tr("%1@%2:%3").arg(username, host, QString::number(port))));
        if (!savedProfileId.isEmpty()) {
            auto* const editButton = new QToolButton(m_tree);
            editButton->setObjectName(QStringLiteral("placesEditServerButton"));
            editButton->setProperty("profileId", savedProfileId);
            editButton->setIcon(QIcon::fromTheme(
                QStringLiteral("document-edit"),
                style()->standardIcon(QStyle::SP_FileDialogDetailedView)));
            editButton->setAutoRaise(true);
            editButton->setFocusPolicy(Qt::NoFocus);
            editButton->setToolTip(tr("Edit server"));
            editButton->setAccessibleName(tr("Edit server"));
            editButton->setIconSize(QSize(16, 16));
            editButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

            connect(editButton, &QToolButton::clicked, this,
                    [this, savedProfileId] { emit editProfileRequested(savedProfileId); });
            m_tree->setItemWidget(item, 1, editButton);
        }
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
    const int scrollPosition = m_tree->verticalScrollBar()->value();
    const bool updatesEnabled = m_tree->updatesEnabled();
    m_tree->setUpdatesEnabled(false);

    auto childKey = [local](const QString& path) {
#ifdef Q_OS_WIN
        return local ? path.toCaseFolded() : path;
#else
        return path;
#endif
    };
    QHash<QString, QTreeWidgetItem*> existingChildren;
    QList<QTreeWidgetItem*> obsoleteChildren;
    const NodeKind expectedKind = local ? NodeKind::LocalDirectory : NodeKind::RemoteDirectory;
    for (int index = 0; index < item->childCount(); ++index) {
        QTreeWidgetItem* const child = item->child(index);
        if (itemKind(child) != expectedKind) {
            obsoleteChildren.push_back(child);
            continue;
        }
        const QString key = childKey(child->data(0, PathRole).toString());
        if (key.isEmpty() || existingChildren.contains(key)) {
            obsoleteChildren.push_back(child);
        } else {
            existingChildren.insert(key, child);
        }
    }

    int directoryIndex = 0;
    for (const rfm::core::RemoteEntry& entry : entries) {
        if (!entry.directory) {
            continue;
        }
        const QString parentPath = item->data(0, PathRole).toString();
        const QString childPath = local ? QDir(parentPath).filePath(entry.name)
                                        : rfm::core::RemotePath::join(parentPath, entry.name);
        const QString normalizedPath =
            local ? normalizedLocalPath(childPath) : rfm::core::RemotePath::normalize(childPath);
        QTreeWidgetItem* child = existingChildren.take(childKey(normalizedPath));
        if (child == nullptr) {
            const PlaceKind placeKind =
                local ? placeKindForLocalPath(normalizedPath) : placeKindForRemoteName(entry.name);
            child = createItem(item, entry.name, expectedKind, placeIcon(placeKind, this));
            addLazyPlaceholder(child);
        } else {
            child->setText(0, entry.name);
            const PlaceKind placeKind =
                local ? placeKindForLocalPath(normalizedPath) : placeKindForRemoteName(entry.name);
            child->setIcon(0, placeIcon(placeKind, this));
        }
        child->setData(0, PathRole, normalizedPath);
        child->setData(0, ProfileIdRole, profileId);
        const bool hidden = entry.hidden || entry.name.startsWith(QChar{'.'});
        child->setData(0, HiddenRole, hidden);
        child->setHidden(hidden && !m_showHiddenFiles);
        const int currentIndex = item->indexOfChild(child);
        if (currentIndex != directoryIndex) {
            item->takeChild(currentIndex);
            item->insertChild(directoryIndex, child);
        }
        ++directoryIndex;
    }
    obsoleteChildren.append(existingChildren.values());
    for (QTreeWidgetItem* const child : std::as_const(obsoleteChildren)) {
        item->removeChild(child);
        delete child;
    }
    item->setData(0, LoadedRole, true);
    m_tree->verticalScrollBar()->setValue(scrollPosition);
    m_tree->setUpdatesEnabled(updatesEnabled);
}

void NavigationTree::setShowHiddenFiles(bool show)
{
    m_showHiddenFiles = show;
    QList<QTreeWidgetItem*> pending;
    for (int index = 0; index < m_tree->topLevelItemCount(); ++index) {
        pending.push_back(m_tree->topLevelItem(index));
    }
    while (!pending.isEmpty()) {
        QTreeWidgetItem* const item = pending.takeLast();
        item->setHidden(item->data(0, HiddenRole).toBool() && !show);
        for (int index = 0; index < item->childCount(); ++index) {
            pending.push_back(item->child(index));
        }
    }
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

QString NavigationTree::localStorageIdentity(const rfm::core::StorageVolume& volume)
{
    const QString deviceNumber = volume.deviceNumber.trimmed();
    if (!deviceNumber.isEmpty()) {
        return QStringLiteral("number\n%1").arg(deviceNumber);
    }
    const QString device = QDir::cleanPath(volume.device.trimmed());
    if (device.startsWith(QStringLiteral("/dev/")) && device != QStringLiteral("/dev")) {
        const QString canonicalDevice = QFileInfo(device).canonicalFilePath();
        return QStringLiteral("device\n%1")
            .arg(canonicalDevice.isEmpty() ? device : QDir::cleanPath(canonicalDevice));
    }
    if (volume.mounted) {
        const QString path = normalizedLocalPath(volume.rootPath);
        return path.isEmpty() ? QString{} : QStringLiteral("path\n%1").arg(path);
    }
    return {};
}

} // namespace rfm::app
