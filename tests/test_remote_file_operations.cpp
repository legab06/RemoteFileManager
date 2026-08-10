#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/RemotePath.hpp"
#include "remotefilemanager/core/ServerSideCopyJob.hpp"
#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include <QFile>
#include <QHash>
#include <QSignalSpy>
#include <QTest>

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

    std::optional<rfm::core::RemoteBackendResult> pollCopy() override
    {
        ++pollCalls;
        if (completeAfterPolls > 0 && pollCalls >= completeAfterPolls) {
            active = false;
            return rfm::core::RemoteBackendResult{};
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
    QStringList started;
    int pollCalls{0};
    int cancellationRequestCalls{0};
    int cancellationPollCalls{0};
    int cleanupCalls{0};
    int completeAfterPolls{0};
    bool active{false};
    QList<std::optional<rfm::core::RemoteBackendResult>> cancellationRequestResponses;
    QList<std::optional<rfm::core::RemoteBackendResult>> cancellationPollResponses;
};

class RemoteFileOperationsTest final : public QObject {
    Q_OBJECT

private slots:
    void validatesAndNormalizesRemotePaths();
    void quotesCopyCommandWithoutInjection();
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
    QCOMPARE(rfm::ssh::RemoteCopyCommand::build(
                 QStringLiteral("./a'; touch /tmp/pwned; '"),
                 QStringLiteral("./target/file"), false),
             QStringLiteral("cp -P -n -- './a'\\''; touch /tmp/pwned; '\\''' './target/file'"));
    QCOMPARE(rfm::ssh::RemoteCopyCommand::build(
                 QStringLiteral("./folder"), QStringLiteral("/backup/folder"), true),
             QStringLiteral("cp -P -R -n -- './folder' '/backup/folder'"));
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
