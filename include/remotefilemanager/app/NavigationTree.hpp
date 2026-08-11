#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/Storage.hpp"

#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;
class QIcon;

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

class NavigationTree final : public QWidget
{
    Q_OBJECT

  public:
    enum class NodeKind {
        LocalMachine,
        Servers,
        LocalLocation,
        LocalDirectory,
        Volumes,
        ExternalDevices,
        ServerProfile,
        RemoteDirectory,
        Placeholder
    };

    explicit NavigationTree(QWidget* parent = nullptr);

    [[nodiscard]] QTreeWidget* tree() const;
    [[nodiscard]] QString selectedProfileId() const;
    [[nodiscard]] QString profileIdAt(const QPoint& viewportPosition) const;

    void setProfiles(const QList<rfm::core::ConnectionProfile>& profiles);
    void setActiveServer(RemoteMachineDescriptor machine, const QString& initialPath);
    void clearActiveServer();
    void setStorageVolumes(const QList<rfm::core::StorageVolume>& volumes);
    void setRemoteStorageVolumes(const QString& profileId,
                                 const QList<rfm::core::StorageVolume>& volumes);
    void setLocalDirectory(const QString& path, const QList<rfm::core::RemoteEntry>& entries);
    void setRemoteDirectory(const QString& profileId, const QString& path,
                            const QList<rfm::core::RemoteEntry>& entries);
    void setDirectoryError(bool local, const QString& profileId, const QString& path,
                           const QString& error);

  signals:
    void localLocationActivated(QString path);
    void remoteLocationActivated(QString profileId, QString path);
    void localDirectoryExpansionRequested(QString path);
    void remoteDirectoryExpansionRequested(QString profileId, QString path);
    void selectedProfileChanged();

  private:
    static constexpr int KindRole = Qt::UserRole;
    static constexpr int PathRole = Qt::UserRole + 1;
    static constexpr int ProfileIdRole = Qt::UserRole + 2;
    static constexpr int LoadedRole = Qt::UserRole + 3;
    static constexpr int ActiveRole = Qt::UserRole + 4;
    static constexpr int SavedProfileIdRole = Qt::UserRole + 5;

    void buildLocalMachine();
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
    [[nodiscard]] QList<rfm::core::StorageVolume>
    uniqueStorageVolumes(const QList<rfm::core::StorageVolume>& volumes, bool local) const;
    void synchronizeStorageBranches(QTreeWidgetItem* machineItem, QTreeWidgetItem* volumesItem,
                                    QTreeWidgetItem*& externalDevicesItem,
                                    const QList<rfm::core::StorageVolume>& volumes, bool local,
                                    const QString& machineId);
    [[nodiscard]] QTreeWidgetItem* directChild(QTreeWidgetItem* parent, NodeKind kind) const;
    [[nodiscard]] QTreeWidgetItem* serverItem(const QString& machineId) const;
    QTreeWidget* m_tree{nullptr};
    QTreeWidgetItem* m_localMachineItem{nullptr};
    QTreeWidgetItem* m_volumesItem{nullptr};
    QTreeWidgetItem* m_externalDevicesItem{nullptr};
    QTreeWidgetItem* m_serversItem{nullptr};
    QList<rfm::core::ConnectionProfile> m_profiles;
    QList<rfm::core::StorageVolume> m_remoteStorageVolumes;
    QString m_remoteStorageProfileId;
    RemoteMachineDescriptor m_activeMachine;
    QString m_activeInitialPath;
};

} // namespace rfm::app
