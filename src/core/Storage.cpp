#include "remotefilemanager/core/Storage.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QHash>
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

bool strictMountRootAncestor(const QString& ancestor, const QString& descendant)
{
    if (ancestor == descendant) {
        return false;
    }
    if (ancestor == QStringLiteral("/")) {
        return descendant.startsWith(QChar{'/'});
    }
    return descendant.startsWith(ancestor + QChar{'/'});
}

bool demonstratedAttachmentAlias(const LinuxMountInfo& candidate, const LinuxMountInfo& current)
{
    if (candidate.mountRoot == current.mountRoot) {
        // mountinfo does not distinguish a root bind from two intentional
        // attachments of the same logical root. Preserve that ambiguity.
        return false;
    }
    if (candidate.fileSystemType.trimmed().compare(QByteArrayLiteral("btrfs"),
                                                   Qt::CaseInsensitive) == 0 ||
        isNetworkFileSystem(candidate.fileSystemType)) {
        return false;
    }
    // For an ordinary block filesystem, a second attachment rooted strictly
    // below another attachment of the same filesystem is a bind/sub-root alias.
    return strictMountRootAncestor(candidate.mountRoot, current.mountRoot) ||
           strictMountRootAncestor(current.mountRoot, candidate.mountRoot);
}

bool isPseudoFileSystem(const QByteArray& fileSystemType)
{
    // These filesystems expose kernel, session or RAM-backed state rather than
    // durable user storage. Keep this explicit list narrow: generic fuse.* and
    // fuseblk are intentionally absent because they can be navigable volumes.
    static const QSet<QByteArray> pseudoTypes{
        QByteArrayLiteral("autofs"),     QByteArrayLiteral("bpf"),
        QByteArrayLiteral("cgroup"),     QByteArrayLiteral("cgroup2"),
        QByteArrayLiteral("configfs"),   QByteArrayLiteral("debugfs"),
        QByteArrayLiteral("devpts"),     QByteArrayLiteral("devtmpfs"),
        QByteArrayLiteral("efivarfs"),   QByteArrayLiteral("fuse.portal"),
        QByteArrayLiteral("fusectl"),    QByteArrayLiteral("hugetlbfs"),
        QByteArrayLiteral("mqueue"),     QByteArrayLiteral("nsfs"),
        QByteArrayLiteral("proc"),       QByteArrayLiteral("pstore"),
        QByteArrayLiteral("ramfs"),      QByteArrayLiteral("rpc_pipefs"),
        QByteArrayLiteral("securityfs"), QByteArrayLiteral("sysfs"),
        QByteArrayLiteral("tmpfs"),      QByteArrayLiteral("tracefs")};
    return pseudoTypes.contains(fileSystemType.trimmed().toLower());
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
        if (!isPseudoFileSystem(visible.fileSystemType)) {
            visibleMounts.push_back(std::move(visible));
        }
    }

    // Process broad roots first so demonstrated sub-root binds can reuse their
    // representative. Different Btrfs roots and other ambiguous attachments are
    // intentionally preserved as independently navigable destinations.
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
            if (!demonstratedAttachmentAlias(mount, mounts.at(index))) {
                continue;
            }
            if (preferMountRepresentative(mount, mounts.at(index))) {
                mounts[index] = std::move(mount);
            }
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

} // namespace rfm::core
