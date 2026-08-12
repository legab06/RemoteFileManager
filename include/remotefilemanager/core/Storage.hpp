#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <utility>

namespace rfm::core
{

enum class StorageKind { System, Internal, External, Network, Unknown };

struct StorageClassificationEvidence {
    bool systemVolume{false};
    bool networkFileSystem{false};
    bool blockDevice{false};
    bool virtualBlockDevice{false};
    bool externalTransport{false};
    bool topologyComplete{false};
};

struct StorageTopologyNode {
    StorageTopologyNode(QString subsystemValue = {}, bool removableValue = false,
                        QString deviceModelValue = {})
        : subsystem(std::move(subsystemValue)), removable(removableValue),
          deviceModel(std::move(deviceModelValue))
    {}

    // Empty means that this sysfs level has no readable subsystem link. It is
    // still part of the ancestry and must not terminate traversal.
    QString subsystem;
    bool removable{false};
    QString deviceModel;
};

struct StorageDeviceEvidence {
    bool blockDevice{false};
    bool virtualBlockDevice{false};
    bool externalTransport{false};
    bool topologyComplete{false};
    bool removable{false};
    bool ejectable{false};
    QString deviceModel;
};

struct StorageVolume {
    StorageVolume() = default;
    StorageVolume(QString displayNameValue, QString rootPathValue, QString deviceValue,
                  QByteArray fileSystemTypeValue, quint64 bytesTotalValue, StorageKind kindValue,
                  bool removableValue, bool ejectableValue, bool readOnlyValue,
                  QString fileSystemLabelValue = {}, QString deviceModelValue = {},
                  bool mountedValue = true)
        : displayName(std::move(displayNameValue)), rootPath(std::move(rootPathValue)),
          device(std::move(deviceValue)), fileSystemType(std::move(fileSystemTypeValue)),
          bytesTotal(bytesTotalValue), kind(kindValue), removable(removableValue),
          ejectable(ejectableValue), readOnly(readOnlyValue),
          fileSystemLabel(std::move(fileSystemLabelValue)),
          deviceModel(std::move(deviceModelValue)), mounted(mountedValue)
    {}

    // Presentation only. Navigation must always use rootPath.
    QString displayName;
    // Mounted filesystem root and stable navigation destination for this snapshot.
    QString rootPath;
    // System identity used for volume operations (for example /dev/sdb1), never presentation.
    QString device;
    QByteArray fileSystemType;
    quint64 bytesTotal{0};
    StorageKind kind{StorageKind::Unknown};
    bool removable{false};
    bool ejectable{false};
    bool readOnly{false};
    QString fileSystemLabel;
    QString deviceModel;
    // Explicit availability state. rootPath is a path only and is not used as this flag.
    bool mounted{true};
};

// Common representation of one lsblk JSON object. Acquisition is platform/backend-specific;
// parsing and conversion to StorageVolume are shared by local and SSH discovery.
struct LinuxBlockDevice {
    QString device;
    QString parentDevice;
    QString objectType;
    QByteArray fileSystemType;
    QString fileSystemLabel;
    QString mountPoint;
    QString transport;
    QString deviceModel;
    quint64 bytesTotal{0};
    bool removable{false};
    bool readOnly{false};
};

struct LinuxMountInfo {
    QString rootPath;
    QString device;
    QByteArray fileSystemType;
    QString deviceNumber;
    bool readOnly{false};
    quint64 mountId{0};
    quint64 parentId{0};
    QString mountRoot;
    QList<QByteArray> optionalFields;
};

[[nodiscard]] bool isNetworkFileSystem(const QByteArray& fileSystemType);
[[nodiscard]] bool hasExternalStorageTransport(const QStringList& subsystemChain);
[[nodiscard]] StorageDeviceEvidence
storageDeviceEvidence(const QList<StorageTopologyNode>& ancestry, bool blockDevice,
                      bool virtualBlockDevice, bool topologyComplete);
[[nodiscard]] StorageKind classifyStorage(const StorageClassificationEvidence& evidence);
[[nodiscard]] QString storageDisplayName(const QString& fileSystemLabel, const QString& deviceModel,
                                         const QString& device, const QString& mountPoint);
[[nodiscard]] StorageVolume makeStorageVolume(const LinuxMountInfo& mount,
                                              const StorageDeviceEvidence& device,
                                              const QString& fileSystemLabel = {},
                                              quint64 bytesTotal = 0);
[[nodiscard]] QList<LinuxMountInfo> parseLinuxMountInfo(const QByteArray& contents);
[[nodiscard]] QList<LinuxBlockDevice> parseLinuxBlockDevices(const QByteArray& contents);
[[nodiscard]] QList<StorageVolume>
mergeLinuxBlockDevices(QList<StorageVolume> mountedVolumes,
                       const QList<LinuxBlockDevice>& blockDevices);
[[nodiscard]] QByteArray storageVolumeFingerprint(const QList<StorageVolume>& volumes);

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::StorageKind)
Q_DECLARE_METATYPE(rfm::core::StorageVolume)
Q_DECLARE_METATYPE(QList<rfm::core::StorageVolume>)
