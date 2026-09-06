#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/Storage.hpp"
#include "remotefilemanager/core/VolumeService.hpp"

#include <QHash>
#include <QWidget>

#include <optional>

class QTreeWidget;
class QTreeWidgetItem;
class QIcon;
class QPushButton;

namespace rfm::app
{

struct RemoteMachineDescriptor {
    QString id;
    QString displayName;
    QString host;
    QString username;
    quint16 port{22};
    QString savedProfileId;

    [[nodiscard]] bool isValid() const
    {
        return !id.isEmpty() && !host.isEmpty() && !username.isEmpty() && port != 0;
    }
};

struct SelectedStorageVolume {
    rfm::core::StorageVolume volume;
    QString machineId;
    bool local{false};
};

class NavigationTree final : public QWidget
{
    Q_OBJECT

  public:
    enum class NodeKind {
        LocalCategory,
        RemoteCategory,
        LocalLocation,
        LocalVolume,
        LocalDirectory,
        Volumes,
        ExternalDevices,
        ServerProfile,
        RemoteDirectory,
        Placeholder
    };

    enum class ContextAction {
        Connect,
        Disconnect,
        RemoveServer,
        Open,
        Mount,
        Unmount,
        Properties
    };

    explicit NavigationTree(QWidget* parent = nullptr);

    [[nodiscard]] QTreeWidget* tree() const;
    [[nodiscard]] QString selectedProfileId() const;
    [[nodiscard]] QList<ContextAction> contextActionsAt(const QPoint& viewportPosition) const;
    [[nodiscard]] std::optional<rfm::core::StorageVolume> selectedLocalStorageVolume() const;
    [[nodiscard]] std::optional<SelectedStorageVolume> selectedStorageVolume() const;
    [[nodiscard]] QString selectedPropertiesTitle() const;
    [[nodiscard]] QString selectedPropertiesText() const;

    void selectItemAt(const QPoint& viewportPosition);
    void activateSelectedItem();
    void mountSelectedVolume();
    void unmountSelectedVolume();

    void setProfiles(const QList<rfm::core::ConnectionProfile>& profiles);
    void setActiveServer(RemoteMachineDescriptor machine, const QString& initialPath);
    void clearActiveServer();
    void setStorageVolumes(const QList<rfm::core::StorageVolume>& volumes);
    void setRemoteStorageVolumes(const QString& profileId,
                                 const QList<rfm::core::StorageVolume>& volumes);
    [[nodiscard]] bool hasLoadedLocalDirectory(const QString& path) const;
    void setLocalDirectory(const QString& path, const QList<rfm::core::RemoteEntry>& entries);
    void setRemoteDirectory(const QString& profileId, const QString& path,
                            const QList<rfm::core::RemoteEntry>& entries);
    void setShowHiddenFiles(bool show);
    void setDirectoryError(bool local, const QString& profileId, const QString& path,
                           const QString& error);
    void setLocalVolumeOperation(const QString& device,
                                 std::optional<rfm::core::VolumeOperation> operation);
    void setVolumeOperation(const QString& machineId, const QString& device,
                            std::optional<rfm::core::VolumeOperation> operation);

  signals:
    void newConnectionRequested();
    void editProfileRequested(QString id);
    void localLocationActivated(QString path);
    void remoteLocationActivated(QString profileId, QString path);
    void localDirectoryExpansionRequested(QString path);
    void remoteDirectoryExpansionRequested(QString profileId, QString path);
    void localVolumeMountRequested(rfm::core::StorageVolume volume);
    void localVolumeUnmountRequested(rfm::core::StorageVolume volume);
    void remoteVolumeMountRequested(QString machineId, rfm::core::StorageVolume volume);
    void remoteVolumeUnmountRequested(QString machineId, rfm::core::StorageVolume volume);
    void selectedProfileChanged();

  private:
    static constexpr int KindRole = Qt::UserRole;
    static constexpr int PathRole = Qt::UserRole + 1;
    static constexpr int ProfileIdRole = Qt::UserRole + 2;
    static constexpr int LoadedRole = Qt::UserRole + 3;
    static constexpr int ActiveRole = Qt::UserRole + 4;
    static constexpr int SavedProfileIdRole = Qt::UserRole + 5;
    static constexpr int VolumeRole = Qt::UserRole + 6;
    static constexpr int StorageIdentityRole = Qt::UserRole + 7;
    static constexpr int BaseTextRole = Qt::UserRole + 8;
    static constexpr int HiddenRole = Qt::UserRole + 9;

    void buildLocalPlaces();
    void rebuildServers();
    void activateItem(QTreeWidgetItem* item);
    void expandItem(QTreeWidgetItem* item);
    void addLazyPlaceholder(QTreeWidgetItem* item);
    void replaceDirectoryChildren(QTreeWidgetItem* item,
                                  const QList<rfm::core::RemoteEntry>& entries, bool local,
                                  const QString& profileId);
    [[nodiscard]] QList<QTreeWidgetItem*> matchingItems(NodeKind kind, const QString& path,
                                                        const QString& profileId = {}) const;
    [[nodiscard]] static NodeKind itemKind(const QTreeWidgetItem* item);
    [[nodiscard]] static QString normalizedLocalPath(const QString& path);
    [[nodiscard]] static QString localStorageIdentity(const rfm::core::StorageVolume& volume);
    [[nodiscard]] static bool canUnmountVolume(const SelectedStorageVolume& selected);
    [[nodiscard]] static QString volumeOperationIdentity(const QString& machineId,
                                                         const QString& device);
    void updateVolumeActions();
    void updateVolumeItemPresentation(QTreeWidgetItem* item);
    [[nodiscard]] QList<rfm::core::StorageVolume>
    uniqueStorageVolumes(const QList<rfm::core::StorageVolume>& volumes, bool local) const;
    void synchronizeStorageBranches(QTreeWidgetItem* machineItem, QTreeWidgetItem* volumesItem,
                                    QTreeWidgetItem*& externalDevicesItem,
                                    const QList<rfm::core::StorageVolume>& volumes, bool local,
                                    const QString& machineId);
    [[nodiscard]] QTreeWidgetItem* directChild(QTreeWidgetItem* parent, NodeKind kind) const;
    [[nodiscard]] QTreeWidgetItem* serverItem(const QString& machineId) const;
    QTreeWidget* m_tree{nullptr};
    QPushButton* m_mountVolumeButton{nullptr};
    QPushButton* m_openVolumeButton{nullptr};
    QPushButton* m_unmountVolumeButton{nullptr};
    QTreeWidgetItem* m_localCategoryItem{nullptr};
    QTreeWidgetItem* m_volumesItem{nullptr};
    QTreeWidgetItem* m_externalDevicesItem{nullptr};
    QTreeWidgetItem* m_remoteCategoryItem{nullptr};
    QList<rfm::core::ConnectionProfile> m_profiles;
    QList<rfm::core::StorageVolume> m_remoteStorageVolumes;
    QHash<QString, rfm::core::VolumeOperation> m_volumeOperations;
    QString m_remoteStorageProfileId;
    RemoteMachineDescriptor m_activeMachine;
    QString m_activeInitialPath;
    bool m_showHiddenFiles{false};
};

} // namespace rfm::app
