#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/RemoteFilesystem.hpp"
#include "remotefilemanager/core/RemoteMoveSafety.hpp"
#include "remotefilemanager/core/RemotePath.hpp"
#include "remotefilemanager/core/ServerSideCopyJob.hpp"
#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include "../src/ssh/RemoteDelete.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <optional>
#include <utility>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace
{

QByteArray encodeMountInfoPath(const QString& path)
{
    QByteArray encoded = path.toUtf8();
    encoded.replace("\\", "\\134");
    encoded.replace(" ", "\\040");
    encoded.replace("\t", "\\011");
    encoded.replace("\n", "\\012");
    return encoded;
}

} // namespace

class FakeRemoteBackend final : public rfm::core::RemoteFileBackend
{
  public:
    rfm::core::RemoteProbeResult probe(const QString& path) override
    {
        calls.push_back(QStringLiteral("probe:%1").arg(path));
        if (forced.contains(QStringLiteral("probe:%1").arg(path))) {
            return {forced.value(QStringLiteral("probe:%1").arg(path)), {}};
        }
        if (!nodes.contains(path)) {
            return {{rfm::core::RemoteBackendError::NotFound, {}}, {}};
        }
        return {{}, {true, nodes.value(path)}};
    }

    rfm::core::RemoteDirectoryResult list(const QString& path) override
    {
        calls.push_back(QStringLiteral("list:%1").arg(path));
        if (forced.contains(QStringLiteral("list:%1").arg(path))) {
            return {forced.value(QStringLiteral("list:%1").arg(path)), {}};
        }
        return {{}, listings.value(path)};
    }

    rfm::core::RemoteBackendResult createDirectory(const QString& path) override
    {
        return mutate(QStringLiteral("mkdir"), path, true);
    }

    rfm::core::RemoteBackendResult rename(const QString& source,
                                          const QString& destination) override
    {
        calls.push_back(QStringLiteral("rename:%1:%2").arg(source, destination));
        const auto result = forced.value(QStringLiteral("rename:%1").arg(source));
        if (!result.succeeded()) {
            return result;
        }
        nodes.remove(source);
        nodes.insert(destination, false);
        return {};
    }

    rfm::core::RemoteBackendResult removeFile(const QString& path) override
    {
        return mutate(QStringLiteral("unlink"), path, false);
    }

    rfm::core::RemoteBackendResult removeDirectory(const QString& path) override
    {
        return mutate(QStringLiteral("rmdir"), path, false);
    }

    rfm::core::RemoteBackendResult copyOnServer(const QString& source, const QString& destination,
                                                bool recursive) override
    {
        calls.push_back(
            QStringLiteral("copy:%1:%2:%3")
                .arg(source, destination, recursive ? QStringLiteral("r") : QStringLiteral("f")));
        const auto result = forced.value(QStringLiteral("copy:%1").arg(source));
        if (result.succeeded()) {
            nodes.insert(destination, recursive);
        }
        return result;
    }

    rfm::core::RemoteBackendResult mutate(const QString& operation, const QString& path,
                                          bool directory)
    {
        calls.push_back(QStringLiteral("%1:%2").arg(operation, path));
        const auto result = forced.value(QStringLiteral("%1:%2").arg(operation, path));
        if (result.succeeded()) {
            if (operation == QStringLiteral("mkdir")) {
                nodes.insert(path, directory);
            } else {
                nodes.remove(path);
            }
        }
        return result;
    }

    QHash<QString, bool> nodes;
    QHash<QString, QList<QPair<QString, bool>>> listings;
    QHash<QString, rfm::core::RemoteBackendResult> forced;
    QStringList calls;
};

class FakeCopyBackend final : public rfm::core::ServerSideCopyBackend
{
  public:
    rfm::core::RemoteProbeResult probe(const QString& path) override
    {
        probed.push_back(path);
        if (!probeResponses.isEmpty()) {
            return probeResponses.takeFirst();
        }
        if (existingPaths.contains(path)) {
            return {{}, {true, false}};
        }
        return {{rfm::core::RemoteBackendError::NotFound, {}}, {}};
    }

    rfm::core::RemoteBackendResult rename(const QString& source,
                                          const QString& destination) override
    {
        renamed.push_back(QStringLiteral("%1:%2").arg(source, destination));
        const bool promotion = source.endsWith(QStringLiteral("/item")) &&
                               (source.contains(QStringLiteral(".rfm-copy-")) ||
                                source.contains(QStringLiteral(".rfm-move-")));
        const rfm::core::RemoteBackendResult result = promotion ? promotionResult : renameResult;
        if (!promotion && result.succeeded()) {
            sourceRemoved = true;
        }
        if (promotion && result.succeeded()) {
            existingPaths.remove(source);
            existingPaths.insert(destination);
        }
        return result;
    }

    rfm::core::RemoteBackendResult startCopy(const QString& source, const QString& destination,
                                             bool recursive) override
    {
        started.push_back(
            QStringLiteral("%1:%2:%3")
                .arg(source, destination,
                     recursive ? QStringLiteral("recursive") : QStringLiteral("file")));
        active = true;
        activeCopyDestination = destination;
        return {};
    }

    rfm::core::RemoteBackendResult reserveStaging(const QString& path) override
    {
        reserved.push_back(path);
        if (reserveResult.succeeded()) {
            stagingExists = true;
        }
        return reserveResult;
    }

    rfm::core::RemoteBackendResult removeEmptyDirectory(const QString& path) override
    {
        emptyDirectoryRemovals.push_back(path);
        if (stagingContainsUnexpectedEntry) {
            return {rfm::core::RemoteBackendError::Failure,
                    QStringLiteral("SFTP rmdir failed: staging directory is not empty")};
        }
        if (emptyDirectoryRemoveResult.succeeded()) {
            stagingExists = false;
        }
        return emptyDirectoryRemoveResult;
    }

    rfm::core::RemoteBackendResult startMoveStagingCopy(const QString& source,
                                                        const QString& destination) override
    {
        stagedCopies.push_back(QStringLiteral("%1:%2:archive").arg(source, destination));
        active = moveCopyStartResult.succeeded();
        activeCopyDestination = destination;
        return moveCopyStartResult;
    }

    std::optional<rfm::core::RemoteBackendResult> pollCopy() override
    {
        ++pollCalls;
        if (completeAfterPolls > 0 && pollCalls >= completeAfterPolls) {
            active = false;
            if (copyCompletionResult.succeeded()) {
                existingPaths.insert(activeCopyDestination);
            }
            return copyCompletionResult;
        }
        return std::nullopt;
    }

    rfm::core::RemoteBackendResult startRemove(const QString& path, bool recursive,
                                               bool protectMountPoint) override
    {
        activeRemovePath = path;
        removed.push_back(QStringLiteral("%1:%2:%3")
                              .arg(path,
                                   recursive ? QStringLiteral("recursive") : QStringLiteral("file"),
                                   protectMountPoint ? QStringLiteral("protected")
                                                     : QStringLiteral("unprotected")));
        const rfm::core::RemoteBackendResult result =
            path.contains(QStringLiteral(".rfm-copy-")) ||
                    path.contains(QStringLiteral(".rfm-move-"))
                ? cleanupStartResult
                : removeStartResult;
        active = result.succeeded();
        return result;
    }

    std::optional<rfm::core::RemoteBackendResult> pollRemove() override
    {
        ++removePollCalls;
        if (removeCompleteAfterPolls > 0 && removePollCalls >= removeCompleteAfterPolls) {
            active = false;
            const bool staging = activeRemovePath.contains(QStringLiteral(".rfm-copy-")) ||
                                 activeRemovePath.contains(QStringLiteral(".rfm-move-"));
            const rfm::core::RemoteBackendResult result =
                staging ? cleanupCompletionResult : removeCompletionResult;
            if (result.succeeded()) {
                if (staging) {
                    stagingExists = false;
                    existingPaths.remove(activeRemovePath + QStringLiteral("/item"));
                } else {
                    sourceRemoved = true;
                }
            }
            return result;
        }
        return std::nullopt;
    }

    std::optional<rfm::core::RemoteBackendResult> requestCopyCancellation() override
    {
        ++cancellationRequestCalls;
        if (cancellationRequestResponses.isEmpty()) {
            return std::nullopt;
        }
        const std::optional<rfm::core::RemoteBackendResult> response =
            cancellationRequestResponses.takeFirst();
        if (response.has_value() && !response->succeeded()) {
            active = false;
            ++cleanupCalls;
        }
        return response;
    }

    std::optional<rfm::core::RemoteBackendResult> pollCopyCancellation() override
    {
        ++cancellationPollCalls;
        if (cancellationPollResponses.isEmpty()) {
            return std::nullopt;
        }
        const std::optional<rfm::core::RemoteBackendResult> response =
            cancellationPollResponses.takeFirst();
        if (response.has_value()) {
            active = false;
            ++cleanupCalls;
        }
        return response;
    }

    QStringList probed;
    QStringList renamed;
    QStringList started;
    QStringList reserved;
    QStringList stagedCopies;
    QStringList removed;
    QStringList emptyDirectoryRemovals;
    QString activeRemovePath;
    QString activeCopyDestination;
    QSet<QString> existingPaths;
    QList<rfm::core::RemoteProbeResult> probeResponses;
    int pollCalls{0};
    int cancellationRequestCalls{0};
    int cancellationPollCalls{0};
    int cleanupCalls{0};
    int completeAfterPolls{0};
    int removePollCalls{0};
    int removeCompleteAfterPolls{1};
    bool active{false};
    bool sourceRemoved{false};
    bool stagingExists{false};
    bool stagingContainsUnexpectedEntry{false};
    rfm::core::RemoteBackendResult renameResult;
    rfm::core::RemoteBackendResult promotionResult;
    rfm::core::RemoteBackendResult reserveResult;
    rfm::core::RemoteBackendResult emptyDirectoryRemoveResult;
    rfm::core::RemoteBackendResult moveCopyStartResult;
    rfm::core::RemoteBackendResult copyCompletionResult;
    rfm::core::RemoteBackendResult cleanupStartResult;
    rfm::core::RemoteBackendResult cleanupCompletionResult;
    rfm::core::RemoteBackendResult removeStartResult;
    rfm::core::RemoteBackendResult removeCompletionResult;
    QList<std::optional<rfm::core::RemoteBackendResult>> cancellationRequestResponses;
    QList<std::optional<rfm::core::RemoteBackendResult>> cancellationPollResponses;
};

class LocalSymlinkStagingBackend final : public rfm::core::ServerSideCopyBackend
{
  public:
    explicit LocalSymlinkStagingBackend(bool moveFallback) : m_moveFallback(moveFallback) {}

    rfm::core::RemoteProbeResult probe(const QString& path) override
    {
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink()) {
            return {{rfm::core::RemoteBackendError::NotFound, {}}, {}};
        }
        return {{}, {true, info.isDir()}};
    }

    rfm::core::RemoteBackendResult rename(const QString& source,
                                          const QString& destination) override
    {
        ++renameCalls;
        if (m_moveFallback && renameCalls == 1) {
            return {rfm::core::RemoteBackendError::CrossDevice, {}};
        }
        promotionObserved = QFileInfo(source).isSymLink() && QFile::rename(source, destination);
        return promotionObserved
                   ? rfm::core::RemoteBackendResult{}
                   : rfm::core::RemoteBackendResult{rfm::core::RemoteBackendError::Failure,
                                                    QStringLiteral("Local promotion failed")};
    }

    rfm::core::RemoteBackendResult startCopy(const QString& source, const QString& destination,
                                             bool recursive) override
    {
        copyStartedAfterReservation = stagingReserved && QFileInfo(stagingPath).isDir();
        copiedSourceWasSymlink = QFileInfo(source).isSymLink();
        return runCopy(rfm::ssh::RemoteCopyCommand::build(source, destination, recursive),
                       destination, false);
    }

    rfm::core::RemoteBackendResult reserveStaging(const QString& path) override
    {
        stagingPath = path;
        const QString parent = rfm::core::RemotePath::parent(path);
        const QString name = rfm::core::RemotePath::fileName(path);
        stagingReserved = QDir(parent).mkdir(name);
        if (!stagingReserved) {
            return {rfm::core::RemoteBackendError::AlreadyExists, {}};
        }
        static_cast<void>(QFile::setPermissions(
            path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        return {};
    }

    rfm::core::RemoteBackendResult removeEmptyDirectory(const QString& path) override
    {
        emptyStagingRemovalUsed = true;
        const QString parent = rfm::core::RemotePath::parent(path);
        const QString name = rfm::core::RemotePath::fileName(path);
        return QDir(parent).rmdir(name)
                   ? rfm::core::RemoteBackendResult{}
                   : rfm::core::RemoteBackendResult{
                         rfm::core::RemoteBackendError::Failure,
                         QStringLiteral("Local empty staging directory removal failed")};
    }

    rfm::core::RemoteBackendResult startMoveStagingCopy(const QString& source,
                                                        const QString& destination) override
    {
        copyStartedAfterReservation = stagingReserved && QFileInfo(stagingPath).isDir();
        copiedSourceWasSymlink = QFileInfo(source).isSymLink();
        return runCopy(rfm::ssh::RemoteCopyCommand::buildMoveStaging(source, destination),
                       destination, true);
    }

    rfm::core::RemoteBackendResult runCopy(const QString& command, const QString& destination,
                                           bool reportsMoveStatus)
    {
        QProcess process;
        process.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
        if (!process.waitForStarted() || !process.waitForFinished()) {
            copyResult = {rfm::core::RemoteBackendError::Failure,
                          QStringLiteral("Unable to run local cp")};
            return {};
        }
        const QByteArray standardOutput = process.readAllStandardOutput();
        const QByteArray standardError = process.readAllStandardError();
        if (reportsMoveStatus) {
            reportedCopyStatus = rfm::ssh::RemoteCopyCommand::parseCopyStatus(standardOutput);
        }
        stagedLinkTarget = QFile::symLinkTarget(destination);
        copyResult =
            process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 &&
                    (!reportsMoveStatus || reportedCopyStatus == 0)
                ? rfm::core::RemoteBackendResult{}
                : rfm::core::RemoteBackendResult{rfm::core::RemoteBackendError::Failure,
                                                 QString::fromUtf8(standardError).trimmed()};
        return {};
    }

    std::optional<rfm::core::RemoteBackendResult> pollCopy() override
    {
        return std::exchange(copyResult, std::nullopt);
    }

    rfm::core::RemoteBackendResult startRemove(const QString& path, bool recursive,
                                               bool protectMountPoint) override
    {
        if (path == stagingPath) {
            stagingRemovalProtected = recursive && protectMountPoint;
            removeResult = QDir(path).removeRecursively()
                               ? rfm::core::RemoteBackendResult{}
                               : rfm::core::RemoteBackendResult{
                                     rfm::core::RemoteBackendError::Failure,
                                     QStringLiteral("Local staging cleanup failed")};
        } else {
            sourceRemovalObservedAfterPromotion = promotionObserved && QFileInfo(path).isSymLink();
            removeResult =
                QFile::remove(path)
                    ? rfm::core::RemoteBackendResult{}
                    : rfm::core::RemoteBackendResult{rfm::core::RemoteBackendError::Failure,
                                                     QStringLiteral("Local source removal failed")};
        }
        return {};
    }

    std::optional<rfm::core::RemoteBackendResult> pollRemove() override
    {
        return std::exchange(removeResult, std::nullopt);
    }

    std::optional<rfm::core::RemoteBackendResult> requestCopyCancellation() override
    {
        return rfm::core::RemoteBackendResult{};
    }

    std::optional<rfm::core::RemoteBackendResult> pollCopyCancellation() override
    {
        return rfm::core::RemoteBackendResult{};
    }

    QString stagingPath;
    QString stagedLinkTarget;
    std::optional<quint32> reportedCopyStatus;
    std::optional<rfm::core::RemoteBackendResult> copyResult;
    std::optional<rfm::core::RemoteBackendResult> removeResult;
    int renameCalls{0};
    bool stagingReserved{false};
    bool copyStartedAfterReservation{false};
    bool copiedSourceWasSymlink{false};
    bool promotionObserved{false};
    bool emptyStagingRemovalUsed{false};
    bool stagingRemovalProtected{false};
    bool sourceRemovalObservedAfterPromotion{false};

  private:
    bool m_moveFallback{false};
};

class RemoteFileOperationsTest final : public QObject
{
    Q_OBJECT

  private slots:
    void validatesAndNormalizesRemotePaths();
    void quotesCopyCommandWithoutInjection();
    void recursiveRemoveRefusesMountPointTrees_data();
    void recursiveRemoveRefusesMountPointTrees();
    void qualifiesCrossDeviceFallbackWithoutFollowingSourceEntries();
    void createsDirectoryAndReportsCollisionOrPermission();
    void renamesWithoutOverwriting();
    void movesSelectionAndReportsPartialFailure();
    void copiesOnServerOrReportsUnsupported();
    void serverSideCopyPollingPreservesSharedSessionBlockingMode();
    void remoteCopyBackendRoutesNativeAndClientMediatedPaths();
    void remoteCopyStagingCleanupIsSftpBounded();
    void remoteCopyHandlesSymlinksConservatively();
    void copyStatusProtocolIsDeterministic();
    void copyStatusWrapperForwardsTermination();
    void activeStagingOwnershipIsExactAndTemporary();
    void stagingNameIsIndependentOfFinalBasename_data();
    void stagingNameIsIndependentOfFinalBasename();
    void transportFailurePreservesCompletedItems();
    void transportFailureBeforeFirstSuccess();
    void serverSideCopyRunsCooperatively_data();
    void serverSideCopyRunsCooperatively();
    void serverSideCopyCancellationWaitsForTermination();
    void serverSideCopyCancellationRetriesAndReportsFailure();
    void serverSideCopyCancellationKeepsShutdownResponsive();
    void serverSideCopyFinalCompletionWinsOverCancellation();
    void serverSideCopyCanCancelWithSourcesRemaining();
    void serverSideCopyFailureCleansStaging();
    void serverSideCopyCleanupFailureIsTerminal();
    void serverSideCopyPostPromotionRmdirFailureIsTerminal();
    void serverSideCopyNonEmptyPostPromotionStagingFailsClosed();
    void serverSideCopyLateCollisionPreservesCompetitor();
    void copiesValidAndBrokenSymlinksThroughReservedStaging_data();
    void copiesValidAndBrokenSymlinksThroughReservedStaging();
    void serverSideCopyNoClobberAndRetryAfterCancellation();
    void movesOnSameFileSystemWithRename();
    void fallsBackToCopyThenDeleteAcrossFileSystems();
    void movesValidAndBrokenSymlinksThroughReservedStaging_data();
    void movesValidAndBrokenSymlinksThroughReservedStaging();
    void lateStagingCollisionPreservesSource();
    void keepsSourceWhenCrossFileSystemCopyFails();
    void reportsFailedStagingCleanupWithoutMaskingCopyError();
    void cancellationCleansMoveStagingAndPreservesSource();
    void keepsSourceWhenFallbackPromotionFails();
    void reportsSourceCleanupFailureAfterCopy();
    void preservesPromotedSourceWhenMountTreeGuardFails();
    void doesNotFallbackForAnUnqualifiedRenameFailure();
    void classifiesRemoteFilesystemIdsConservatively();
    void removesFileAndRecursiveTreeWithGuards();
    void refusesSelectedAndNestedRemoteMountPoints();
    void preflightsAllRemoteSourcesBeforeRemoval();
    void detectsBindMountAndMalformedMountInfo();
    void acceptsFileSystemSpecificMountRoots();
    void doesNotTraverseRemoteSymlinksAndRemovesOrdinaryTrees();
    void emitsWorkerOperationErrors();
    void treatsListingWithoutSessionAsFatal();
};

void RemoteFileOperationsTest::validatesAndNormalizesRemotePaths()
{
    QVERIFY(rfm::core::RemotePath::isValidName(QStringLiteral("notes.txt")));
    QVERIFY(!rfm::core::RemotePath::isValidName({}));
    QVERIFY(!rfm::core::RemotePath::isValidName(QStringLiteral(".")));
    QVERIFY(!rfm::core::RemotePath::isValidName(QStringLiteral("..")));
    QVERIFY(!rfm::core::RemotePath::isValidName(QStringLiteral("a/b")));
    QCOMPARE(rfm::core::RemotePath::normalize(QStringLiteral("/srv//data/../files")),
             QStringLiteral("/srv/files"));
    QCOMPARE(rfm::core::RemotePath::normalize(QStringLiteral("rfm-sprint4/./dossier-test/")),
             QStringLiteral("rfm-sprint4/dossier-test"));
    QCOMPARE(rfm::core::RemotePath::join(QStringLiteral("rfm-sprint4/./"),
                                         QStringLiteral("fichier.txt")),
             QStringLiteral("rfm-sprint4/fichier.txt"));
    QCOMPARE(rfm::core::RemotePath::join(QStringLiteral("/home/gabriel/rfm-sprint4/"),
                                         QStringLiteral("fichier.txt")),
             QStringLiteral("/home/gabriel/rfm-sprint4/fichier.txt"));
    QCOMPARE(rfm::core::RemotePath::join(QStringLiteral("."), QStringLiteral("folder")),
             QStringLiteral("./folder"));
    QCOMPARE(rfm::core::RemotePath::parent(QStringLiteral("./folder/child")),
             QStringLiteral("folder"));
    QVERIFY(rfm::core::RemotePath::isProtected(QStringLiteral("/")));
    QVERIFY(rfm::core::RemotePath::isProtected(QStringLiteral("../outside")));
}

void RemoteFileOperationsTest::quotesCopyCommandWithoutInjection()
{
    const QString copy = rfm::ssh::RemoteCopyCommand::build(
        QStringLiteral("./a'; touch /tmp/pwned; '"), QStringLiteral("./target/file"), false);
    QVERIFY(copy.contains(
        QStringLiteral("cp -P -p './a'\\''; touch /tmp/pwned; '\\''' './target/file'")));
    const QString recursiveCopy = rfm::ssh::RemoteCopyCommand::build(
        QStringLiteral("./folder"), QStringLiteral("/backup/folder"), true);
    QVERIFY(recursiveCopy.contains(QStringLiteral("cp -P -p -R './folder' '/backup/folder'")));
    const QString unusualCopy = rfm::ssh::RemoteCopyCommand::build(
        QStringLiteral("/srv/- source \"été\""), QStringLiteral("/backup/O'Brien;$(id)"), false);
    QVERIFY(unusualCopy.contains(
        QStringLiteral("cp -P -p '/srv/- source \"été\"' '/backup/O'\\''Brien;$(id)'")));
    const QString leadingHyphenCopy = rfm::ssh::RemoteCopyCommand::build(
        QStringLiteral("-source"), QStringLiteral("-destination"), false);
    QVERIFY(leadingHyphenCopy.contains(QStringLiteral("cp -P -p './-source' './-destination'")));
    for (const QString& command : {copy, recursiveCopy}) {
        QVERIFY(command.contains(QStringLiteral("trap 'rfm_forward_term' TERM HUP INT")));
        QVERIFY(command.contains(QStringLiteral("kill -TERM \"$rfm_copy_pid\"")));
        QVERIFY(command.contains(QStringLiteral("wait \"$rfm_copy_pid\"")));
        QVERIFY(command.contains(QStringLiteral("RFM_COPY_STATUS:%s")));
    }
    QCOMPARE(
        rfm::ssh::RemoteCopyCommand::buildRemove(QStringLiteral("./a'; touch /tmp/pwned; '"), true),
        QStringLiteral("rm -R -f -- './a'\\''; touch /tmp/pwned; '\\'''"));
    const QString moveCopy = rfm::ssh::RemoteCopyCommand::buildMoveStaging(
        QStringLiteral("./a'; touch /tmp/pwned; '"), QStringLiteral("./stage/item"));
    QVERIFY(moveCopy.contains(
        QStringLiteral("cp -a -- './a'\\''; touch /tmp/pwned; '\\''' './stage/item'")));
    QVERIFY(moveCopy.contains(QStringLiteral("trap 'rfm_forward_term' TERM HUP INT")));
    QVERIFY(moveCopy.contains(QStringLiteral("RFM_COPY_STATUS:%s")));
    QCOMPARE(
        rfm::ssh::RemoteCopyCommand::parseCopyStatus(QByteArrayLiteral("\nRFM_COPY_STATUS:0\n")),
        std::optional<quint32>{0});
    QCOMPARE(rfm::ssh::RemoteCopyCommand::parseCopyStatus(
                 QByteArrayLiteral("diagnostic\nRFM_COPY_STATUS:23\n")),
             std::optional<quint32>{23});
    QVERIFY(!rfm::ssh::RemoteCopyCommand::parseCopyStatus(QByteArrayLiteral("RFM_COPY_STATUS:0"))
                 .has_value());
    QVERIFY(!rfm::ssh::RemoteCopyCommand::parseCopyStatus(
                 QByteArrayLiteral("spoof-RFM_COPY_STATUS:0\n"))
                 .has_value());
    QVERIFY(
        !rfm::ssh::RemoteCopyCommand::parseCopyStatus(QByteArrayLiteral("RFM_COPY_STATUS:256\n"))
             .has_value());
    const QString guardedRemove = rfm::ssh::RemoteCopyCommand::buildRemove(
        QStringLiteral("/mnt/My Disk's"), true, true, QStringLiteral("/mnt/My Disk's"));
    QVERIFY(guardedRemove.contains(QStringLiteral("/proc/self/mountinfo")));
    QVERIFY(guardedRemove.contains(QStringLiteral("rfm_mountinfo_seen")));
    QVERIFY(guardedRemove.contains(QStringLiteral("rfm_mount_in_tree")));
    QVERIFY(guardedRemove.contains(QStringLiteral("rfm_device")));
    QVERIFY(guardedRemove.contains(QStringLiteral("rfm_take_mount_field")));
    QVERIFY(guardedRemove.contains(QStringLiteral("rfm_valid_optional_field")));
    QVERIFY(guardedRemove.contains(QStringLiteral("[ -z \"$rfm_mount_record\" ]")));
    QVERIFY(!guardedRemove.contains(QStringLiteral("set -- $rfm_mount_record")));
    QVERIFY(guardedRemove.contains(QStringLiteral("*[!0-9]*")));
    QVERIFY(guardedRemove.contains(QStringLiteral("/mnt/My\\040Disk")));
    QVERIFY(guardedRemove.contains(QStringLiteral("'\\''s")));
    QVERIFY(guardedRemove.contains(QStringLiteral("|'/mnt/My\\040Disk'\\''s'/*")));
    QVERIFY(guardedRemove.endsWith(
        QStringLiteral("rm -R --one-file-system -f -- '/mnt/My Disk'\\''s'")));
}

void RemoteFileOperationsTest::recursiveRemoveRefusesMountPointTrees_data()
{
    QTest::addColumn<QString>("relation");
    QTest::addColumn<bool>("specialPath");
    QTest::addColumn<int>("expectedExitCode");
    QTest::addColumn<QByteArray>("invalidRecord");
    QTest::addColumn<bool>("terminateRecord");

    QTest::newRow("source-is-mountpoint")
        << QStringLiteral("root") << false << 75 << QByteArray{} << true;
    QTest::newRow("nested-mountpoint")
        << QStringLiteral("nested") << false << 75 << QByteArray{} << true;
    QTest::newRow("nested-bind-mount-same-filesystem")
        << QStringLiteral("bind") << false << 75 << QByteArray{} << true;
    QTest::newRow("false-prefix") << QStringLiteral("false-prefix") << false << 0 << QByteArray{}
                                  << true;
    QTest::newRow("special-nested-mountpoint")
        << QStringLiteral("nested") << true << 75 << QByteArray{} << true;
    QTest::newRow("escaped-space-descendant")
        << QStringLiteral("escaped-space") << false << 75 << QByteArray{} << true;
    QTest::newRow("unterminated-final-record")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("25 24 8:1 / /outside rw - ext4 /dev/sda1 rw") << false;
    QTest::newRow("raw-space-in-mountpoint")
        << QStringLiteral("raw-space") << false << 74 << QByteArrayLiteral("GENERATE") << true;
    QTest::newRow("raw-tab-in-mountpoint")
        << QStringLiteral("raw-tab") << false << 74 << QByteArrayLiteral("GENERATE") << true;
    QTest::newRow("review-incomplete-record") << QStringLiteral("invalid") << false << 74
                                              << QByteArrayLiteral("24 1 bogus bogus / rw") << true;
    QTest::newRow("invalid-mount-id")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("x 1 8:1 / / rw - ext4 /dev/sda1 rw") << true;
    QTest::newRow("invalid-parent-id")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 x 8:1 / / rw - ext4 /dev/sda1 rw") << true;
    QTest::newRow("missing-separator")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:1 / / rw ext4 /dev/sda1 rw") << true;
    QTest::newRow("invalid-major-minor")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:x / / rw - ext4 /dev/sda1 rw") << true;
    QTest::newRow("missing-post-separator-field")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:1 / / rw - ext4 /dev/sda1") << true;
    QTest::newRow("relative-mountpoint")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:1 / relative rw - ext4 /dev/sda1 rw") << true;
    QTest::newRow("relative-root")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:1 relative / rw - ext4 /dev/sda1 rw") << true;
    QTest::newRow("invalid-path-escape")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:1 / /bad\\999 rw - ext4 /dev/sda1 rw") << true;
}

void RemoteFileOperationsTest::recursiveRemoveRefusesMountPointTrees()
{
    QFETCH(QString, relation);
    QFETCH(bool, specialPath);
    QFETCH(int, expectedExitCode);
    QFETCH(QByteArray, invalidRecord);
    QFETCH(bool, terminateRecord);

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString parent = temporaryDirectory.path() + QStringLiteral("/srv");
    const bool spacePath =
        relation == QStringLiteral("escaped-space") || relation == QStringLiteral("raw-space");
    const QString source = parent + (spacePath     ? QStringLiteral("/data dir")
                                     : specialPath ? QStringLiteral("/data dir's [tree]")
                                                   : QStringLiteral("/data"));
    QVERIFY(QDir().mkpath(source));
    QFile normalFile(source + QStringLiteral("/file.txt"));
    QVERIFY(normalFile.open(QIODevice::WriteOnly));
    QCOMPARE(normalFile.write("SOURCE\n"), qint64{7});
    normalFile.close();

    QString mountPoint;
    if (relation == QStringLiteral("root")) {
        mountPoint = source;
    } else if (relation == QStringLiteral("false-prefix")) {
        mountPoint = parent + QStringLiteral("/database");
    } else {
        mountPoint =
            source + (specialPath ? QStringLiteral("/nested mount's [bind]")
                      : relation == QStringLiteral("bind") ? QStringLiteral("/nested-bind")
                                                           : QStringLiteral("/nested-mount"));
    }
    QVERIFY(QDir().mkpath(mountPoint));
    QFile importantFile(mountPoint + QStringLiteral("/IMPORTANT.txt"));
    QVERIFY(importantFile.open(QIODevice::WriteOnly));
    QCOMPARE(importantFile.write("DO NOT DELETE\n"), qint64{14});
    importantFile.close();

    const QString mountInfoPath = temporaryDirectory.path() + QStringLiteral("/mount info's");
    QFile mountInfo(mountInfoPath);
    QVERIFY(mountInfo.open(QIODevice::WriteOnly));
    const QByteArray rootMountRecord = QByteArrayLiteral("24 1 8:1 / / rw - ext4 /dev/sda1 rw\n");
    QCOMPARE(mountInfo.write(rootMountRecord), qint64{rootMountRecord.size()});
    if (invalidRecord == QByteArrayLiteral("GENERATE")) {
        const QByteArray rawSuffix = relation == QStringLiteral("raw-tab")
                                         ? QByteArrayLiteral("\t/nested")
                                         : QByteArrayLiteral("/nested");
        const QByteArray rawMountPoint = source.toUtf8() + rawSuffix;
        invalidRecord = QByteArrayLiteral("25 24 8:1 / ") + rawMountPoint +
                        QByteArrayLiteral(" rw shared:7 - ext4 /dev/sda1 rw,bind");
    }
    if (!invalidRecord.isEmpty()) {
        if (terminateRecord) {
            invalidRecord.push_back('\n');
        }
        QCOMPARE(mountInfo.write(invalidRecord), qint64{invalidRecord.size()});
    } else {
        const QByteArray mountRecord =
            QByteArrayLiteral("25 24 ") +
            (relation == QStringLiteral("bind") ? QByteArrayLiteral("8:1")
                                                : QByteArrayLiteral("8:2")) +
            QByteArrayLiteral(" / ") + encodeMountInfoPath(mountPoint) +
            (relation == QStringLiteral("bind")
                 ? QByteArrayLiteral(" rw shared:7 master:1 - ext4 /dev/sda1 "
                                     "rw,bind\n")
                 : QByteArrayLiteral(" rw - ext4 /dev/sdb1 rw\n"));
        QCOMPARE(mountInfo.write(mountRecord), qint64{mountRecord.size()});
    }
    mountInfo.close();

    const QString command =
        rfm::ssh::RemoteCopyCommand::buildRemove(source, true, true, source, mountInfoPath);
    QVERIFY(!command.isEmpty());
    QProcess process;
    process.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished());
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), expectedExitCode);

    if (expectedExitCode == 0) {
        QVERIFY(!QFileInfo(source).exists());
        QVERIFY(QFileInfo(mountPoint + QStringLiteral("/IMPORTANT.txt")).exists());
    } else {
        QVERIFY(QFileInfo(source + QStringLiteral("/file.txt")).exists());
        QVERIFY(QFileInfo(mountPoint + QStringLiteral("/IMPORTANT.txt")).exists());
        const QByteArray error = process.readAllStandardError();
        if (expectedExitCode == 75) {
            QVERIFY(error.contains("mount point exists in the removal tree"));
        } else {
            QVERIFY(error.contains("Invalid remote mount information"));
        }
    }
}

void RemoteFileOperationsTest::qualifiesCrossDeviceFallbackWithoutFollowingSourceEntries()
{
    QCOMPARE(rfm::core::remoteMoveFileSystemContainer(QStringLiteral("/fs-a/file.txt")),
             QStringLiteral("/fs-a"));
    QCOMPARE(rfm::core::remoteMoveFileSystemContainer(QStringLiteral("/fs-a/folder")),
             QStringLiteral("/fs-a"));
    QCOMPARE(rfm::core::remoteMoveFileSystemContainer(QStringLiteral("/fs-a/symlink-to-fs-b")),
             QStringLiteral("/fs-a"));
    QCOMPARE(rfm::core::remoteMoveFileSystemContainer(QStringLiteral("/fs-a/broken-symlink")),
             QStringLiteral("/fs-a"));
    QVERIFY(rfm::core::remoteMoveFileSystemContainer(QStringLiteral("relative/file")).isEmpty());

    const QByteArray mountInfo = "24 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
                                 "25 24 8:17 / /mnt/source rw - ext4 /dev/sdb1 rw\n"
                                 "26 24 8:18 / /mnt/My\\040Disk rw - ext4 /dev/sdc1 rw\n";
    QCOMPARE(rfm::core::linuxMountPointState(mountInfo, QStringLiteral("/mnt/source")),
             rfm::core::RemoteMountPointState::MountPoint);
    QCOMPARE(rfm::core::linuxMountPointState(mountInfo, QStringLiteral("/mnt/My Disk")),
             rfm::core::RemoteMountPointState::MountPoint);
    QCOMPARE(rfm::core::linuxMountPointState(mountInfo, QStringLiteral("/fs-a/file.txt")),
             rfm::core::RemoteMountPointState::NotMountPoint);
    QCOMPARE(rfm::core::linuxMountPointState({}, QStringLiteral("/fs-a/file.txt")),
             rfm::core::RemoteMountPointState::Unknown);

    const rfm::core::RemoteMoveFallbackEvidence normal{
        quint64{11}, quint64{22}, rfm::core::RemoteMountPointState::NotMountPoint};
    QVERIFY(rfm::core::allowsRemoteMoveFallback(normal));
    auto mountPoint = normal;
    mountPoint.sourceMountPoint = rfm::core::RemoteMountPointState::MountPoint;
    QVERIFY(!rfm::core::allowsRemoteMoveFallback(mountPoint));
    auto ambiguous = normal;
    ambiguous.sourceMountPoint = rfm::core::RemoteMountPointState::Unknown;
    QVERIFY(!rfm::core::allowsRemoteMoveFallback(ambiguous));
    auto sameFileSystem = normal;
    sameFileSystem.destinationContainerFileSystem = quint64{11};
    QVERIFY(!rfm::core::allowsRemoteMoveFallback(sameFileSystem));
    auto missingFileSystem = normal;
    missingFileSystem.sourceContainerFileSystem.reset();
    QVERIFY(!rfm::core::allowsRemoteMoveFallback(missingFileSystem));
}

void RemoteFileOperationsTest::classifiesRemoteFilesystemIdsConservatively()
{
    QCOMPARE(rfm::core::remoteFilesystemRelation(quint64{42}, quint64{42}),
             rfm::core::RemoteFilesystemRelation::Same);
    QCOMPARE(rfm::core::remoteFilesystemRelation(quint64{42}, quint64{43}),
             rfm::core::RemoteFilesystemRelation::Different);
    QCOMPARE(rfm::core::remoteFilesystemRelation(std::nullopt, quint64{43}),
             rfm::core::RemoteFilesystemRelation::Unknown);
    QCOMPARE(rfm::core::remoteFilesystemRelation(quint64{42}, std::nullopt),
             rfm::core::RemoteFilesystemRelation::Unknown);
}

void RemoteFileOperationsTest::serverSideCopyPollingPreservesSharedSessionBlockingMode()
{
    QFile source(QStringLiteral(RFM_SOURCE_DIR "/src/ssh/SshSession.cpp"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray implementation = source.readAll();

    QVERIFY(!implementation.contains("ssh_channel_set_blocking"));
    QVERIFY(!implementation.contains("ssh_set_blocking"));
    QVERIFY(!implementation.contains("ssh_channel_get_exit_state"));
    QVERIFY(!implementation.contains("ssh_channel_get_exit_status"));
    QVERIFY(implementation.count("ssh_channel_read_nonblocking") >= 2);
    QVERIFY(implementation.contains("ssh_set_channel_callbacks"));
    QVERIFY(implementation.contains("CommandKind::MoveStagingCopy"));
    QVERIFY(implementation.contains("parseCopyStatus(m_standardOutput)"));
}

void RemoteFileOperationsTest::remoteCopyBackendRoutesNativeAndClientMediatedPaths()
{
    QFile source(QStringLiteral(RFM_SOURCE_DIR "/src/ssh/SshSession.cpp"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray implementation = source.readAll();
    const qsizetype start = implementation.indexOf("startCopy(const QString& source");
    const qsizetype end = implementation.indexOf("reserveStaging", start);
    QVERIFY(start >= 0);
    QVERIFY(end > start);
    const QByteArray copyPath = implementation.sliced(start, end - start);

    const qsizetype nativeBranch = copyPath.indexOf("RemoteCopyMethod::NativeServerCopy");
    const qsizetype nativeCommand = copyPath.indexOf("RemoteCopyCommand::build", nativeBranch);
    const qsizetype nativeStart =
        copyPath.indexOf("startCommand(command, CommandKind::Copy)", nativeCommand);
    const qsizetype clientMediatedStart = copyPath.indexOf("closeChannel()", nativeStart);
    QVERIFY(nativeBranch >= 0);
    QVERIFY(nativeCommand > nativeBranch);
    QVERIFY(nativeStart > nativeCommand);
    QVERIFY(clientMediatedStart > nativeStart);
    const QByteArray nativePath = copyPath.sliced(nativeBranch, clientMediatedStart - nativeBranch);
    QVERIFY(!nativePath.contains("m_copyActive"));
    QVERIFY(!nativePath.contains("sftp_read"));
    QVERIFY(!nativePath.contains("sftp_write"));
    QVERIFY(copyPath.indexOf("m_copyTasks.push_back", clientMediatedStart) > clientMediatedStart);
    QVERIFY(copyPath.indexOf("m_copyActive = true", clientMediatedStart) > clientMediatedStart);
    QVERIFY(implementation.contains("request.kind == rfm::core::RemoteOperationKind::Copy"));
    QVERIFY(implementation.contains("selectRemoteCopyMethod(m_impl->currentServerCapabilities"));
    QVERIFY(implementation.contains("Remote copy method: %1"));
    QVERIFY(implementation.contains("pollSftpCopy"));
    QVERIFY(implementation.contains("m_copyActive ? pollSftpCopy() : pollCommand()"));
    QVERIFY(implementation.contains("sftp_open"));
    QVERIFY(implementation.contains("sftp_read"));
    QVERIFY(implementation.contains("sftp_write"));
    QVERIFY(implementation.contains("sftp_mkdir"));
    QVERIFY(implementation.contains("65'536"));
    QVERIFY(implementation.contains("m_copyBufferOffset"));
    QVERIFY(implementation.contains("written > remaining"));
    QVERIFY(implementation.contains("SSH_FILEXFER_TYPE_DIRECTORY"));
    QVERIFY(!copyPath.contains("rm "));
}

void RemoteFileOperationsTest::remoteCopyStagingCleanupIsSftpBounded()
{
    QFile source(QStringLiteral(RFM_SOURCE_DIR "/src/ssh/SshSession.cpp"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray implementation = source.readAll();
    const qsizetype start = implementation.indexOf("startRemove(const QString& path");
    const qsizetype end = implementation.indexOf("startCommand(const QString& command", start);
    QVERIFY(start >= 0);
    QVERIFY(end > start);
    const QByteArray cleanupPath = implementation.sliced(start, end - start);

    QVERIFY(cleanupPath.contains("startsWith(QStringLiteral(\".rfm-copy-\"))"));
    QVERIFY(cleanupPath.contains("m_removeRoot"));
    QVERIFY(implementation.contains("pollSftpRemove"));
    QVERIFY(implementation.contains("sftp_lstat"));
    QVERIFY(implementation.contains("sftp_unlink"));
    QVERIFY(implementation.contains("sftp_rmdir"));
    QVERIFY(implementation.contains("task.removeDirectory"));
    QVERIFY(implementation.contains("SSH_FILEXFER_TYPE_SYMLINK"));
    const qsizetype shellFallback = cleanupPath.indexOf("RemoteCopyCommand::buildRemove");
    QVERIFY(shellFallback > 0);
    QVERIFY(!cleanupPath.left(shellFallback).contains("RemoteCopyCommand::buildRemove"));
}

void RemoteFileOperationsTest::remoteCopyHandlesSymlinksConservatively()
{
    QFile source(QStringLiteral(RFM_SOURCE_DIR "/src/ssh/SshSession.cpp"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    const QByteArray implementation = source.readAll();
    QVERIFY(implementation.contains("SSH_FILEXFER_TYPE_SYMLINK"));
    QVERIFY(implementation.contains("Symbolic links are not supported by remote "));
    QVERIFY(implementation.contains("SFTP copy."));
    QVERIFY(implementation.contains("sftp_lstat"));
}

void RemoteFileOperationsTest::copyStatusProtocolIsDeterministic()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.path() + QStringLiteral("/source.txt");
    const QString destinationPath = directory.path() + QStringLiteral("/destination.txt");
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("verified copy\n"), qint64{14});
    source.close();

    QProcess successful;
    successful.start(QStringLiteral("/bin/sh"),
                     {QStringLiteral("-c"),
                      rfm::ssh::RemoteCopyCommand::build(sourcePath, destinationPath, false)});
    QVERIFY(successful.waitForStarted());
    QVERIFY(successful.waitForFinished());
    const std::optional<quint32> successfulStatus =
        rfm::ssh::RemoteCopyCommand::parseCopyStatus(successful.readAllStandardOutput());
    QCOMPARE(successfulStatus, std::optional<quint32>{0});
    QCOMPARE(successful.exitStatus(), QProcess::NormalExit);
    QCOMPARE(successful.exitCode(), 0);
    QCOMPARE(QFileInfo(destinationPath).size(), QFileInfo(sourcePath).size());

    QProcess failing;
    failing.start(QStringLiteral("/bin/sh"),
                  {QStringLiteral("-c"), rfm::ssh::RemoteCopyCommand::build(
                                             directory.path() + QStringLiteral("/missing"),
                                             directory.path() + QStringLiteral("/failed"), false)});
    QVERIFY(failing.waitForStarted());
    QVERIFY(failing.waitForFinished());
    const std::optional<quint32> failingStatus =
        rfm::ssh::RemoteCopyCommand::parseCopyStatus(failing.readAllStandardOutput());
    QVERIFY(failingStatus.has_value());
    QVERIFY(*failingStatus != 0);
    QCOMPARE(static_cast<int>(*failingStatus), failing.exitCode());
}

void RemoteFileOperationsTest::copyStatusWrapperForwardsTermination()
{
#ifdef Q_OS_UNIX
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = directory.path() + QStringLiteral("/blocking-source");
    const QString destinationPath = directory.path() + QStringLiteral("/partial-destination");
    QCOMPARE(::mkfifo(QFile::encodeName(sourcePath).constData(), 0600), 0);

    QProcess process;
    process.start(QStringLiteral("/bin/sh"),
                  {QStringLiteral("-c"),
                   rfm::ssh::RemoteCopyCommand::build(sourcePath, destinationPath, false)});
    QVERIFY(process.waitForStarted());
    QTest::qWait(50);
    QVERIFY(process.state() == QProcess::Running);
    process.terminate();
    QVERIFY(process.waitForFinished(3000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 143);
    QVERIFY(
        !rfm::ssh::RemoteCopyCommand::parseCopyStatus(process.readAllStandardOutput()).has_value());
    const qint64 sizeAfterTermination = QFileInfo(destinationPath).size();
    QTest::qWait(50);
    QCOMPARE(QFileInfo(destinationPath).size(), sizeAfterTermination);
#else
    QSKIP("Signal forwarding is exercised on POSIX platforms.");
#endif
}

void RemoteFileOperationsTest::activeStagingOwnershipIsExactAndTemporary()
{
    FakeCopyBackend backend;
    backend.completeAfterPolls = 1;
    backend.emptyDirectoryRemoveResult = {rfm::core::RemoteBackendError::PermissionDenied,
                                          QStringLiteral("rmdir denied")};
    rfm::core::ServerSideCopyJob job(backend, 99, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"));
    job.step();
    job.step();

    const QString staging = backend.reserved.constFirst();
    QVERIFY(job.ownedStagingPath().has_value());
    QCOMPARE(*job.ownedStagingPath(), staging);
    QVERIFY(job.ownsInternalPath(staging));
    QVERIFY(job.ownsInternalPath(staging + QStringLiteral("/item")));
    QVERIFY(job.hidesListingEntry(QStringLiteral("/destination"),
                                  rfm::core::RemotePath::fileName(staging)));
    QVERIFY(!job.hidesListingEntry(QStringLiteral("/destination"),
                                   QStringLiteral(".rfm-copy-abandoned.partial")));
    QVERIFY(!job.ownsInternalPath(QStringLiteral("/destination/.rfm-copy-abandoned.partial")));
    QVERIFY(!job.ownsInternalPath(QStringLiteral("/destination/ordinary.txt")));

    while (!job.isFinished()) {
        job.step();
    }
    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(backend.stagingExists);
    QVERIFY(!job.ownedStagingPath().has_value());
    QVERIFY(!job.ownsInternalPath(staging));
    QVERIFY(!job.hidesListingEntry(QStringLiteral("/destination"),
                                   rfm::core::RemotePath::fileName(staging)));
    QVERIFY(!job.ownsInternalPath(QStringLiteral("/destination/.rfm-copy-abandoned.partial")));
}

void RemoteFileOperationsTest::stagingNameIsIndependentOfFinalBasename_data()
{
    QTest::addColumn<QString>("basename");
    QTest::newRow("near-name-max") << QString(250, QChar{'a'});
    QTest::newRow("long-multibyte-utf8") << QString(110, QChar{0x00e9});
}

void RemoteFileOperationsTest::stagingNameIsIndependentOfFinalBasename()
{
    QFETCH(QString, basename);

    FakeCopyBackend backend;
    backend.completeAfterPolls = 1;
    const QString source = QStringLiteral("/source/") + basename;
    const QString destination = QStringLiteral("/destination/") + basename;
    rfm::core::ServerSideCopyJob job(backend, 100, {{source, false}},
                                     QStringLiteral("/destination"));
    job.step();
    job.step();

    const QString staging = backend.reserved.constFirst();
    const QString stagingName = rfm::core::RemotePath::fileName(staging);
    QVERIFY(stagingName.startsWith(QStringLiteral(".rfm-copy-")));
    QVERIFY(stagingName.endsWith(QStringLiteral(".partial")));
    QVERIFY(stagingName.toUtf8().size() < 80);
    QVERIFY(!stagingName.contains(basename));

    while (!job.isFinished()) {
        job.step();
    }
    QCOMPARE(job.progress().state, rfm::core::OperationState::Completed);
    QCOMPARE(job.result().items.constFirst().destination, destination);
    QVERIFY(backend.existingPaths.contains(destination));
}

void RemoteFileOperationsTest::transportFailurePreservesCompletedItems()
{
    FakeCopyBackend backend;
    backend.completeAfterPolls = 1;
    rfm::core::ServerSideCopyJob job(backend, 101,
                                     {{QStringLiteral("/source/A"), false},
                                      {QStringLiteral("/source/B"), false},
                                      {QStringLiteral("/source/C"), false}},
                                     QStringLiteral("/destination"));
    while (job.progress().completedItems == 0) {
        job.step();
    }
    job.step(); // Prepare B.
    job.step(); // Reserve B staging.
    job.step(); // Start B copy.
    const QString staging = backend.reserved.constLast();

    job.failTransport(QStringLiteral("SSH connection lost"));
    job.failTransport(QStringLiteral("must be idempotent"));

    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QCOMPARE(job.result().items.size(), 3);
    QVERIFY(job.result().items.at(0).success);
    QVERIFY(!job.result().items.at(1).success);
    QVERIFY(job.result().items.at(1).error.contains(QStringLiteral("SSH connection lost")));
    QVERIFY(job.result().items.at(1).error.contains(staging));
    QVERIFY(!job.result().items.at(2).success);
    QVERIFY(job.result().items.at(2).error.contains(QStringLiteral("not started")));
    QCOMPARE(job.progress().completedItems, quint64{1});
}

void RemoteFileOperationsTest::transportFailureBeforeFirstSuccess()
{
    FakeCopyBackend backend;
    rfm::core::ServerSideCopyJob job(
        backend, 102, {{QStringLiteral("/source/A"), false}, {QStringLiteral("/source/B"), false}},
        QStringLiteral("/destination"));
    job.step();
    job.step();
    const QString staging = backend.reserved.constFirst();

    job.failTransport(QStringLiteral("SSH connection lost"));

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QCOMPARE(job.result().items.size(), 2);
    QVERIFY(!job.result().items.at(0).success);
    QVERIFY(job.result().items.at(0).error.contains(staging));
    QVERIFY(!job.result().items.at(1).success);
    QCOMPARE(job.progress().completedItems, quint64{0});
}

void RemoteFileOperationsTest::serverSideCopyRunsCooperatively_data()
{
    QTest::addColumn<bool>("directory");
    QTest::newRow("file") << false;
    QTest::newRow("directory") << true;
}

void RemoteFileOperationsTest::serverSideCopyRunsCooperatively()
{
    QFETCH(bool, directory);

    FakeCopyBackend backend;
    backend.completeAfterPolls = 2;
    const QString name = directory ? QStringLiteral("tree") : QStringLiteral("file");
    const QString source = QStringLiteral("/source/%1").arg(name);
    const QString destination = QStringLiteral("/destination/%1").arg(name);
    rfm::core::ServerSideCopyJob job(backend, 72, {{source, directory}},
                                     QStringLiteral("/destination"));

    QCOMPARE(job.progress().state, rfm::core::OperationState::Preparing);
    job.step(); // Check the final destination.
    QCOMPARE(job.progress().state, rfm::core::OperationState::Running);
    QVERIFY(!backend.existingPaths.contains(destination));
    job.step(); // Atomically reserve staging beside the final destination.
    QCOMPARE(backend.reserved.size(), 1);
    const QString staging = backend.reserved.constFirst();
    QVERIFY(staging.startsWith(QStringLiteral("/destination/.rfm-copy-")));
    QVERIFY(staging.endsWith(QStringLiteral(".partial")));
    QVERIFY(backend.stagingExists);
    QVERIFY(!backend.existingPaths.contains(destination));
    job.step(); // Start cp into staging/item.
    QCOMPARE(backend.started.size(), 1);
    QCOMPARE(backend.started.constFirst(),
             QStringLiteral("%1:%2/item:%3")
                 .arg(source, staging,
                      directory ? QStringLiteral("recursive") : QStringLiteral("file")));
    job.step(); // cp still running.
    QVERIFY(!job.isFinished());
    QVERIFY(backend.renamed.isEmpty());
    QVERIFY(!backend.existingPaths.contains(destination));
    job.step(); // cp succeeded, but promotion has not run yet.
    QCOMPARE(job.progress().state, rfm::core::OperationState::Finalizing);
    QVERIFY(!backend.existingPaths.contains(destination));
    job.step(); // Re-check and promote staging/item.
    QVERIFY(backend.existingPaths.contains(destination));
    QVERIFY(!backend.existingPaths.contains(staging + QStringLiteral("/item")));
    QVERIFY(backend.stagingExists);
    QVERIFY(!job.isFinished());
    job.step(); // Remove the now-empty staging directory with SFTP rmdir.
    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Completed);
    QCOMPARE(job.progress().completedItems, quint64{1});
    QVERIFY(!backend.stagingExists);
    QCOMPARE(backend.renamed,
             QStringList({QStringLiteral("%1/item:%2").arg(staging, destination)}));
    QCOMPARE(backend.emptyDirectoryRemovals, QStringList({staging}));
    QVERIFY(backend.removed.isEmpty());
}

void RemoteFileOperationsTest::serverSideCopyCancellationWaitsForTermination()
{
    FakeCopyBackend backend;
    backend.cancellationRequestResponses = {rfm::core::RemoteBackendResult{}};
    backend.cancellationPollResponses = {std::nullopt, rfm::core::RemoteBackendResult{}};
    rfm::core::ServerSideCopyJob job(backend, 73, {{QStringLiteral("/source/link"), false}},
                                     QStringLiteral("/destination"));
    job.step(); // prepare
    job.step(); // reserve staging
    job.step(); // start cp
    const QString staging = backend.reserved.constFirst();

    QVERIFY(job.requestCancel());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Cancelling);
    QCOMPARE(backend.cancellationRequestCalls, 0);
    QCOMPARE(backend.cancellationPollCalls, 0);
    QVERIFY(backend.active);
    QVERIFY(!job.isFinished());

    job.step();
    QCOMPARE(backend.cancellationRequestCalls, 1);
    QCOMPARE(backend.cancellationPollCalls, 0);
    QCOMPARE(backend.cleanupCalls, 0);
    QVERIFY(backend.active);
    QVERIFY(!job.isFinished());

    job.step();
    QCOMPARE(backend.cancellationPollCalls, 1);
    QCOMPARE(backend.cleanupCalls, 0);
    QVERIFY(backend.active);
    QVERIFY(!job.isFinished());

    job.step();
    QCOMPARE(backend.cancellationPollCalls, 2);
    QCOMPARE(backend.cleanupCalls, 1);
    QVERIFY(!backend.active);
    QVERIFY(!job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Cancelling);
    QVERIFY(backend.stagingExists);
    QVERIFY(!backend.existingPaths.contains(QStringLiteral("/destination/link")));

    job.step(); // Start protected staging cleanup.
    QVERIFY(!job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Cancelling);
    job.step(); // Cleanup becomes terminal.
    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Cancelled);
    QCOMPARE(job.result().items.size(), 1);
    QVERIFY(job.result().items.at(0).error.contains(QStringLiteral("cancelled")));
    QVERIFY(!backend.stagingExists);
    QVERIFY(!backend.existingPaths.contains(QStringLiteral("/destination/link")));
    QVERIFY(backend.removed.constFirst().startsWith(staging));
    QVERIFY(backend.emptyDirectoryRemovals.isEmpty());
}

void RemoteFileOperationsTest::serverSideCopyCancellationRetriesAndReportsFailure()
{
    FakeCopyBackend retryingBackend;
    retryingBackend.cancellationRequestResponses = {std::nullopt, std::nullopt,
                                                    rfm::core::RemoteBackendResult{}};
    retryingBackend.cancellationPollResponses = {rfm::core::RemoteBackendResult{}};
    rfm::core::ServerSideCopyJob retrying(retryingBackend, 74,
                                          {{QStringLiteral("/source/file"), false}},
                                          QStringLiteral("/destination"));
    retrying.step();
    retrying.step();
    retrying.step();
    QVERIFY(retrying.requestCancel());
    for (int step = 0; step < 20 && !retrying.isFinished(); ++step) {
        retrying.step();
    }
    QCOMPARE(retrying.progress().state, rfm::core::OperationState::Cancelled);
    QCOMPARE(retryingBackend.cancellationRequestCalls, 3);
    QCOMPARE(retryingBackend.cancellationPollCalls, 1);
    QVERIFY(!retryingBackend.stagingExists);

    FakeCopyBackend failingBackend;
    failingBackend.cancellationRequestResponses = {rfm::core::RemoteBackendResult{
        rfm::core::RemoteBackendError::Failure, QStringLiteral("TERM delivery timed out")}};
    rfm::core::ServerSideCopyJob failing(failingBackend, 75,
                                         {{QStringLiteral("/source/file"), false}},
                                         QStringLiteral("/destination"));
    failing.step();
    failing.step();
    failing.step();
    QVERIFY(failing.requestCancel());
    for (int step = 0; step < 20 && !failing.isFinished(); ++step) {
        failing.step();
    }
    QVERIFY(failing.isFinished());
    QCOMPARE(failing.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(failing.progress().error.contains(QStringLiteral("TERM delivery timed out")));
    QVERIFY(
        failing.result().items.constFirst().error.contains(QStringLiteral("cancellation failed")));
    QVERIFY(failing.progress().error.contains(failingBackend.reserved.constFirst()));
    QVERIFY(failingBackend.stagingExists);
    QVERIFY(failingBackend.removed.isEmpty());
}

void RemoteFileOperationsTest::serverSideCopyCancellationKeepsShutdownResponsive()
{
    FakeCopyBackend backend;
    backend.cancellationRequestResponses = {rfm::core::RemoteBackendResult{}};
    backend.cancellationPollResponses = {
        std::nullopt, std::nullopt,
        rfm::core::RemoteBackendResult{rfm::core::RemoteBackendError::Failure,
                                       QStringLiteral("Cancellation timed out")}};
    rfm::core::ServerSideCopyJob job(backend, 76, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"));
    job.step();
    job.step();
    job.step();

    QVERIFY(job.requestCancel());
    QCOMPARE(backend.cancellationRequestCalls, 0);
    job.step();
    QCOMPARE(backend.cancellationRequestCalls, 1);
    QVERIFY(!job.isFinished());
    for (int expectedCalls = 1; expectedCalls <= 2; ++expectedCalls) {
        job.step();
        QCOMPARE(backend.cancellationPollCalls, expectedCalls);
        QVERIFY(!job.isFinished());
        QVERIFY(backend.active);
    }
    job.step();
    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(backend.stagingExists);
    QVERIFY(job.progress().error.contains(backend.reserved.constFirst()));
    QVERIFY(backend.removed.isEmpty());
}

void RemoteFileOperationsTest::serverSideCopyFinalCompletionWinsOverCancellation()
{
    FakeCopyBackend backend;
    backend.completeAfterPolls = 1;
    rfm::core::ServerSideCopyJob job(backend, 77, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"));
    while (!job.isFinished()) {
        job.step();
    }

    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Completed);
    QVERIFY(!job.requestCancel());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Completed);
    QCOMPARE(backend.cancellationRequestCalls, 0);
}

void RemoteFileOperationsTest::serverSideCopyCanCancelWithSourcesRemaining()
{
    FakeCopyBackend backend;
    backend.completeAfterPolls = 1;
    rfm::core::ServerSideCopyJob job(
        backend, 78,
        {{QStringLiteral("/source/first"), false}, {QStringLiteral("/source/second"), false}},
        QStringLiteral("/destination"));
    while (job.progress().completedItems == 0) {
        job.step();
    }

    QVERIFY(!job.isFinished());
    QVERIFY(job.requestCancel());
    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Cancelled);
    QCOMPARE(job.progress().completedItems, quint64{1});
    QCOMPARE(job.result().items.size(), 2);
}

void RemoteFileOperationsTest::serverSideCopyFailureCleansStaging()
{
    FakeCopyBackend backend;
    backend.completeAfterPolls = 1;
    backend.copyCompletionResult = {rfm::core::RemoteBackendError::Failure,
                                    QStringLiteral("No space left on device")};
    rfm::core::ServerSideCopyJob job(backend, 90, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"));

    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(job.progress().error.contains(QStringLiteral("No space left on device")));
    QVERIFY(!backend.existingPaths.contains(QStringLiteral("/destination/file")));
    QVERIFY(!backend.stagingExists);
    QVERIFY(backend.renamed.isEmpty());
    QCOMPARE(backend.removed.size(), 1);
    QVERIFY(backend.removed.constFirst().contains(QStringLiteral(".rfm-copy-")));
    QVERIFY(backend.emptyDirectoryRemovals.isEmpty());
}

void RemoteFileOperationsTest::serverSideCopyCleanupFailureIsTerminal()
{
    FakeCopyBackend backend;
    backend.cancellationRequestResponses = {rfm::core::RemoteBackendResult{}};
    backend.cancellationPollResponses = {rfm::core::RemoteBackendResult{}};
    backend.cleanupCompletionResult = {rfm::core::RemoteBackendError::PermissionDenied,
                                       QStringLiteral("Cleanup denied")};
    rfm::core::ServerSideCopyJob job(backend, 91, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"));
    job.step();
    job.step();
    job.step();
    const QString staging = backend.reserved.constFirst();

    QVERIFY(job.requestCancel());
    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(job.progress().error.contains(QStringLiteral("cleanup failed")));
    QVERIFY(job.progress().error.contains(staging));
    QVERIFY(backend.stagingExists);
    QVERIFY(!backend.existingPaths.contains(QStringLiteral("/destination/file")));
    QVERIFY(!backend.sourceRemoved);
    QVERIFY(backend.emptyDirectoryRemovals.isEmpty());
}

void RemoteFileOperationsTest::serverSideCopyPostPromotionRmdirFailureIsTerminal()
{
    FakeCopyBackend backend;
    backend.completeAfterPolls = 1;
    backend.emptyDirectoryRemoveResult = {rfm::core::RemoteBackendError::PermissionDenied,
                                          QStringLiteral("SFTP rmdir permission denied")};
    rfm::core::ServerSideCopyJob job(backend, 97, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"));

    while (!job.isFinished()) {
        job.step();
    }

    const QString staging = backend.reserved.constFirst();
    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(job.progress().error.contains(QStringLiteral("promoted")));
    QVERIFY(job.progress().error.contains(QStringLiteral("SFTP rmdir permission denied")));
    QVERIFY(job.progress().error.contains(staging));
    QVERIFY(backend.existingPaths.contains(QStringLiteral("/destination/file")));
    QVERIFY(backend.stagingExists);
    QCOMPARE(backend.emptyDirectoryRemovals, QStringList({staging}));
    QVERIFY(backend.removed.isEmpty());
}

void RemoteFileOperationsTest::serverSideCopyNonEmptyPostPromotionStagingFailsClosed()
{
    FakeCopyBackend backend;
    backend.completeAfterPolls = 1;
    backend.stagingContainsUnexpectedEntry = true;
    rfm::core::ServerSideCopyJob job(backend, 98, {{QStringLiteral("/source/tree"), true}},
                                     QStringLiteral("/destination"));

    while (!job.isFinished()) {
        job.step();
    }

    const QString staging = backend.reserved.constFirst();
    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(job.progress().error.contains(QStringLiteral("not empty")));
    QVERIFY(job.progress().error.contains(staging));
    QVERIFY(backend.existingPaths.contains(QStringLiteral("/destination/tree")));
    QVERIFY(backend.stagingExists);
    QCOMPARE(backend.emptyDirectoryRemovals, QStringList({staging}));
    QVERIFY(backend.removed.isEmpty());
}

void RemoteFileOperationsTest::serverSideCopyLateCollisionPreservesCompetitor()
{
    FakeCopyBackend backend;
    backend.completeAfterPolls = 1;
    rfm::core::ServerSideCopyJob job(backend, 92, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"));
    job.step();
    job.step();
    job.step();
    job.step(); // cp succeeded in staging.
    QCOMPARE(job.progress().state, rfm::core::OperationState::Finalizing);
    backend.existingPaths.insert(QStringLiteral("/destination/file"));

    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(job.progress().error.contains(QStringLiteral("appeared")));
    QVERIFY(backend.existingPaths.contains(QStringLiteral("/destination/file")));
    QVERIFY(backend.renamed.isEmpty());
    QVERIFY(!backend.stagingExists);
}

void RemoteFileOperationsTest::copiesValidAndBrokenSymlinksThroughReservedStaging_data()
{
    QTest::addColumn<bool>("targetExists");
    QTest::newRow("valid-symlink") << true;
    QTest::newRow("broken-symlink") << false;
}

void RemoteFileOperationsTest::copiesValidAndBrokenSymlinksThroughReservedStaging()
{
    QFETCH(bool, targetExists);

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sourceDirectory = temporaryDirectory.path() + QStringLiteral("/source");
    const QString destinationDirectory = temporaryDirectory.path() + QStringLiteral("/destination");
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(QDir().mkpath(destinationDirectory));
    const QString target = sourceDirectory + QStringLiteral("/target.txt");
    if (targetExists) {
        QFile targetFile(target);
        QVERIFY(targetFile.open(QIODevice::WriteOnly));
        QCOMPARE(targetFile.write("TARGET\n"), qint64{7});
    }
    const QString sourceLink = sourceDirectory + QStringLiteral("/copy-link");
    QVERIFY(QFile::link(target, sourceLink));
    QVERIFY(QFileInfo(sourceLink).isSymLink());

    LocalSymlinkStagingBackend backend(false);
    rfm::core::ServerSideCopyJob job(backend, 93, {{sourceLink, false}}, destinationDirectory);
    for (int step = 0; step < 20 && !job.isFinished(); ++step) {
        job.step();
    }

    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Completed);
    QVERIFY(backend.stagingReserved);
    QVERIFY(backend.copyStartedAfterReservation);
    QVERIFY(backend.copiedSourceWasSymlink);
    QVERIFY(backend.promotionObserved);
    QVERIFY(backend.emptyStagingRemovalUsed);
    QVERIFY(!backend.stagingRemovalProtected);
    QVERIFY(QFileInfo(sourceLink).isSymLink());
    const QString destinationLink = destinationDirectory + QStringLiteral("/copy-link");
    QVERIFY(QFileInfo(destinationLink).isSymLink());
    QCOMPARE(QFile::symLinkTarget(destinationLink), target);
    QVERIFY(!QFileInfo(backend.stagingPath).exists());
    QCOMPARE(QFileInfo(target).exists(), targetExists);
}

void RemoteFileOperationsTest::serverSideCopyNoClobberAndRetryAfterCancellation()
{
    FakeCopyBackend collisionBackend;
    collisionBackend.existingPaths.insert(QStringLiteral("/destination/file"));
    rfm::core::ServerSideCopyJob collision(collisionBackend, 94,
                                           {{QStringLiteral("/source/file"), false}},
                                           QStringLiteral("/destination"));
    while (!collision.isFinished()) {
        collision.step();
    }
    QCOMPARE(collision.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(collisionBackend.reserved.isEmpty());
    QVERIFY(collisionBackend.started.isEmpty());
    QVERIFY(collisionBackend.existingPaths.contains(QStringLiteral("/destination/file")));

    FakeCopyBackend backend;
    backend.cancellationRequestResponses = {rfm::core::RemoteBackendResult{}};
    backend.cancellationPollResponses = {rfm::core::RemoteBackendResult{}};
    rfm::core::ServerSideCopyJob cancelled(backend, 95, {{QStringLiteral("/source/file"), false}},
                                           QStringLiteral("/destination"));
    cancelled.step();
    cancelled.step();
    cancelled.step();
    QVERIFY(cancelled.requestCancel());
    while (!cancelled.isFinished()) {
        cancelled.step();
    }
    QCOMPARE(cancelled.progress().state, rfm::core::OperationState::Cancelled);
    QVERIFY(!backend.existingPaths.contains(QStringLiteral("/destination/file")));
    QVERIFY(!backend.stagingExists);

    backend.completeAfterPolls = 1;
    backend.pollCalls = 0;
    rfm::core::ServerSideCopyJob retry(backend, 96, {{QStringLiteral("/source/file"), false}},
                                       QStringLiteral("/destination"));
    while (!retry.isFinished()) {
        retry.step();
    }
    QCOMPARE(retry.progress().state, rfm::core::OperationState::Completed);
    QVERIFY(backend.existingPaths.contains(QStringLiteral("/destination/file")));
    QVERIFY(!backend.stagingExists);
}

void RemoteFileOperationsTest::movesOnSameFileSystemWithRename()
{
    FakeCopyBackend backend;
    rfm::core::ServerSideCopyJob job(backend, 79, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"),
                                     rfm::core::RemoteOperationKind::Move);

    job.step();
    job.step();

    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Completed);
    QCOMPARE(job.result().kind, rfm::core::RemoteOperationKind::Move);
    QCOMPARE(backend.renamed, QStringList({QStringLiteral("/source/file:/destination/file")}));
    QVERIFY(backend.started.isEmpty());
    QVERIFY(backend.removed.isEmpty());
}

void RemoteFileOperationsTest::fallsBackToCopyThenDeleteAcrossFileSystems()
{
    FakeCopyBackend backend;
    backend.renameResult = {rfm::core::RemoteBackendError::CrossDevice, {}};
    backend.completeAfterPolls = 1;
    rfm::core::ServerSideCopyJob job(backend, 80, {{QStringLiteral("/source/tree"), true}},
                                     QStringLiteral("/destination"),
                                     rfm::core::RemoteOperationKind::Move);

    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Completed);
    QCOMPARE(backend.reserved.size(), 1);
    const QString temporaryPath = backend.reserved.constFirst();
    QVERIFY(temporaryPath.startsWith(QStringLiteral("/destination/.rfm-move-")));
    QVERIFY(temporaryPath.endsWith(QStringLiteral(".partial")));
    QCOMPARE(backend.stagedCopies,
             QStringList({QStringLiteral("/source/tree:%1/item:archive").arg(temporaryPath)}));
    QCOMPARE(backend.renamed.size(), 2);
    QCOMPARE(backend.renamed.constLast(),
             temporaryPath + QStringLiteral("/item:/destination/tree"));
    QCOMPARE(backend.removed, QStringList({temporaryPath + QStringLiteral(":recursive:protected"),
                                           QStringLiteral("/source/tree:recursive:protected")}));
    QVERIFY(backend.emptyDirectoryRemovals.isEmpty());
    QVERIFY(backend.sourceRemoved);
    QVERIFY(!backend.stagingExists);
}

void RemoteFileOperationsTest::movesValidAndBrokenSymlinksThroughReservedStaging_data()
{
    QTest::addColumn<bool>("targetExists");

    QTest::newRow("valid-symlink") << true;
    QTest::newRow("broken-symlink") << false;
}

void RemoteFileOperationsTest::movesValidAndBrokenSymlinksThroughReservedStaging()
{
    QFETCH(bool, targetExists);

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString sourceDirectory = temporaryDirectory.path() + QStringLiteral("/source");
    const QString destinationDirectory = temporaryDirectory.path() + QStringLiteral("/destination");
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(QDir().mkpath(destinationDirectory));

    const QString target = sourceDirectory + QStringLiteral("/target.txt");
    if (targetExists) {
        QFile targetFile(target);
        QVERIFY(targetFile.open(QIODevice::WriteOnly));
        QCOMPARE(targetFile.write("TARGET 2\n"), qint64{9});
        targetFile.close();
    }
    const QString sourceLink = sourceDirectory + QStringLiteral("/test-link2");
    QVERIFY(QFile::link(target, sourceLink));
    QVERIFY(QFileInfo(sourceLink).isSymLink());

    LocalSymlinkStagingBackend backend(true);
    rfm::core::ServerSideCopyJob job(backend, 88, {{sourceLink, false}}, destinationDirectory,
                                     rfm::core::RemoteOperationKind::Move);
    for (int step = 0; step < 20 && !job.isFinished(); ++step) {
        job.step();
    }

    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Completed);
    QCOMPARE(job.result().items.size(), 1);
    QVERIFY(job.result().items.constFirst().success);
    QCOMPARE(backend.reportedCopyStatus, std::optional<quint32>{0});
    QVERIFY(backend.stagingReserved);
    QVERIFY(backend.copyStartedAfterReservation);
    QVERIFY(backend.copiedSourceWasSymlink);
    QCOMPARE(backend.stagedLinkTarget, target);
    QVERIFY(backend.promotionObserved);
    QVERIFY(backend.stagingRemovalProtected);
    QVERIFY(backend.sourceRemovalObservedAfterPromotion);

    const QString destinationLink = destinationDirectory + QStringLiteral("/test-link2");
    QVERIFY(QFileInfo(destinationLink).isSymLink());
    QCOMPARE(QFile::symLinkTarget(destinationLink), target);
    QVERIFY(!QFileInfo(sourceLink).exists());
    QVERIFY(!QFileInfo(sourceLink).isSymLink());
    QVERIFY(!QFileInfo(backend.stagingPath).exists());
    QCOMPARE(QFileInfo(target).exists(), targetExists);
}

void RemoteFileOperationsTest::lateStagingCollisionPreservesSource()
{
    FakeCopyBackend backend;
    backend.renameResult = {rfm::core::RemoteBackendError::CrossDevice, {}};
    backend.reserveResult = {rfm::core::RemoteBackendError::AlreadyExists, {}};
    rfm::core::ServerSideCopyJob job(backend, 85, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"),
                                     rfm::core::RemoteOperationKind::Move);

    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(job.result().items.constFirst().error.contains(QStringLiteral("temporary")));
    QCOMPARE(backend.probed, QStringList({QStringLiteral("/destination/file")}));
    QCOMPARE(backend.reserved.size(), 1);
    QVERIFY(backend.stagedCopies.isEmpty());
    QCOMPARE(backend.renamed.size(), 1);
    QVERIFY(backend.removed.isEmpty());
    QVERIFY(!backend.sourceRemoved);
    QVERIFY(!backend.stagingExists);
}

void RemoteFileOperationsTest::keepsSourceWhenCrossFileSystemCopyFails()
{
    FakeCopyBackend backend;
    backend.renameResult = {rfm::core::RemoteBackendError::CrossDevice, {}};
    backend.completeAfterPolls = 1;
    backend.copyCompletionResult = {rfm::core::RemoteBackendError::Failure,
                                    QStringLiteral("Copy interrupted")};
    rfm::core::ServerSideCopyJob job(backend, 81, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"),
                                     rfm::core::RemoteOperationKind::Move);

    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(!job.result().items.constFirst().success);
    QVERIFY(job.result().items.constFirst().error.contains(QStringLiteral("Copy interrupted")));
    QCOMPARE(backend.removed.size(), 1);
    QVERIFY(backend.removed.constFirst().contains(QStringLiteral(".rfm-move-")));
    QVERIFY(!backend.sourceRemoved);
    QVERIFY(!backend.stagingExists);
}

void RemoteFileOperationsTest::reportsFailedStagingCleanupWithoutMaskingCopyError()
{
    FakeCopyBackend backend;
    backend.renameResult = {rfm::core::RemoteBackendError::CrossDevice, {}};
    backend.completeAfterPolls = 1;
    backend.copyCompletionResult = {rfm::core::RemoteBackendError::Failure,
                                    QStringLiteral("No space left on device")};
    backend.cleanupCompletionResult = {rfm::core::RemoteBackendError::PermissionDenied, {}};
    rfm::core::ServerSideCopyJob job(backend, 86, {{QStringLiteral("/source/tree"), true}},
                                     QStringLiteral("/destination"),
                                     rfm::core::RemoteOperationKind::Move);

    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    const QString error = job.result().items.constFirst().error;
    QVERIFY(error.contains(QStringLiteral("No space left on device")));
    QVERIFY(error.contains(QStringLiteral("cleanup failed")));
    QVERIFY(error.contains(QStringLiteral(".rfm-move-")));
    QVERIFY(!backend.sourceRemoved);
    QVERIFY(backend.stagingExists);
}

void RemoteFileOperationsTest::cancellationCleansMoveStagingAndPreservesSource()
{
    FakeCopyBackend backend;
    backend.renameResult = {rfm::core::RemoteBackendError::CrossDevice, {}};
    backend.cancellationRequestResponses = {rfm::core::RemoteBackendResult{}};
    backend.cancellationPollResponses = {rfm::core::RemoteBackendResult{}};
    rfm::core::ServerSideCopyJob job(backend, 87, {{QStringLiteral("/source/tree"), true}},
                                     QStringLiteral("/destination"),
                                     rfm::core::RemoteOperationKind::Move);

    job.step(); // prepare
    job.step(); // cross-device rename
    job.step(); // reserve staging
    job.step(); // start staging copy
    QVERIFY(job.requestCancel());
    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Cancelled);
    QVERIFY(job.result().items.constFirst().error.contains(QStringLiteral("cancelled")));
    QCOMPARE(backend.renamed.size(), 1);
    QVERIFY(!backend.sourceRemoved);
    QVERIFY(!backend.stagingExists);
    QVERIFY(std::ranges::any_of(backend.removed, [](const QString& call) {
        return call.contains(QStringLiteral(".rfm-move-"));
    }));
}

void RemoteFileOperationsTest::keepsSourceWhenFallbackPromotionFails()
{
    FakeCopyBackend backend;
    backend.renameResult = {rfm::core::RemoteBackendError::CrossDevice, {}};
    backend.promotionResult = {rfm::core::RemoteBackendError::PermissionDenied, {}};
    backend.completeAfterPolls = 1;
    rfm::core::ServerSideCopyJob job(backend, 84, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"),
                                     rfm::core::RemoteOperationKind::Move);

    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(
        job.result().items.constFirst().error.contains(QStringLiteral("temporary destination")));
    QVERIFY(job.result().items.constFirst().error.contains(QStringLiteral("source was preserved")));
    QCOMPARE(backend.removed.size(), 1);
    QVERIFY(backend.removed.constFirst().contains(QStringLiteral(".rfm-move-")));
    QVERIFY(!backend.sourceRemoved);
    QVERIFY(!backend.stagingExists);
}

void RemoteFileOperationsTest::reportsSourceCleanupFailureAfterCopy()
{
    FakeCopyBackend backend;
    backend.renameResult = {rfm::core::RemoteBackendError::CrossDevice, {}};
    backend.completeAfterPolls = 1;
    backend.removeCompletionResult = {rfm::core::RemoteBackendError::PermissionDenied, {}};
    rfm::core::ServerSideCopyJob job(backend, 83, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"),
                                     rfm::core::RemoteOperationKind::Move);

    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(job.result().items.constFirst().error.contains(QStringLiteral("was copied")));
    QVERIFY(job.result().items.constFirst().error.contains(QStringLiteral("could not be removed")));
    QCOMPARE(backend.removed.size(), 2);
    QVERIFY(!backend.sourceRemoved);
    QVERIFY(!backend.stagingExists);
}

void RemoteFileOperationsTest::preservesPromotedSourceWhenMountTreeGuardFails()
{
    FakeCopyBackend backend;
    backend.renameResult = {rfm::core::RemoteBackendError::CrossDevice, {}};
    backend.completeAfterPolls = 1;
    backend.removeCompletionResult = {
        rfm::core::RemoteBackendError::Failure,
        QStringLiteral("Refusing recursive removal: a mount point exists in the removal tree")};
    rfm::core::ServerSideCopyJob job(backend, 89, {{QStringLiteral("/source/directory"), true}},
                                     QStringLiteral("/destination"),
                                     rfm::core::RemoteOperationKind::Move);

    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QCOMPARE(backend.renamed.size(), 2);
    QVERIFY(backend.removed.constLast().endsWith(QStringLiteral(":recursive:protected")));
    QVERIFY(job.result().items.constFirst().error.contains(
        QStringLiteral("mount point exists in the removal tree")));
    QVERIFY(!backend.sourceRemoved);
    QVERIFY(!backend.stagingExists);
}

void RemoteFileOperationsTest::doesNotFallbackForAnUnqualifiedRenameFailure()
{
    FakeCopyBackend backend;
    backend.renameResult = {rfm::core::RemoteBackendError::PermissionDenied, {}};
    rfm::core::ServerSideCopyJob job(backend, 82, {{QStringLiteral("/source/file"), false}},
                                     QStringLiteral("/destination"),
                                     rfm::core::RemoteOperationKind::Move);

    while (!job.isFinished()) {
        job.step();
    }

    QCOMPARE(job.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(job.result().items.constFirst().error.contains(QStringLiteral("Permission")));
    QVERIFY(backend.started.isEmpty());
    QVERIFY(backend.reserved.isEmpty());
    QVERIFY(backend.removed.isEmpty());
    QVERIFY(!backend.sourceRemoved);
}

void RemoteFileOperationsTest::createsDirectoryAndReportsCollisionOrPermission()
{
    FakeRemoteBackend backend;
    rfm::core::RemoteFileOperations operations(backend);
    const auto created = operations.createDirectory(1, QStringLiteral("."), QStringLiteral("new"));
    QVERIFY(created.allSucceeded());
    QVERIFY(backend.nodes.contains(QStringLiteral("./new")));

    const auto collision =
        operations.createDirectory(2, QStringLiteral("."), QStringLiteral("new"));
    QVERIFY(!collision.allSucceeded());
    QVERIFY(collision.items.constFirst().error.contains(QStringLiteral("exists")));

    backend.forced.insert(QStringLiteral("mkdir:./private"),
                          {rfm::core::RemoteBackendError::PermissionDenied, {}});
    const auto denied =
        operations.createDirectory(3, QStringLiteral("."), QStringLiteral("private"));
    QVERIFY(!denied.allSucceeded());
    QVERIFY(denied.items.constFirst().error.contains(QStringLiteral("Permission")));
}

void RemoteFileOperationsTest::renamesWithoutOverwriting()
{
    FakeRemoteBackend backend;
    backend.nodes.insert(QStringLiteral("docs/a.txt"), false);
    rfm::core::RemoteFileOperations operations(backend);
    const auto renamed =
        operations.rename(4, QStringLiteral("docs/a.txt"), QStringLiteral("b.txt"));
    QVERIFY(renamed.allSucceeded());
    QVERIFY(backend.calls.contains(QStringLiteral("rename:docs/a.txt:docs/b.txt")));

    backend.nodes.insert(QStringLiteral("docs/taken.txt"), false);
    const auto collision =
        operations.rename(5, QStringLiteral("docs/b.txt"), QStringLiteral("taken.txt"));
    QVERIFY(!collision.allSucceeded());
    QVERIFY(!backend.calls.contains(QStringLiteral("rename:docs/b.txt:docs/taken.txt")));
}

void RemoteFileOperationsTest::movesSelectionAndReportsPartialFailure()
{
    FakeRemoteBackend backend;
    backend.forced.insert(QStringLiteral("rename:src/b.txt"),
                          {rfm::core::RemoteBackendError::PermissionDenied, {}});
    rfm::core::RemoteFileOperations operations(backend);
    const auto result = operations.move(
        6, {{QStringLiteral("src/a.txt"), false}, {QStringLiteral("src/b.txt"), false}},
        QStringLiteral("archive"));
    QCOMPARE(result.items.size(), 2);
    QVERIFY(result.items.at(0).success);
    QVERIFY(!result.items.at(1).success);
    QVERIFY(result.items.at(1).error.contains(QStringLiteral("Permission")));
}

void RemoteFileOperationsTest::copiesOnServerOrReportsUnsupported()
{
    FakeRemoteBackend backend;
    backend.forced.insert(QStringLiteral("copy:src/folder"),
                          {rfm::core::RemoteBackendError::Unsupported, {}});
    rfm::core::RemoteFileOperations operations(backend);
    const auto result = operations.copy(
        7, {{QStringLiteral("src/a.txt"), false}, {QStringLiteral("src/folder"), true}},
        QStringLiteral("backup"));
    QVERIFY(result.items.at(0).success);
    QVERIFY(!result.items.at(1).success);
    QVERIFY(result.items.at(1).error.contains(QStringLiteral("not supported")));
    QVERIFY(backend.calls.contains(QStringLiteral("copy:src/a.txt:backup/a.txt:f")));
    QVERIFY(backend.calls.contains(QStringLiteral("copy:src/folder:backup/folder:r")));

    const qsizetype beforeCollision = backend.calls.size();
    const auto collision =
        operations.copy(71, {{QStringLiteral("src/a.txt"), false}}, QStringLiteral("backup"));
    QVERIFY(!collision.allSucceeded());
    QVERIFY(collision.items.constFirst().error.contains(QStringLiteral("exists")));
    QCOMPARE(backend.calls.size(), beforeCollision + 1);
    QCOMPARE(backend.calls.constLast(), QStringLiteral("probe:backup/a.txt"));

    backend.forced.remove(QStringLiteral("copy:src/folder"));
    const qsizetype callCount = backend.calls.size();
    const auto insideItself = operations.copy(8, {{QStringLiteral("src/folder"), true}},
                                              QStringLiteral("src/folder/child"));
    QVERIFY(!insideItself.allSucceeded());
    QCOMPARE(backend.calls.size(), callCount);

    const auto childToParent = operations.copy(9, {{QStringLiteral("tree/child/folder"), true}},
                                               QStringLiteral("tree/./"));
    QVERIFY(childToParent.allSucceeded());
    QCOMPARE(childToParent.items.constFirst().destination, QStringLiteral("tree/folder"));
    QVERIFY(backend.calls.contains(QStringLiteral("copy:tree/child/folder:tree/folder:r")));

    const auto parentToChild =
        operations.move(10, {{QStringLiteral("/home/gabriel/tree/file.txt"), false}},
                        QStringLiteral("/home/gabriel/tree/child/"));
    QVERIFY(parentToChild.allSucceeded());
    QCOMPARE(parentToChild.items.constFirst().destination,
             QStringLiteral("/home/gabriel/tree/child/file.txt"));
    QVERIFY(backend.calls.contains(
        QStringLiteral("rename:/home/gabriel/tree/file.txt:/home/gabriel/tree/child/file.txt")));
}

void RemoteFileOperationsTest::removesFileAndRecursiveTreeWithGuards()
{
    FakeRemoteBackend backend;
    backend.nodes.insert(QStringLiteral("trash/file.txt"), false);
    backend.listings.insert(QStringLiteral("trash/folder"), {{QStringLiteral("nested.txt"), false},
                                                             {QStringLiteral("child"), true}});
    backend.listings.insert(QStringLiteral("trash/folder/child"), {});
    rfm::core::RemoteFileOperations operations(backend);
    const auto removedFile =
        operations.remove(8, {{QStringLiteral("trash/file.txt"), false}}, false);
    QVERIFY(removedFile.allSucceeded());

    const auto removedTree = operations.remove(9, {{QStringLiteral("trash/folder"), true}}, true);
    QVERIFY(removedTree.allSucceeded());
    const qsizetype nested =
        backend.calls.indexOf(QStringLiteral("unlink:trash/folder/nested.txt"));
    const qsizetype child = backend.calls.indexOf(QStringLiteral("rmdir:trash/folder/child"));
    const qsizetype parent = backend.calls.indexOf(QStringLiteral("rmdir:trash/folder"));
    QVERIFY(nested >= 0);
    QVERIFY(child > nested);
    QVERIFY(parent > child);

    const auto protectedPath = operations.remove(10, {{QStringLiteral("/"), true}}, true);
    QVERIFY(!protectedPath.allSucceeded());
    QVERIFY(!backend.calls.contains(QStringLiteral("rmdir:/")));

    backend.forced.insert(QStringLiteral("unlink:trash/denied.txt"),
                          {rfm::core::RemoteBackendError::PermissionDenied, {}});
    const auto partial = operations.remove(
        11, {{QStringLiteral("trash/ok.txt"), false}, {QStringLiteral("trash/denied.txt"), false}},
        false);
    QVERIFY(partial.items.at(0).success);
    QVERIFY(!partial.items.at(1).success);
}

void RemoteFileOperationsTest::refusesSelectedAndNestedRemoteMountPoints()
{
    FakeRemoteBackend selectedBackend;
    selectedBackend.nodes.insert(QStringLiteral("/tree"), true);
    selectedBackend.listings.insert(QStringLiteral("/tree"), {});
    const auto selectedResult = rfm::ssh::detail::removeRemoteEntriesSafely(
        selectedBackend, 12, {{QStringLiteral("/tree"), true}}, true, [](const QString& path) {
            return path == QStringLiteral("/tree")
                       ? rfm::core::RemoteMountPointState::MountPoint
                       : rfm::core::RemoteMountPointState::NotMountPoint;
        });
    QVERIFY(!selectedResult.allSucceeded());
    QVERIFY(selectedBackend.nodes.contains(QStringLiteral("/tree")));
    QVERIFY(!selectedBackend.calls.contains(QStringLiteral("rmdir:/tree")));

    FakeRemoteBackend nestedBackend;
    nestedBackend.nodes.insert(QStringLiteral("/tree"), true);
    nestedBackend.nodes.insert(QStringLiteral("/tree/normal.txt"), false);
    nestedBackend.nodes.insert(QStringLiteral("/tree/nested"), true);
    nestedBackend.nodes.insert(QStringLiteral("/tree/nested/protected.txt"), false);
    nestedBackend.listings.insert(QStringLiteral("/tree"), {{QStringLiteral("normal.txt"), false},
                                                            {QStringLiteral("nested"), true}});
    nestedBackend.listings.insert(QStringLiteral("/tree/nested"),
                                  {{QStringLiteral("protected.txt"), false}});
    QStringList probed;
    const auto nestedResult = rfm::ssh::detail::removeRemoteEntriesSafely(
        nestedBackend, 13, {{QStringLiteral("/tree"), true}}, true, [&probed](const QString& path) {
            probed.push_back(path);
            return path == QStringLiteral("/tree/nested")
                       ? rfm::core::RemoteMountPointState::MountPoint
                       : rfm::core::RemoteMountPointState::NotMountPoint;
        });
    QVERIFY(!nestedResult.allSucceeded());
    QVERIFY(nestedBackend.nodes.contains(QStringLiteral("/tree/normal.txt")));
    QVERIFY(nestedBackend.nodes.contains(QStringLiteral("/tree/nested/protected.txt")));
    QVERIFY(std::ranges::none_of(nestedBackend.calls, [](const QString& call) {
        return call.startsWith(QStringLiteral("unlink:")) ||
               call.startsWith(QStringLiteral("rmdir:"));
    }));
    QVERIFY(!probed.contains(QStringLiteral("/tree/nested/protected.txt")));
}

void RemoteFileOperationsTest::preflightsAllRemoteSourcesBeforeRemoval()
{
    FakeRemoteBackend backend;
    backend.nodes.insert(QStringLiteral("/safe"), true);
    backend.nodes.insert(QStringLiteral("/dangerous"), true);
    backend.nodes.insert(QStringLiteral("/dangerous/mounted"), true);
    backend.listings.insert(QStringLiteral("/safe"), {});
    backend.listings.insert(QStringLiteral("/dangerous"), {{QStringLiteral("mounted"), true}});
    backend.listings.insert(QStringLiteral("/dangerous/mounted"), {});

    const auto result = rfm::ssh::detail::removeRemoteEntriesSafely(
        backend, 14, {{QStringLiteral("/safe"), true}, {QStringLiteral("/dangerous"), true}}, true,
        [](const QString& path) {
            return path == QStringLiteral("/dangerous/mounted")
                       ? rfm::core::RemoteMountPointState::MountPoint
                       : rfm::core::RemoteMountPointState::NotMountPoint;
        });

    QVERIFY(!result.allSucceeded());
    QVERIFY(backend.nodes.contains(QStringLiteral("/safe")));
    QVERIFY(std::ranges::none_of(backend.calls, [](const QString& call) {
        return call.startsWith(QStringLiteral("unlink:")) ||
               call.startsWith(QStringLiteral("rmdir:"));
    }));
}

void RemoteFileOperationsTest::detectsBindMountAndMalformedMountInfo()
{
    const QByteArray namespaceRootMountInfo = "24 24 8:1 / / rw - ext4 /dev/sda1 rw\n";
    QCOMPARE(rfm::core::linuxMountPointState(namespaceRootMountInfo, QStringLiteral("/")),
             rfm::core::RemoteMountPointState::MountPoint);

    const QByteArray extensibleMountInfo =
        "24 24 8:1 / / rw future_tag:value-1 - ext4 /dev/sda1 rw\n";
    QCOMPARE(rfm::core::linuxMountPointState(extensibleMountInfo, QStringLiteral("/")),
             rfm::core::RemoteMountPointState::MountPoint);

    const QByteArray malformedOptionalField = "24 24 8:1 / / rw future_tag: - ext4 /dev/sda1 rw\n";
    QCOMPARE(rfm::core::linuxMountPointState(malformedOptionalField, QStringLiteral("/")),
             rfm::core::RemoteMountPointState::Unknown);

    const QByteArray bindMountInfo = "24 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
                                     "25 24 8:1 /bound /tree/nested rw - ext4 /dev/sda1 rw\n";
    QCOMPARE(rfm::core::linuxMountPointState(bindMountInfo, QStringLiteral("/tree/nested")),
             rfm::core::RemoteMountPointState::MountPoint);

    const QByteArray malformedMountInfo = bindMountInfo + "not a valid mountinfo record\n";
    QCOMPARE(rfm::core::linuxMountPointState(malformedMountInfo, QStringLiteral("/tree")),
             rfm::core::RemoteMountPointState::Unknown);

    FakeRemoteBackend backend;
    backend.nodes.insert(QStringLiteral("/tree"), true);
    backend.listings.insert(QStringLiteral("/tree"), {});
    const auto result = rfm::ssh::detail::removeRemoteEntriesSafely(
        backend, 15, {{QStringLiteral("/tree"), true}}, true,
        [&malformedMountInfo](const QString& path) {
            return rfm::core::linuxMountPointState(malformedMountInfo, path);
        });
    QVERIFY(!result.allSucceeded());
    QVERIFY(backend.nodes.contains(QStringLiteral("/tree")));
}

void RemoteFileOperationsTest::acceptsFileSystemSpecificMountRoots()
{
    const QByteArray representativeMountInfo =
        "34 2 8:2 / / rw,relatime shared:1 - ext4 /dev/sda2 rw\n"
        "399 32 0:5 net:[4026531833] /run/docker/netns/default rw shared:281 - nsfs nsfs rw\n"
        "833 34 0:90 / /mnt/usbtemp rw,relatime shared:456 - btrfs /dev/sdi1 "
        "rw,space_cache=v2,subvolid=5,subvol=/\n";

    QCOMPARE(rfm::core::linuxMountPointState(representativeMountInfo,
                                             QStringLiteral("/home/gabriel/rfm-big.bin")),
             rfm::core::RemoteMountPointState::NotMountPoint);
    QCOMPARE(rfm::core::linuxMountPointState(representativeMountInfo, QStringLiteral("/")),
             rfm::core::RemoteMountPointState::MountPoint);
    QCOMPARE(
        rfm::core::linuxMountPointState(representativeMountInfo, QStringLiteral("/mnt/usbtemp")),
        rfm::core::RemoteMountPointState::MountPoint);

    const QByteArray malformedMountInfo =
        representativeMountInfo +
        "900 34 0:5 invalid\\999 /run/invalid rw shared:500 - nsfs nsfs rw\n";
    QCOMPARE(rfm::core::linuxMountPointState(malformedMountInfo,
                                             QStringLiteral("/home/gabriel/rfm-big.bin")),
             rfm::core::RemoteMountPointState::Unknown);
}

void RemoteFileOperationsTest::doesNotTraverseRemoteSymlinksAndRemovesOrdinaryTrees()
{
    FakeRemoteBackend backend;
    backend.nodes.insert(QStringLiteral("/tree"), true);
    backend.nodes.insert(QStringLiteral("/tree/normal.txt"), false);
    backend.nodes.insert(QStringLiteral("/tree/link"), false);
    backend.listings.insert(QStringLiteral("/tree"), {{QStringLiteral("normal.txt"), false},
                                                      {QStringLiteral("link"), false}});
    backend.listings.insert(QStringLiteral("/tree/link"),
                            {{QStringLiteral("must-not-be-visited"), true}});
    QStringList probed;
    const auto result = rfm::ssh::detail::removeRemoteEntriesSafely(
        backend, 16, {{QStringLiteral("/tree"), true}}, true, [&probed](const QString& path) {
            probed.push_back(path);
            return rfm::core::RemoteMountPointState::NotMountPoint;
        });

    QVERIFY(result.allSucceeded());
    QVERIFY(!backend.calls.contains(QStringLiteral("list:/tree/link")));
    QVERIFY(!probed.contains(QStringLiteral("/tree/link/must-not-be-visited")));
    QVERIFY(backend.calls.contains(QStringLiteral("unlink:/tree/link")));
    QVERIFY(backend.calls.contains(QStringLiteral("rmdir:/tree")));
}

void RemoteFileOperationsTest::emitsWorkerOperationErrors()
{
    rfm::ssh::SshSession session;
    QSignalSpy failure(&session, &rfm::ssh::SshSession::failed);
    session.createDirectory(11, QStringLiteral("."), QStringLiteral("folder"));
    QCOMPARE(failure.size(), 1);
}

void RemoteFileOperationsTest::treatsListingWithoutSessionAsFatal()
{
    rfm::ssh::SshSession session;
    QSignalSpy fatalFailure(&session, &rfm::ssh::SshSession::failed);
    QSignalSpy localizedFailure(&session, &rfm::ssh::SshSession::directoryListingFailed);

    session.listDirectory(42, QStringLiteral("/srv"));

    QCOMPARE(fatalFailure.size(), 1);
    QCOMPARE(localizedFailure.size(), 0);
}

QTEST_APPLESS_MAIN(RemoteFileOperationsTest)

#include "test_remote_file_operations.moc"
