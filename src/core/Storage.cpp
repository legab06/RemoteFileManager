#include "remotefilemanager/core/Storage.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace rfm::core
{
namespace
{

QString meaningfulMetadata(const QString& value)
{
    const QString trimmed = value.trimmed();
    return trimmed.compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) == 0 ? QString{}
                                                                                : trimmed;
}

QString decodeMountInfoField(const QByteArray& field)
{
    QByteArray decoded;
    decoded.reserve(field.size());
    for (qsizetype index = 0; index < field.size(); ++index) {
        if (field.at(index) == '\\' && index + 3 < field.size()) {
            const QByteArray escape = field.sliced(index, 4);
            static const QHash<QByteArray, char> standardEscapes{
                {QByteArrayLiteral("\\040"), ' '},
                {QByteArrayLiteral("\\011"), '\t'},
                {QByteArrayLiteral("\\012"), '\n'},
                {QByteArrayLiteral("\\134"), '\\'}};
            const auto decodedEscape = standardEscapes.constFind(escape);
            if (decodedEscape != standardEscapes.cend()) {
                decoded.push_back(decodedEscape.value());
                index += 3;
                continue;
            }
        }
        decoded.push_back(field.at(index));
    }
    return QString::fromUtf8(decoded);
}

bool preferMountRepresentative(const LinuxMountInfo& candidate, const LinuxMountInfo& current)
{
    if ((candidate.rootPath == QStringLiteral("/")) != (current.rootPath == QStringLiteral("/"))) {
        return candidate.rootPath == QStringLiteral("/");
    }
    if ((candidate.mountRoot == QStringLiteral("/")) !=
        (current.mountRoot == QStringLiteral("/"))) {
        return candidate.mountRoot == QStringLiteral("/");
    }
    if (candidate.mountRoot.size() != current.mountRoot.size()) {
        return candidate.mountRoot.size() < current.mountRoot.size();
    }
    if (candidate.rootPath.size() != current.rootPath.size()) {
        return candidate.rootPath.size() < current.rootPath.size();
    }
    return candidate.rootPath.compare(current.rootPath, Qt::CaseInsensitive) < 0;
}

QString mountedPhysicalIdentity(const LinuxMountInfo& mount)
{
    if (isNetworkFileSystem(mount.fileSystemType)) {
        return QStringLiteral("network\n%1\n%2")
            .arg(QString::fromLatin1(mount.fileSystemType), mount.device);
    }
    if (!mount.deviceNumber.startsWith(QStringLiteral("0:"))) {
        return QStringLiteral("block\n%1\n%2")
            .arg(mount.deviceNumber, QString::fromLatin1(mount.fileSystemType));
    }
    return {};
}

LinuxMountInfo visibleOvermount(const QList<LinuxMountInfo>& mounts)
{
    QSet<quint64> mountIds;
    for (const LinuxMountInfo& mount : mounts) {
        mountIds.insert(mount.mountId);
    }

    QSet<quint64> hiddenMountIds;
    for (const LinuxMountInfo& mount : mounts) {
        if (mount.parentId != mount.mountId && mountIds.contains(mount.parentId)) {
            hiddenMountIds.insert(mount.parentId);
        }
    }

    const LinuxMountInfo* visible = nullptr;
    for (const LinuxMountInfo& mount : mounts) {
        if (!hiddenMountIds.contains(mount.mountId)) {
            if (visible != nullptr) {
                // Ambiguous stack: retain the first non-hidden candidate instead
                // of inventing an ordering from reusable numeric IDs.
                return *visible;
            }
            visible = &mount;
        }
    }
    // A cycle is malformed and has no top candidate. Keep the first record as a
    // deterministic fallback without interpreting the numeric IDs as chronology.
    return visible == nullptr ? mounts.constFirst() : *visible;
}

bool isPseudoFileSystem(const QByteArray& fileSystemType)
{
    // These filesystems expose kernel, session or RAM-backed state rather than
    // durable user storage. Keep this explicit list narrow: generic fuse.* and
    // fuseblk are intentionally absent because they can be navigable volumes.
    static const QSet<QByteArray> pseudoTypes{
        QByteArrayLiteral("autofs"),      QByteArrayLiteral("binfmt_misc"),
        QByteArrayLiteral("bpf"),         QByteArrayLiteral("cgroup"),
        QByteArrayLiteral("cgroup2"),     QByteArrayLiteral("configfs"),
        QByteArrayLiteral("debugfs"),     QByteArrayLiteral("devpts"),
        QByteArrayLiteral("devtmpfs"),    QByteArrayLiteral("efivarfs"),
        QByteArrayLiteral("fuse.portal"), QByteArrayLiteral("fusectl"),
        QByteArrayLiteral("hugetlbfs"),   QByteArrayLiteral("mqueue"),
        QByteArrayLiteral("nsfs"),        QByteArrayLiteral("overlay"),
        QByteArrayLiteral("proc"),        QByteArrayLiteral("pstore"),
        QByteArrayLiteral("ramfs"),       QByteArrayLiteral("rpc_pipefs"),
        QByteArrayLiteral("securityfs"),  QByteArrayLiteral("sysfs"),
        QByteArrayLiteral("tmpfs"),       QByteArrayLiteral("tracefs")};
    return pseudoTypes.contains(fileSystemType.trimmed().toLower());
}

bool isStorageVolumeCandidateImpl(const QByteArray& fileSystemType, const QString& deviceValue)
{
    const QString device = deviceValue.trimmed();
    if (isPseudoFileSystem(fileSystemType) || device.isEmpty() ||
        device.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0 ||
        device.compare(QStringLiteral("rootfs"), Qt::CaseInsensitive) == 0) {
        return false;
    }

    // Loop-mounted squashfs images are normally package/runtime internals
    // (for example Snap), not user storage destinations.
    return !(fileSystemType.trimmed().compare(QByteArrayLiteral("squashfs"),
                                              Qt::CaseInsensitive) == 0 &&
             device.startsWith(QStringLiteral("/dev/loop")));
}

QList<QByteArray> splitMountInfoFields(const QByteArray& value)
{
    QList<QByteArray> fields;
    for (const QByteArray& field : value.split(' ')) {
        if (!field.isEmpty()) {
            fields.push_back(field);
        }
    }
    return fields;
}

QString jsonString(const QJsonObject& object, const QString& name)
{
    const QJsonValue value = object.value(name);
    if (value.isString()) {
        return value.toString().trimmed();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'f', 0);
    }
    return {};
}

bool jsonBool(const QJsonObject& object, const QString& name)
{
    const QJsonValue value = object.value(name);
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isDouble()) {
        return value.toInt() != 0;
    }
    const QString text = value.toString().trimmed();
    return text == QStringLiteral("1") ||
           text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

quint64 jsonUnsignedInteger(const QJsonObject& object, const QString& name)
{
    const QJsonValue value = object.value(name);
    if (value.isDouble()) {
        const double number = value.toDouble();
        return number > 0 ? static_cast<quint64>(number) : quint64{0};
    }
    bool valid = false;
    const quint64 number = value.toString().toULongLong(&valid);
    return valid ? number : quint64{0};
}

QString firstMountPoint(const QJsonObject& object)
{
    const QJsonValue mountPoints = object.value(QStringLiteral("mountpoints"));
    if (mountPoints.isArray()) {
        for (const QJsonValue& value : mountPoints.toArray()) {
            const QString path = value.toString().trimmed();
            if (!path.isEmpty()) {
                return path;
            }
        }
    }
    return jsonString(object, QStringLiteral("mountpoint"));
}

void appendBlockDevices(const QJsonArray& entries, const QString& parentDevice,
                        QList<LinuxBlockDevice>& devices)
{
    for (const QJsonValue& value : entries) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        QString device = jsonString(object, QStringLiteral("path"));
        if (device.isEmpty()) {
            const QString name = jsonString(object, QStringLiteral("name"));
            device = name.startsWith(QChar{'/'}) ? name : QStringLiteral("/dev/") + name;
        }
        QString reportedParent = jsonString(object, QStringLiteral("pkname"));
        if (!reportedParent.isEmpty() && !reportedParent.startsWith(QChar{'/'})) {
            reportedParent.prepend(QStringLiteral("/dev/"));
        }
        if (reportedParent.isEmpty()) {
            reportedParent = parentDevice;
        }
        if (!device.isEmpty()) {
            devices.push_back({device, reportedParent, jsonString(object, QStringLiteral("type")),
                               jsonString(object, QStringLiteral("fstype")).toLatin1(),
                               jsonString(object, QStringLiteral("label")), firstMountPoint(object),
                               jsonString(object, QStringLiteral("tran")),
                               jsonString(object, QStringLiteral("model")),
                               jsonUnsignedInteger(object, QStringLiteral("size")),
                               jsonBool(object, QStringLiteral("rm")),
                               jsonBool(object, QStringLiteral("ro")),
                               jsonString(object, QStringLiteral("maj:min"))});
        }
        appendBlockDevices(object.value(QStringLiteral("children")).toArray(), device, devices);
    }
}

bool hasNavigableBlockFileSystem(const QByteArray& fileSystemType)
{
    const QByteArray normalized = fileSystemType.trimmed().toLower();
    if (normalized.isEmpty()) {
        return false;
    }
    static const QSet<QByteArray> nonNavigableTypes{
        QByteArrayLiteral("swap"),        QByteArrayLiteral("crypto_luks"),
        QByteArrayLiteral("lvm2_member"), QByteArrayLiteral("linux_raid_member"),
        QByteArrayLiteral("zfs_member"),  QByteArrayLiteral("bcache"),
        QByteArrayLiteral("drbd"),        QByteArrayLiteral("bitlocker")};
    return !nonNavigableTypes.contains(normalized);
}

bool externallyAttached(const QString& transport, bool removable)
{
    const QString normalized = transport.trimmed().toLower();
    return normalized == QStringLiteral("usb") || normalized == QStringLiteral("firewire") ||
           normalized == QStringLiteral("thunderbolt") ||
           (normalized == QStringLiteral("mmc") && removable);
}

} // namespace

bool isNetworkFileSystem(const QByteArray& fileSystemType)
{
    const QByteArray normalized = fileSystemType.trimmed().toLower();
    static const QSet<QByteArray> networkTypes{
        QByteArrayLiteral("9p"),   QByteArrayLiteral("afs"),        QByteArrayLiteral("ceph"),
        QByteArrayLiteral("cifs"), QByteArrayLiteral("fuse.sshfs"), QByteArrayLiteral("glusterfs"),
        QByteArrayLiteral("nfs"),  QByteArrayLiteral("nfs4"),       QByteArrayLiteral("smb3"),
        QByteArrayLiteral("smbfs")};
    return networkTypes.contains(normalized);
}

bool isStorageVolumeCandidate(const QByteArray& fileSystemType, const QString& device)
{
    return isStorageVolumeCandidateImpl(fileSystemType, device);
}

bool hasExternalStorageTransport(const QStringList& subsystemChain)
{
    static const QSet<QString> externalTransports{
        QStringLiteral("firewire"), QStringLiteral("thunderbolt"), QStringLiteral("usb")};
    return std::ranges::any_of(subsystemChain, [](const QString& subsystem) {
        return externalTransports.contains(subsystem.trimmed().toLower());
    });
}

StorageDeviceEvidence storageDeviceEvidence(const QList<StorageTopologyNode>& ancestry,
                                            bool blockDevice, bool virtualBlockDevice,
                                            bool topologyComplete)
{
    StorageDeviceEvidence evidence;
    evidence.blockDevice = blockDevice;
    evidence.virtualBlockDevice = virtualBlockDevice;
    evidence.topologyComplete = topologyComplete;
    QStringList subsystems;
    subsystems.reserve(ancestry.size());
    for (const StorageTopologyNode& node : ancestry) {
        evidence.removable = evidence.removable || node.removable;
        if (evidence.deviceModel.isEmpty()) {
            evidence.deviceModel = node.deviceModel.trimmed();
        }
        if (!node.subsystem.trimmed().isEmpty()) {
            subsystems.push_back(node.subsystem);
        }
    }
    evidence.externalTransport =
        hasExternalStorageTransport(subsystems) ||
        (subsystems.contains(QStringLiteral("mmc"), Qt::CaseInsensitive) && evidence.removable);
    return evidence;
}

QString storageDisplayName(const QString& fileSystemLabel, const QString& deviceModel,
                           const QString& device, const QString& mountPoint)
{
    const QString normalizedMountPoint = RemotePath::normalize(mountPoint.trimmed());
    if (normalizedMountPoint.startsWith(QChar{'/'})) {
        return normalizedMountPoint;
    }
    const QString label = meaningfulMetadata(fileSystemLabel);
    if (!label.isEmpty()) {
        return label;
    }
    const QString model = meaningfulMetadata(deviceModel);
    if (!model.isEmpty()) {
        return model;
    }
    const QString normalizedDevice = device.trimmed();
    if (!normalizedDevice.isEmpty()) {
        const QString deviceName = RemotePath::fileName(normalizedDevice);
        return deviceName.isEmpty() ? normalizedDevice : deviceName;
    }
    return mountPoint.trimmed();
}

StorageKind classifyStorage(const StorageClassificationEvidence& evidence)
{
    if (evidence.systemVolume) {
        return StorageKind::System;
    }
    if (evidence.networkFileSystem) {
        return StorageKind::Network;
    }
    if (evidence.virtualBlockDevice) {
        return StorageKind::Unknown;
    }
    if (evidence.externalTransport) {
        return StorageKind::External;
    }
    if (evidence.blockDevice && evidence.topologyComplete) {
        return StorageKind::Internal;
    }
    return StorageKind::Unknown;
}

StorageVolume makeStorageVolume(const LinuxMountInfo& mount, const StorageDeviceEvidence& device,
                                const QString& fileSystemLabel, quint64 bytesTotal)
{
    const StorageClassificationEvidence classification{mount.rootPath == QStringLiteral("/"),
                                                       isNetworkFileSystem(mount.fileSystemType),
                                                       device.blockDevice,
                                                       device.virtualBlockDevice,
                                                       device.externalTransport,
                                                       device.topologyComplete};
    StorageVolume volume;
    volume.rootPath = mount.rootPath;
    volume.device = mount.device;
    volume.fileSystemType = mount.fileSystemType;
    volume.bytesTotal = bytesTotal;
    volume.kind = classifyStorage(classification);
    volume.removable = device.removable;
    volume.ejectable = device.ejectable;
    volume.readOnly = mount.readOnly;
    volume.fileSystemLabel = fileSystemLabel.trimmed();
    volume.deviceModel = device.deviceModel.trimmed();
    volume.mounted = true;
    volume.deviceNumber = mount.deviceNumber;
    volume.displayName = storageDisplayName(volume.fileSystemLabel, volume.deviceModel,
                                            volume.device, volume.rootPath);
    return volume;
}

QList<LinuxMountInfo> parseLinuxMountInfo(const QByteArray& contents)
{
    static const QRegularExpression deviceNumberPattern(QStringLiteral("^[0-9]+:[0-9]+$"));
    QList<QList<LinuxMountInfo>> mountPointGroups;
    QHash<QString, qsizetype> mountPointIndexes;
    for (const QByteArray& line : contents.split('\n')) {
        const qsizetype separator = line.indexOf(" - ");
        if (separator < 0) {
            continue;
        }
        const QList<QByteArray> left = splitMountInfoFields(line.first(separator));
        const QList<QByteArray> right = splitMountInfoFields(line.sliced(separator + 3));
        if (left.size() < 6 || right.size() < 3) {
            continue;
        }
        bool mountIdOk = false;
        bool parentIdOk = false;
        const quint64 mountId = left.at(0).toULongLong(&mountIdOk);
        const quint64 parentId = left.at(1).toULongLong(&parentIdOk);
        const QString deviceNumber = QString::fromLatin1(left.at(2));
        const QString mountRoot = RemotePath::normalize(decodeMountInfoField(left.at(3)));
        const QString rootPath = RemotePath::normalize(decodeMountInfoField(left.at(4)));
        const QByteArray fileSystemType = right.at(0);
        const QString device = decodeMountInfoField(right.at(1));
        if (!mountIdOk || !parentIdOk || mountId == 0 || parentId == 0 ||
            !deviceNumberPattern.match(deviceNumber).hasMatch() ||
            !mountRoot.startsWith(QChar{'/'}) || !rootPath.startsWith(QChar{'/'})) {
            continue;
        }
        const QList<QByteArray> options = left.at(5).split(',');
        LinuxMountInfo mount{rootPath,
                             device,
                             fileSystemType,
                             deviceNumber,
                             options.contains(QByteArrayLiteral("ro")),
                             mountId,
                             parentId,
                             mountRoot,
                             left.sliced(6)};

        const auto existing = mountPointIndexes.constFind(rootPath);
        if (existing == mountPointIndexes.cend()) {
            mountPointIndexes.insert(rootPath, mountPointGroups.size());
            mountPointGroups.push_back({std::move(mount)});
        } else {
            mountPointGroups[existing.value()].push_back(std::move(mount));
        }
    }

    QList<LinuxMountInfo> visibleMounts;
    visibleMounts.reserve(mountPointGroups.size());
    for (const QList<LinuxMountInfo>& group : std::as_const(mountPointGroups)) {
        LinuxMountInfo visible = visibleOvermount(group);
        if (isStorageVolumeCandidate(visible.fileSystemType, visible.device)) {
            visibleMounts.push_back(std::move(visible));
        }
    }

    // Process broad roots first so one representative is retained for each
    // physical filesystem. A volume list is a list of accessible storage, not
    // a raw mount table: bind mounts and Btrfs subvolumes must not duplicate it.
    std::ranges::stable_sort(visibleMounts,
                             [](const LinuxMountInfo& first, const LinuxMountInfo& second) {
                                 return first.mountRoot.size() < second.mountRoot.size();
                             });
    QList<LinuxMountInfo> mounts;
    QHash<QString, QList<qsizetype>> physicalIndexes;
    for (LinuxMountInfo& mount : visibleMounts) {
        const QString identity = mountedPhysicalIdentity(mount);
        if (identity.isEmpty()) {
            mounts.push_back(std::move(mount));
            continue;
        }

        bool merged = false;
        for (const qsizetype index : std::as_const(physicalIndexes[identity])) {
            if (preferMountRepresentative(mount, mounts.at(index))) {
                mounts[index] = std::move(mount);
            }
            // The identity is a filesystem device number (or a network
            // export), so every later attachment is the same storage.
            merged = true;
            break;
        }
        if (!merged) {
            physicalIndexes[identity].push_back(mounts.size());
            mounts.push_back(std::move(mount));
        }
    }
    std::ranges::sort(mounts, [](const LinuxMountInfo& first, const LinuxMountInfo& second) {
        return first.rootPath.compare(second.rootPath, Qt::CaseInsensitive) < 0;
    });
    return mounts;
}

QList<LinuxBlockDevice> parseLinuxBlockDevices(const QByteArray& contents)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(contents, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    QList<LinuxBlockDevice> devices;
    appendBlockDevices(document.object().value(QStringLiteral("blockdevices")).toArray(), {},
                       devices);
    return devices;
}

QList<StorageVolume> mergeLinuxBlockDevices(QList<StorageVolume> mountedVolumes,
                                            const QList<LinuxBlockDevice>& blockDevices)
{
    QSet<QString> mountedDevices;
    QSet<QString> mountedDeviceNumbers;
    QSet<QString> mountedRoots;
    for (const StorageVolume& volume : std::as_const(mountedVolumes)) {
        const QString device = RemotePath::normalize(volume.device.trimmed());
        if (device.startsWith(QStringLiteral("/dev/"))) {
            mountedDevices.insert(device);
        }
        if (!volume.deviceNumber.trimmed().isEmpty()) {
            mountedDeviceNumbers.insert(volume.deviceNumber.trimmed());
        }
        const QString rootPath = RemotePath::normalize(volume.rootPath);
        if (volume.mounted && rootPath.startsWith(QChar{'/'})) {
            mountedRoots.insert(rootPath);
        }
    }

    QSet<QString> parentsWithNavigableChildren;
    QHash<QString, const LinuxBlockDevice*> devicesByPath;
    for (const LinuxBlockDevice& blockDevice : blockDevices) {
        const QString device = RemotePath::normalize(blockDevice.device.trimmed());
        devicesByPath.insert(device, &blockDevice);
        if (hasNavigableBlockFileSystem(blockDevice.fileSystemType) &&
            !blockDevice.parentDevice.trimmed().isEmpty()) {
            parentsWithNavigableChildren.insert(
                RemotePath::normalize(blockDevice.parentDevice.trimmed()));
        }
    }

    QSet<QString> addedDevices;
    for (const LinuxBlockDevice& blockDevice : blockDevices) {
        const QString device = RemotePath::normalize(blockDevice.device.trimmed());
        const QString objectType = blockDevice.objectType.trimmed().toLower();
        const QString mountPoint = RemotePath::normalize(blockDevice.mountPoint.trimmed());
        if (!device.startsWith(QStringLiteral("/dev/")) || device == QStringLiteral("/dev") ||
            !hasNavigableBlockFileSystem(blockDevice.fileSystemType) ||
            objectType == QStringLiteral("loop") || objectType == QStringLiteral("zram") ||
            parentsWithNavigableChildren.contains(device) || mountedDevices.contains(device) ||
            mountedDeviceNumbers.contains(blockDevice.deviceNumber.trimmed()) ||
            (mountPoint.startsWith(QChar{'/'}) && mountedRoots.contains(mountPoint)) ||
            addedDevices.contains(device)) {
            continue;
        }

        QString transport = blockDevice.transport.trimmed();
        QString model = blockDevice.deviceModel.trimmed();
        bool removable = blockDevice.removable;
        QString parentDevice = RemotePath::normalize(blockDevice.parentDevice.trimmed());
        QSet<QString> visitedParents;
        while (parentDevice.startsWith(QStringLiteral("/dev/")) &&
               !visitedParents.contains(parentDevice)) {
            visitedParents.insert(parentDevice);
            const auto parent = devicesByPath.constFind(parentDevice);
            if (parent == devicesByPath.cend()) {
                break;
            }
            removable = removable || (*parent)->removable;
            if (transport.isEmpty()) {
                transport = (*parent)->transport.trimmed();
            }
            if (model.isEmpty()) {
                model = (*parent)->deviceModel.trimmed();
            }
            parentDevice = RemotePath::normalize((*parent)->parentDevice.trimmed());
        }

        const bool mounted = mountPoint.startsWith(QChar{'/'});
        const bool external = externallyAttached(transport, removable);
        StorageVolume volume;
        volume.rootPath = mounted ? mountPoint : QString{};
        volume.device = device;
        volume.fileSystemType = blockDevice.fileSystemType.trimmed();
        volume.bytesTotal = blockDevice.bytesTotal;
        volume.kind = classifyStorage({mounted && mountPoint == QStringLiteral("/"), false, true,
                                       objectType == QStringLiteral("dm"), external,
                                       !transport.isEmpty() || removable});
        volume.removable = removable;
        volume.readOnly = blockDevice.readOnly;
        volume.fileSystemLabel = blockDevice.fileSystemLabel.trimmed();
        volume.deviceModel = model;
        volume.mounted = mounted;
        volume.deviceNumber = blockDevice.deviceNumber.trimmed();
        volume.displayName = storageDisplayName(volume.fileSystemLabel, volume.deviceModel,
                                                volume.device, volume.rootPath);
        mountedVolumes.push_back(std::move(volume));
        addedDevices.insert(device);
    }

    std::ranges::sort(mountedVolumes, [](const StorageVolume& first, const StorageVolume& second) {
        if (first.mounted != second.mounted) {
            return first.mounted;
        }
        const QString firstIdentity = first.mounted ? first.rootPath : first.displayName;
        const QString secondIdentity = second.mounted ? second.rootPath : second.displayName;
        return firstIdentity.compare(secondIdentity, Qt::CaseInsensitive) < 0;
    });
    return mountedVolumes;
}

QByteArray storageVolumeFingerprint(const QList<StorageVolume>& volumes)
{
    QStringList identities;
    identities.reserve(volumes.size());
    for (const StorageVolume& volume : volumes) {
        identities.push_back(
            QStringLiteral("%1\n%2\n%3\n%4\n%5\n%6\n%7")
                .arg(volume.mounted ? QStringLiteral("mounted") : QStringLiteral("available"),
                     volume.device, volume.rootPath, QString::fromLatin1(volume.fileSystemType),
                     volume.fileSystemLabel, QString::number(volume.bytesTotal),
                     QString::number(static_cast<int>(volume.kind))));
    }
    std::ranges::sort(identities);
    return QCryptographicHash::hash(identities.join(QChar{'\n'}).toUtf8(),
                                    QCryptographicHash::Sha256);
}

} // namespace rfm::core
