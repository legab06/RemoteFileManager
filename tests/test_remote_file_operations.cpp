#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/RemoteMoveSafety.hpp"
#include "remotefilemanager/core/RemotePath.hpp"
#include "remotefilemanager/core/ServerSideCopyJob.hpp"
#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <optional>
#include <utility>

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

class FakeRemoteBackend final : public rfm::core::RemoteFileBackend {
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

    rfm::core::RemoteBackendResult rename(
        const QString& source, const QString& destination) override
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

    rfm::core::RemoteBackendResult copyOnServer(
        const QString& source, const QString& destination, bool recursive) override
    {
        calls.push_back(QStringLiteral("copy:%1:%2:%3")
                            .arg(source, destination, recursive ? QStringLiteral("r")
                                                               : QStringLiteral("f")));
        const auto result = forced.value(QStringLiteral("copy:%1").arg(source));
        if (result.succeeded()) {
            nodes.insert(destination, recursive);
        }
        return result;
    }

    rfm::core::RemoteBackendResult mutate(
        const QString& operation, const QString& path, bool directory)
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
        return {{rfm::core::RemoteBackendError::NotFound, {}}, {}};
    }

    rfm::core::RemoteBackendResult rename(const QString& source,
                                          const QString& destination) override
    {
        renamed.push_back(QStringLiteral("%1:%2").arg(source, destination));
        const rfm::core::RemoteBackendResult result =
            renamed.size() == 1 ? renameResult : promotionResult;
        if (renamed.size() == 1 && result.succeeded()) {
            sourceRemoved = true;
        }
        return result;
    }

    rfm::core::RemoteBackendResult startCopy(const QString& source, const QString& destination,
                                             bool recursive) override
    {
        started.push_back(QStringLiteral("%1:%2:%3")
                              .arg(source, destination,
                                   recursive ? QStringLiteral("recursive")
                                             : QStringLiteral("file")));
        active = true;
        return {};
    }

    rfm::core::RemoteBackendResult reserveMoveStaging(const QString& path) override
    {
        reserved.push_back(path);
        if (reserveResult.succeeded()) {
            stagingExists = true;
        }
        return reserveResult;
    }

    rfm::core::RemoteBackendResult startMoveStagingCopy(const QString& source,
                                                        const QString& destination) override
    {
        stagedCopies.push_back(QStringLiteral("%1:%2:archive").arg(source, destination));
        active = moveCopyStartResult.succeeded();
        return moveCopyStartResult;
    }

    std::optional<rfm::core::RemoteBackendResult> pollCopy() override
    {
        ++pollCalls;
        if (completeAfterPolls > 0 && pollCalls >= completeAfterPolls) {
            active = false;
            return copyCompletionResult;
        }
        return std::nullopt;
    }

    rfm::core::RemoteBackendResult startRemove(const QString& path, bool recursive,
                                               bool protectMountPoint) override
    {
        activeRemovePath = path;
        removed.push_back(
            QStringLiteral("%1:%2:%3")
                .arg(path, recursive ? QStringLiteral("recursive") : QStringLiteral("file"),
                     protectMountPoint ? QStringLiteral("protected")
                                       : QStringLiteral("unprotected")));
        const rfm::core::RemoteBackendResult result =
            path.contains(QStringLiteral(".rfm-move-")) ? cleanupStartResult : removeStartResult;
        active = result.succeeded();
        return result;
    }

    std::optional<rfm::core::RemoteBackendResult> pollRemove() override
    {
        ++removePollCalls;
        if (removeCompleteAfterPolls > 0 && removePollCalls >= removeCompleteAfterPolls) {
            active = false;
            const bool staging = activeRemovePath.contains(QStringLiteral(".rfm-move-"));
            const rfm::core::RemoteBackendResult result =
                staging ? cleanupCompletionResult : removeCompletionResult;
            if (result.succeeded()) {
                if (staging) {
                    stagingExists = false;
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
    QString activeRemovePath;
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
    rfm::core::RemoteBackendResult renameResult;
    rfm::core::RemoteBackendResult promotionResult;
    rfm::core::RemoteBackendResult reserveResult;
    rfm::core::RemoteBackendResult moveCopyStartResult;
    rfm::core::RemoteBackendResult copyCompletionResult;
    rfm::core::RemoteBackendResult cleanupStartResult;
    rfm::core::RemoteBackendResult cleanupCompletionResult;
    rfm::core::RemoteBackendResult removeStartResult;
    rfm::core::RemoteBackendResult removeCompletionResult;
    QList<std::optional<rfm::core::RemoteBackendResult>> cancellationRequestResponses;
    QList<std::optional<rfm::core::RemoteBackendResult>> cancellationPollResponses;
};

class LocalSymlinkMoveBackend final : public rfm::core::ServerSideCopyBackend
{
  public:
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
        if (renameCalls == 1) {
            return {rfm::core::RemoteBackendError::CrossDevice, {}};
        }
        promotionObserved = QFileInfo(source).isSymLink() && QFile::rename(source, destination);
        return promotionObserved
                   ? rfm::core::RemoteBackendResult{}
                   : rfm::core::RemoteBackendResult{rfm::core::RemoteBackendError::Failure,
                                                    QStringLiteral("Local promotion failed")};
    }

    rfm::core::RemoteBackendResult startCopy(const QString&, const QString&, bool) override
    {
        return {rfm::core::RemoteBackendError::Unsupported, {}};
    }

    rfm::core::RemoteBackendResult reserveMoveStaging(const QString& path) override
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

    rfm::core::RemoteBackendResult startMoveStagingCopy(const QString& source,
                                                        const QString& destination) override
    {
        copyStartedAfterReservation = stagingReserved && QFileInfo(stagingPath).isDir();
        copiedSourceWasSymlink = QFileInfo(source).isSymLink();
        QProcess process;
        process.start(QStringLiteral("/bin/sh"),
                      {QStringLiteral("-c"),
                       rfm::ssh::RemoteCopyCommand::buildMoveStaging(source, destination)});
        if (!process.waitForStarted() || !process.waitForFinished()) {
            copyResult = {rfm::core::RemoteBackendError::Failure,
                          QStringLiteral("Unable to run local cp")};
            return {};
        }
        const QByteArray standardOutput = process.readAllStandardOutput();
        const QByteArray standardError = process.readAllStandardError();
        reportedCopyStatus = rfm::ssh::RemoteCopyCommand::parseMoveStagingStatus(standardOutput);
        stagedLinkTarget = QFile::symLinkTarget(destination);
        copyResult =
            process.exitStatus() == QProcess::NormalExit && reportedCopyStatus == 0
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
    bool stagingRemovalProtected{false};
    bool sourceRemovalObservedAfterPromotion{false};
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
    void serverSideCopyRunsCooperatively();
    void serverSideCopyCancellationWaitsForTermination();
    void serverSideCopyCancellationRetriesAndReportsFailure();
    void serverSideCopyCancellationKeepsShutdownResponsive();
    void serverSideCopyFinalCompletionWinsOverCancellation();
    void serverSideCopyCanCancelWithSourcesRemaining();
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
    void removesFileAndRecursiveTreeWithGuards();
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
    QCOMPARE(rfm::ssh::RemoteCopyCommand::build(QStringLiteral("./a'; touch /tmp/pwned; '"),
                                                QStringLiteral("./target/file"), false),
             QStringLiteral("cp -P -n -- './a'\\''; touch /tmp/pwned; '\\''' './target/file'"));
    QCOMPARE(rfm::ssh::RemoteCopyCommand::build(QStringLiteral("./folder"),
                                                QStringLiteral("/backup/folder"), true),
             QStringLiteral("cp -P -R -n -- './folder' '/backup/folder'"));
    QCOMPARE(
        rfm::ssh::RemoteCopyCommand::buildRemove(QStringLiteral("./a'; touch /tmp/pwned; '"), true),
        QStringLiteral("rm -R -f -- './a'\\''; touch /tmp/pwned; '\\'''"));
    QCOMPARE(rfm::ssh::RemoteCopyCommand::buildMoveStaging(
                 QStringLiteral("./a'; touch /tmp/pwned; '"), QStringLiteral("./stage/item")),
             QStringLiteral("cp -a -- './a'\\''; touch /tmp/pwned; '\\''' './stage/item'; "
                            "rfm_copy_status=$?; "
                            "printf '\\nRFM_MOVE_COPY_STATUS:%s\\n' \"$rfm_copy_status\"; "
                            "exit \"$rfm_copy_status\""));
    QCOMPARE(rfm::ssh::RemoteCopyCommand::parseMoveStagingStatus(
                 QByteArrayLiteral("\nRFM_MOVE_COPY_STATUS:0\n")),
             std::optional<quint32>{0});
    QCOMPARE(rfm::ssh::RemoteCopyCommand::parseMoveStagingStatus(
                 QByteArrayLiteral("diagnostic\nRFM_MOVE_COPY_STATUS:23\n")),
             std::optional<quint32>{23});
    QVERIFY(!rfm::ssh::RemoteCopyCommand::parseMoveStagingStatus(
                 QByteArrayLiteral("RFM_MOVE_COPY_STATUS:0"))
                 .has_value());
    QVERIFY(!rfm::ssh::RemoteCopyCommand::parseMoveStagingStatus(
                 QByteArrayLiteral("spoof-RFM_MOVE_COPY_STATUS:0\n"))
                 .has_value());
    QVERIFY(!rfm::ssh::RemoteCopyCommand::parseMoveStagingStatus(
                 QByteArrayLiteral("RFM_MOVE_COPY_STATUS:256\n"))
                 .has_value());
    const QString guardedRemove = rfm::ssh::RemoteCopyCommand::buildRemove(
        QStringLiteral("/mnt/My Disk's"), true, true, QStringLiteral("/mnt/My Disk's"));
    QVERIFY(guardedRemove.contains(QStringLiteral("/proc/self/mountinfo")));
    QVERIFY(guardedRemove.contains(QStringLiteral("rfm_mountinfo_seen")));
    QVERIFY(guardedRemove.contains(QStringLiteral("rfm_mount_in_tree")));
    QVERIFY(guardedRemove.contains(QStringLiteral("rfm_device")));
    QVERIFY(guardedRemove.contains(QStringLiteral("rfm_separator_seen")));
    QVERIFY(guardedRemove.contains(QStringLiteral("[ \"$#\" -eq 3 ]")));
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

    QTest::newRow("source-is-mountpoint") << QStringLiteral("root") << false << 75 << QByteArray{};
    QTest::newRow("nested-mountpoint") << QStringLiteral("nested") << false << 75 << QByteArray{};
    QTest::newRow("nested-bind-mount") << QStringLiteral("bind") << false << 75 << QByteArray{};
    QTest::newRow("false-prefix") << QStringLiteral("false-prefix") << false << 0 << QByteArray{};
    QTest::newRow("special-nested-mountpoint")
        << QStringLiteral("nested") << true << 75 << QByteArray{};
    QTest::newRow("review-incomplete-record")
        << QStringLiteral("invalid") << false << 74 << QByteArrayLiteral("24 1 bogus bogus / rw");
    QTest::newRow("invalid-mount-id") << QStringLiteral("invalid") << false << 74
                                      << QByteArrayLiteral("x 1 8:1 / / rw - ext4 /dev/sda1 rw");
    QTest::newRow("invalid-parent-id") << QStringLiteral("invalid") << false << 74
                                       << QByteArrayLiteral("24 x 8:1 / / rw - ext4 /dev/sda1 rw");
    QTest::newRow("missing-separator") << QStringLiteral("invalid") << false << 74
                                       << QByteArrayLiteral("24 1 8:1 / / rw ext4 /dev/sda1 rw");
    QTest::newRow("invalid-major-minor")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:x / / rw - ext4 /dev/sda1 rw");
    QTest::newRow("missing-post-separator-field")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:1 / / rw - ext4 /dev/sda1");
    QTest::newRow("relative-mountpoint")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:1 / relative rw - ext4 /dev/sda1 rw");
    QTest::newRow("relative-root")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:1 relative / rw - ext4 /dev/sda1 rw");
    QTest::newRow("invalid-path-escape")
        << QStringLiteral("invalid") << false << 74
        << QByteArrayLiteral("24 1 8:1 / /bad\\999 rw - ext4 /dev/sda1 rw");
}

void RemoteFileOperationsTest::recursiveRemoveRefusesMountPointTrees()
{
    QFETCH(QString, relation);
    QFETCH(bool, specialPath);
    QFETCH(int, expectedExitCode);
    QFETCH(QByteArray, invalidRecord);

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString parent = temporaryDirectory.path() + QStringLiteral("/srv");
    const QString source =
        parent + (specialPath ? QStringLiteral("/data dir's [tree]") : QStringLiteral("/data"));
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
    if (!invalidRecord.isEmpty()) {
        invalidRecord.push_back('\n');
        QCOMPARE(mountInfo.write(invalidRecord), qint64{invalidRecord.size()});
    } else {
        const QByteArray mountRecord =
            QByteArrayLiteral("25 24 8:2 / ") + encodeMountInfoPath(mountPoint) +
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
    QVERIFY(implementation.contains("parseMoveStagingStatus(m_standardOutput)"));
}

void RemoteFileOperationsTest::serverSideCopyRunsCooperatively()
{
    FakeCopyBackend backend;
    backend.completeAfterPolls = 2;
    rfm::core::ServerSideCopyJob job(
        backend, 72, {{QStringLiteral("/source/tree"), true}},
        QStringLiteral("/destination"));

    job.step();
    QCOMPARE(job.progress().state, rfm::core::OperationState::Running);
    job.step();
    QCOMPARE(backend.started.size(), 1);
    QCOMPARE(backend.started.constFirst(),
             QStringLiteral("/source/tree:/destination/tree:recursive"));
    job.step();
    QVERIFY(!job.isFinished());
    job.step();
    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Completed);
    QCOMPARE(job.progress().completedItems, quint64{1});
}

void RemoteFileOperationsTest::serverSideCopyCancellationWaitsForTermination()
{
    FakeCopyBackend backend;
    backend.cancellationRequestResponses = {rfm::core::RemoteBackendResult{}};
    backend.cancellationPollResponses = {std::nullopt, rfm::core::RemoteBackendResult{}};
    rfm::core::ServerSideCopyJob job(
        backend, 73, {{QStringLiteral("/source/link"), false}},
        QStringLiteral("/destination"));
    job.step();
    job.step();

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
    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Cancelled);
    QCOMPARE(job.result().items.size(), 1);
    QVERIFY(job.result().items.at(0).error.contains(QStringLiteral("cancelled")));
}

void RemoteFileOperationsTest::serverSideCopyCancellationRetriesAndReportsFailure()
{
    FakeCopyBackend retryingBackend;
    retryingBackend.cancellationRequestResponses = {
        std::nullopt, std::nullopt, rfm::core::RemoteBackendResult{}};
    retryingBackend.cancellationPollResponses = {rfm::core::RemoteBackendResult{}};
    rfm::core::ServerSideCopyJob retrying(
        retryingBackend, 74, {{QStringLiteral("/source/file"), false}},
        QStringLiteral("/destination"));
    retrying.step();
    retrying.step();
    QVERIFY(retrying.requestCancel());
    retrying.step();
    retrying.step();
    QVERIFY(!retrying.isFinished());
    QVERIFY(retryingBackend.active);
    retrying.step();
    QVERIFY(!retrying.isFinished());
    QVERIFY(retryingBackend.active);
    retrying.step();
    QCOMPARE(retrying.progress().state, rfm::core::OperationState::Cancelled);
    QCOMPARE(retryingBackend.cancellationRequestCalls, 3);
    QCOMPARE(retryingBackend.cancellationPollCalls, 1);

    FakeCopyBackend failingBackend;
    failingBackend.cancellationRequestResponses = {
        rfm::core::RemoteBackendResult{rfm::core::RemoteBackendError::Failure,
                                       QStringLiteral("TERM delivery timed out")}};
    rfm::core::ServerSideCopyJob failing(
        failingBackend, 75, {{QStringLiteral("/source/file"), false}},
        QStringLiteral("/destination"));
    failing.step();
    failing.step();
    QVERIFY(failing.requestCancel());
    failing.step();
    QVERIFY(failing.isFinished());
    QCOMPARE(failing.progress().state, rfm::core::OperationState::Failed);
    QVERIFY(failing.progress().error.contains(QStringLiteral("TERM delivery timed out")));
    QVERIFY(failing.result().items.constFirst().error.contains(
        QStringLiteral("cancellation failed")));
}

void RemoteFileOperationsTest::serverSideCopyCancellationKeepsShutdownResponsive()
{
    FakeCopyBackend backend;
    backend.cancellationRequestResponses = {rfm::core::RemoteBackendResult{}};
    backend.cancellationPollResponses = {
        std::nullopt, std::nullopt,
        rfm::core::RemoteBackendResult{rfm::core::RemoteBackendError::Failure,
                                       QStringLiteral("Cancellation timed out")}};
    rfm::core::ServerSideCopyJob job(
        backend, 76, {{QStringLiteral("/source/file"), false}},
        QStringLiteral("/destination"));
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
}

void RemoteFileOperationsTest::serverSideCopyFinalCompletionWinsOverCancellation()
{
    FakeCopyBackend backend;
    backend.completeAfterPolls = 1;
    rfm::core::ServerSideCopyJob job(
        backend, 77, {{QStringLiteral("/source/file"), false}},
        QStringLiteral("/destination"));
    job.step();
    job.step();
    job.step();

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
        {{QStringLiteral("/source/first"), false},
         {QStringLiteral("/source/second"), false}},
        QStringLiteral("/destination"));
    job.step();
    job.step();
    job.step();

    QVERIFY(!job.isFinished());
    QVERIFY(job.requestCancel());
    QVERIFY(job.isFinished());
    QCOMPARE(job.progress().state, rfm::core::OperationState::Cancelled);
    QCOMPARE(job.progress().completedItems, quint64{1});
    QCOMPARE(job.result().items.size(), 2);
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
    QVERIFY(temporaryPath.startsWith(QStringLiteral("/destination/.tree.rfm-move-")));
    QVERIFY(temporaryPath.endsWith(QStringLiteral(".partial")));
    QCOMPARE(backend.stagedCopies,
             QStringList({QStringLiteral("/source/tree:%1/item:archive").arg(temporaryPath)}));
    QCOMPARE(backend.renamed.size(), 2);
    QCOMPARE(backend.renamed.constLast(),
             temporaryPath + QStringLiteral("/item:/destination/tree"));
    QCOMPARE(backend.removed, QStringList({temporaryPath + QStringLiteral(":recursive:protected"),
                                           QStringLiteral("/source/tree:recursive:protected")}));
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

    LocalSymlinkMoveBackend backend;
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
    rfm::core::ServerSideCopyJob job(backend, 86,
                                     {{QStringLiteral("/source/tree"), true}},
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
    rfm::core::ServerSideCopyJob job(backend, 87,
                                     {{QStringLiteral("/source/tree"), true}},
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
    QVERIFY(job.result().items.constFirst().error.contains(QStringLiteral("temporary destination")));
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

    const auto collision = operations.createDirectory(
        2, QStringLiteral("."), QStringLiteral("new"));
    QVERIFY(!collision.allSucceeded());
    QVERIFY(collision.items.constFirst().error.contains(QStringLiteral("exists")));

    backend.forced.insert(QStringLiteral("mkdir:./private"),
                          {rfm::core::RemoteBackendError::PermissionDenied, {}});
    const auto denied = operations.createDirectory(
        3, QStringLiteral("."), QStringLiteral("private"));
    QVERIFY(!denied.allSucceeded());
    QVERIFY(denied.items.constFirst().error.contains(QStringLiteral("Permission")));
}

void RemoteFileOperationsTest::renamesWithoutOverwriting()
{
    FakeRemoteBackend backend;
    backend.nodes.insert(QStringLiteral("docs/a.txt"), false);
    rfm::core::RemoteFileOperations operations(backend);
    const auto renamed = operations.rename(
        4, QStringLiteral("docs/a.txt"), QStringLiteral("b.txt"));
    QVERIFY(renamed.allSucceeded());
    QVERIFY(backend.calls.contains(QStringLiteral("rename:docs/a.txt:docs/b.txt")));

    backend.nodes.insert(QStringLiteral("docs/taken.txt"), false);
    const auto collision = operations.rename(
        5, QStringLiteral("docs/b.txt"), QStringLiteral("taken.txt"));
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
        6,
        {{QStringLiteral("src/a.txt"), false}, {QStringLiteral("src/b.txt"), false}},
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
        7,
        {{QStringLiteral("src/a.txt"), false}, {QStringLiteral("src/folder"), true}},
        QStringLiteral("backup"));
    QVERIFY(result.items.at(0).success);
    QVERIFY(!result.items.at(1).success);
    QVERIFY(result.items.at(1).error.contains(QStringLiteral("not supported")));
    QVERIFY(backend.calls.contains(QStringLiteral("copy:src/a.txt:backup/a.txt:f")));
    QVERIFY(backend.calls.contains(QStringLiteral("copy:src/folder:backup/folder:r")));

    const qsizetype beforeCollision = backend.calls.size();
    const auto collision = operations.copy(
        71, {{QStringLiteral("src/a.txt"), false}}, QStringLiteral("backup"));
    QVERIFY(!collision.allSucceeded());
    QVERIFY(collision.items.constFirst().error.contains(QStringLiteral("exists")));
    QCOMPARE(backend.calls.size(), beforeCollision + 1);
    QCOMPARE(backend.calls.constLast(), QStringLiteral("probe:backup/a.txt"));

    backend.forced.remove(QStringLiteral("copy:src/folder"));
    const qsizetype callCount = backend.calls.size();
    const auto insideItself = operations.copy(
        8, {{QStringLiteral("src/folder"), true}}, QStringLiteral("src/folder/child"));
    QVERIFY(!insideItself.allSucceeded());
    QCOMPARE(backend.calls.size(), callCount);

    const auto childToParent = operations.copy(
        9, {{QStringLiteral("tree/child/folder"), true}}, QStringLiteral("tree/./"));
    QVERIFY(childToParent.allSucceeded());
    QCOMPARE(childToParent.items.constFirst().destination, QStringLiteral("tree/folder"));
    QVERIFY(backend.calls.contains(QStringLiteral("copy:tree/child/folder:tree/folder:r")));

    const auto parentToChild = operations.move(
        10, {{QStringLiteral("/home/gabriel/tree/file.txt"), false}},
        QStringLiteral("/home/gabriel/tree/child/"));
    QVERIFY(parentToChild.allSucceeded());
    QCOMPARE(parentToChild.items.constFirst().destination,
             QStringLiteral("/home/gabriel/tree/child/file.txt"));
    QVERIFY(backend.calls.contains(QStringLiteral(
        "rename:/home/gabriel/tree/file.txt:/home/gabriel/tree/child/file.txt")));
}

void RemoteFileOperationsTest::removesFileAndRecursiveTreeWithGuards()
{
    FakeRemoteBackend backend;
    backend.nodes.insert(QStringLiteral("trash/file.txt"), false);
    backend.listings.insert(QStringLiteral("trash/folder"),
                            {{QStringLiteral("nested.txt"), false},
                             {QStringLiteral("child"), true}});
    backend.listings.insert(QStringLiteral("trash/folder/child"), {});
    rfm::core::RemoteFileOperations operations(backend);
    const auto removedFile = operations.remove(
        8, {{QStringLiteral("trash/file.txt"), false}}, false);
    QVERIFY(removedFile.allSucceeded());

    const auto removedTree = operations.remove(
        9, {{QStringLiteral("trash/folder"), true}}, true);
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
        11,
        {{QStringLiteral("trash/ok.txt"), false},
         {QStringLiteral("trash/denied.txt"), false}},
        false);
    QVERIFY(partial.items.at(0).success);
    QVERIFY(!partial.items.at(1).success);
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
