#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/RemotePath.hpp"
#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

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

class RemoteFileOperationsTest final : public QObject {
    Q_OBJECT

private slots:
    void validatesAndNormalizesRemotePaths();
    void quotesCopyCommandWithoutInjection();
    void createsDirectoryAndReportsCollisionOrPermission();
    void renamesWithoutOverwriting();
    void movesSelectionAndReportsPartialFailure();
    void copiesOnServerOrReportsUnsupported();
    void removesFileAndRecursiveTreeWithGuards();
    void emitsWorkerOperationErrors();
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
             QStringLiteral("cp -n -- './a'\\''; touch /tmp/pwned; '\\''' './target/file'"));
    QCOMPARE(rfm::ssh::RemoteCopyCommand::build(
                 QStringLiteral("./folder"), QStringLiteral("/backup/folder"), true),
             QStringLiteral("cp -R -n -- './folder' '/backup/folder'"));
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

    backend.forced.remove(QStringLiteral("copy:src/folder"));
    const qsizetype callCount = backend.calls.size();
    const auto insideItself = operations.copy(
        8, {{QStringLiteral("src/folder"), true}}, QStringLiteral("src/folder/child"));
    QVERIFY(!insideItself.allSucceeded());
    QCOMPARE(backend.calls.size(), callCount);
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

QTEST_APPLESS_MAIN(RemoteFileOperationsTest)

#include "test_remote_file_operations.moc"
