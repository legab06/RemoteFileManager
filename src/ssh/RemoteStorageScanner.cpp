#include "remotefilemanager/ssh/RemoteStorageScanner.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

namespace rfm::ssh
{
namespace
{

QString resolveRemoteLink(const QString& linkPath, const QString& target)
{
    if (target.startsWith(QChar{'/'})) {
        return rfm::core::RemotePath::normalize(target);
    }
    return rfm::core::RemotePath::normalize(rfm::core::RemotePath::parent(linkPath) + QChar{'/'} +
                                            target);
}

bool connectionError(RemoteStorageError error)
{
    return error == RemoteStorageError::ConnectionLost;
}

bool cancellationError(RemoteStorageError error) { return error == RemoteStorageError::Cancelled; }

} // namespace

RemoteStorageScanner::RemoteStorageScanner(std::unique_ptr<RemoteStorageReader> reader,
                                           quint64 requestId)
    : RemoteStorageScanner(std::move(reader), requestId, Limits{})
{}

RemoteStorageScanner::RemoteStorageScanner(std::unique_ptr<RemoteStorageReader> reader,
                                           quint64 requestId, Limits limits)
    : m_reader(std::move(reader)), m_requestId(requestId), m_limits(limits)
{}

RemoteStorageScanner::~RemoteStorageScanner()
{
    closeMountInfo();
    closeLabels();
}

quint64 RemoteStorageScanner::requestId() const { return m_requestId; }

RemoteStorageScanStep RemoteStorageScanner::step()
{
    if (m_stage == Stage::Completed || m_stage == Stage::Failed ||
        m_stage == Stage::ConnectionLost || m_stage == Stage::Cancelled) {
        return terminalStep();
    }
    if (m_reader == nullptr || !m_reader->connectionAlive()) {
        return fail(
            RemoteStorageScanStatus::ConnectionLost,
            QCoreApplication::translate("RemoteStorageScanner",
                                        "The SSH connection was lost during storage discovery."));
    }

    switch (m_stage) {
    case Stage::BeginMountInfo: {
        if (m_limits.maximumMountInfoBytes <= 0 || m_limits.maximumMountInfoChunkBytes <= 0) {
            return fail(RemoteStorageScanStatus::Failed,
                        QCoreApplication::translate(
                            "RemoteStorageScanner",
                            "The remote mount information safety limit is invalid."));
        }
        const RemoteStorageError error = m_reader->beginMountInfo();
        if (error != RemoteStorageError::None) {
            return handleRequiredError(
                error,
                QCoreApplication::translate("RemoteStorageScanner",
                                            "Unable to open the remote Linux mount information."));
        }
        m_mountInfoOpen = true;
        m_stage = Stage::MountInfo;
        return {};
    }
    case Stage::MountInfo: {
        qsizetype requestedBytes = 1;
        if (m_mountInfo.size() < m_limits.maximumMountInfoBytes) {
            requestedBytes = std::min(m_limits.maximumMountInfoChunkBytes,
                                      m_limits.maximumMountInfoBytes - m_mountInfo.size());
        }
        const RemoteStorageChunkResult chunk = m_reader->readMountInfoChunk(requestedBytes);
        if (chunk.error != RemoteStorageError::None) {
            return handleRequiredError(
                chunk.error,
                QCoreApplication::translate("RemoteStorageScanner",
                                            "Unable to read the remote Linux mount information."));
        }
        if (chunk.data.size() > requestedBytes ||
            chunk.data.size() > m_limits.maximumMountInfoBytes - m_mountInfo.size()) {
            return fail(RemoteStorageScanStatus::Failed,
                        QCoreApplication::translate(
                            "RemoteStorageScanner",
                            "The remote mount information exceeds the safety limit."));
        }
        m_mountInfo.append(chunk.data);
        if (chunk.end) {
            closeMountInfo();
            m_stage = Stage::ParseMountInfo;
        } else if (chunk.data.isEmpty()) {
            return fail(
                RemoteStorageScanStatus::Failed,
                QCoreApplication::translate("RemoteStorageScanner",
                                            "The remote mount information read made no progress."));
        }
        return {};
    }
    case Stage::ParseMountInfo: {
        m_mountInfoFingerprint = QCryptographicHash::hash(m_mountInfo, QCryptographicHash::Sha256);
        m_mounts = rfm::core::parseLinuxMountInfo(m_mountInfo);
        m_mountInfo.clear();
        if (m_mounts.size() > m_limits.maximumMounts) {
            return fail(RemoteStorageScanStatus::Failed,
                        QCoreApplication::translate(
                            "RemoteStorageScanner",
                            "The remote server exposes too many mounts to scan safely."));
        }
        m_volumes.reserve(m_mounts.size());
        m_stage = Stage::BeginLabels;
        return {};
    }
    case Stage::BeginLabels: {
        const RemoteStorageError error = m_reader->beginFileSystemLabels();
        if (connectionError(error)) {
            return fail(RemoteStorageScanStatus::ConnectionLost,
                        QCoreApplication::translate(
                            "RemoteStorageScanner",
                            "The SSH connection was lost while reading filesystem labels."));
        }
        if (cancellationError(error)) {
            return fail(RemoteStorageScanStatus::Cancelled, {});
        }
        if (error == RemoteStorageError::None) {
            m_labelsOpen = true;
            m_stage = Stage::Labels;
        } else {
            m_stage = Stage::BeginMount;
        }
        return {};
    }
    case Stage::Labels: {
        if (m_labelCount >= m_limits.maximumLabels) {
            closeLabels();
            m_stage = Stage::BeginMount;
            return {};
        }
        const RemoteStorageLabelResult label = m_reader->nextFileSystemLabel();
        if (connectionError(label.error)) {
            return fail(RemoteStorageScanStatus::ConnectionLost,
                        QCoreApplication::translate(
                            "RemoteStorageScanner",
                            "The SSH connection was lost while reading filesystem labels."));
        }
        if (cancellationError(label.error)) {
            return fail(RemoteStorageScanStatus::Cancelled, {});
        }
        if (label.error != RemoteStorageError::None || label.end) {
            closeLabels();
            m_stage = Stage::BeginMount;
            return {};
        }
        ++m_labelCount;
        if (!label.label.trimmed().isEmpty() && !label.deviceIdentity.isEmpty()) {
            m_labels.insert(label.deviceIdentity, label.label.trimmed());
        }
        return {};
    }
    case Stage::BeginMount: {
        if (m_mountIndex >= m_mounts.size()) {
            m_stage = Stage::Completed;
            return terminalStep();
        }
        resetCurrentMount();
        m_currentMount = m_mounts.at(m_mountIndex);
        static const QRegularExpression deviceNumberPattern(QStringLiteral("^[0-9]+:[0-9]+$"));
        if (m_currentMount.deviceNumber.startsWith(QStringLiteral("0:")) ||
            !deviceNumberPattern.match(m_currentMount.deviceNumber).hasMatch()) {
            m_stage = Stage::FinishMount;
            return {};
        }

        m_currentBlockDevice = true;
        const QString linkPath = QStringLiteral("/sys/dev/block/") + m_currentMount.deviceNumber;
        const RemoteStorageStringResult target = m_reader->readLink(linkPath);
        if (connectionError(target.error)) {
            return fail(RemoteStorageScanStatus::ConnectionLost,
                        QCoreApplication::translate(
                            "RemoteStorageScanner",
                            "The SSH connection was lost while reading storage topology."));
        }
        if (cancellationError(target.error)) {
            return fail(RemoteStorageScanStatus::Cancelled, {});
        }
        if (target.error != RemoteStorageError::None) {
            m_stage = Stage::FinishMount;
            return {};
        }
        m_currentTopologyPath = resolveRemoteLink(linkPath, target.value);
        if (!m_currentTopologyPath.startsWith(QStringLiteral("/sys/"))) {
            m_stage = Stage::FinishMount;
            return {};
        }
        m_currentVirtualBlockDevice =
            m_currentTopologyPath.startsWith(QStringLiteral("/sys/devices/virtual/"));
        m_stage = Stage::Topology;
        return {};
    }
    case Stage::Topology: {
        if (m_currentTopologyPath == QStringLiteral("/sys")) {
            m_currentTopologyComplete = true;
            m_stage = Stage::FinishMount;
            return {};
        }
        if (!m_currentTopologyPath.startsWith(QStringLiteral("/sys/")) ||
            m_visitedTopologyPaths.contains(m_currentTopologyPath) ||
            m_topologyNodeCount >= m_limits.maximumTopologyNodes) {
            m_stage = Stage::FinishMount;
            return {};
        }
        m_visitedTopologyPaths.insert(m_currentTopologyPath);
        ++m_topologyNodeCount;
        const RemoteStorageTopologyResult topology =
            m_reader->readTopologyNode(m_currentTopologyPath);
        if (connectionError(topology.error)) {
            return fail(RemoteStorageScanStatus::ConnectionLost,
                        QCoreApplication::translate(
                            "RemoteStorageScanner",
                            "The SSH connection was lost while reading storage topology."));
        }
        if (cancellationError(topology.error)) {
            return fail(RemoteStorageScanStatus::Cancelled, {});
        }
        if (topology.error != RemoteStorageError::None) {
            m_stage = Stage::FinishMount;
            return {};
        }
        m_ancestry.push_back(topology.node);
        if (!topology.reliable) {
            m_stage = Stage::FinishMount;
            return {};
        }
        const QString parent = rfm::core::RemotePath::parent(m_currentTopologyPath);
        if (parent == m_currentTopologyPath || parent == QStringLiteral("/")) {
            m_stage = Stage::FinishMount;
            return {};
        }
        m_currentTopologyPath = parent;
        return {};
    }
    case Stage::FinishMount: {
        const rfm::core::StorageDeviceEvidence device = rfm::core::storageDeviceEvidence(
            m_ancestry, m_currentBlockDevice, m_currentVirtualBlockDevice,
            m_currentTopologyComplete);
        RemoteStorageStringResult identity = m_reader->deviceIdentity(m_currentMount.device);
        if (connectionError(identity.error)) {
            return fail(RemoteStorageScanStatus::ConnectionLost,
                        QCoreApplication::translate(
                            "RemoteStorageScanner",
                            "The SSH connection was lost while identifying a storage device."));
        }
        if (cancellationError(identity.error)) {
            return fail(RemoteStorageScanStatus::Cancelled, {});
        }
        if (identity.error != RemoteStorageError::None || identity.value.isEmpty()) {
            identity.value = rfm::core::RemotePath::normalize(m_currentMount.device);
        }

        const RemoteStorageSizeResult size = m_reader->storageSize(m_currentMount.rootPath);
        if (connectionError(size.error)) {
            return fail(RemoteStorageScanStatus::ConnectionLost,
                        QCoreApplication::translate(
                            "RemoteStorageScanner",
                            "The SSH connection was lost while reading storage capacity."));
        }
        if (cancellationError(size.error)) {
            return fail(RemoteStorageScanStatus::Cancelled, {});
        }
        m_volumes.push_back(rfm::core::makeStorageVolume(
            m_currentMount, device, m_labels.value(identity.value),
            size.error == RemoteStorageError::None ? size.bytes : quint64{0}));
        ++m_mountIndex;
        m_stage = Stage::BeginMount;
        return {};
    }
    case Stage::Completed:
    case Stage::Failed:
    case Stage::ConnectionLost:
    case Stage::Cancelled:
        return terminalStep();
    }
    return fail(RemoteStorageScanStatus::Failed,
                QCoreApplication::translate("RemoteStorageScanner",
                                            "Storage discovery entered an invalid state."));
}

void RemoteStorageScanner::cancel()
{
    if (m_stage == Stage::Completed || m_stage == Stage::Failed ||
        m_stage == Stage::ConnectionLost || m_stage == Stage::Cancelled) {
        return;
    }
    closeLabels();
    closeMountInfo();
    m_mountInfo.clear();
    m_volumes.clear();
    m_stage = Stage::Cancelled;
    m_error.clear();
}

QList<rfm::core::StorageVolume> RemoteStorageScanner::takeVolumes()
{
    return std::exchange(m_volumes, {});
}

QByteArray RemoteStorageScanner::mountInfoFingerprint() const { return m_mountInfoFingerprint; }

RemoteStorageScanStep RemoteStorageScanner::terminalStep() const
{
    switch (m_stage) {
    case Stage::Completed:
        return {RemoteStorageScanStatus::Completed, {}};
    case Stage::Failed:
        return {RemoteStorageScanStatus::Failed, m_error};
    case Stage::ConnectionLost:
        return {RemoteStorageScanStatus::ConnectionLost, m_error};
    case Stage::Cancelled:
        return {RemoteStorageScanStatus::Cancelled, {}};
    default:
        return {};
    }
}

RemoteStorageScanStep RemoteStorageScanner::fail(RemoteStorageScanStatus status,
                                                 const QString& error)
{
    closeMountInfo();
    closeLabels();
    m_mountInfo.clear();
    if (status != RemoteStorageScanStatus::Completed) {
        m_volumes.clear();
    }
    m_error = error;
    switch (status) {
    case RemoteStorageScanStatus::Failed:
        m_stage = Stage::Failed;
        break;
    case RemoteStorageScanStatus::ConnectionLost:
        m_stage = Stage::ConnectionLost;
        break;
    case RemoteStorageScanStatus::Cancelled:
        m_stage = Stage::Cancelled;
        break;
    case RemoteStorageScanStatus::Completed:
        m_stage = Stage::Completed;
        break;
    case RemoteStorageScanStatus::Pending:
        m_stage = Stage::Failed;
        break;
    }
    return terminalStep();
}

void RemoteStorageScanner::closeMountInfo()
{
    if (m_mountInfoOpen && m_reader != nullptr) {
        m_reader->endMountInfo();
    }
    m_mountInfoOpen = false;
}

RemoteStorageScanStep RemoteStorageScanner::handleRequiredError(RemoteStorageError error,
                                                                const QString& context)
{
    if (connectionError(error)) {
        return fail(
            RemoteStorageScanStatus::ConnectionLost,
            QCoreApplication::translate("RemoteStorageScanner",
                                        "The SSH connection was lost during storage discovery."));
    }
    if (cancellationError(error)) {
        return fail(RemoteStorageScanStatus::Cancelled, {});
    }
    return fail(RemoteStorageScanStatus::Failed, context);
}

void RemoteStorageScanner::closeLabels()
{
    if (m_labelsOpen && m_reader != nullptr) {
        m_reader->endFileSystemLabels();
    }
    m_labelsOpen = false;
}

void RemoteStorageScanner::resetCurrentMount()
{
    m_currentMount = {};
    m_ancestry.clear();
    m_visitedTopologyPaths.clear();
    m_currentTopologyPath.clear();
    m_currentBlockDevice = false;
    m_currentVirtualBlockDevice = false;
    m_currentTopologyComplete = false;
}

} // namespace rfm::ssh
