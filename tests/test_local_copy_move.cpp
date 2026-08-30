#include "remotefilemanager/core/LocalFileSystem.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <filesystem>
#include <functional>

namespace
{

bool writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

QString pathText(const std::filesystem::path& path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.native());
#else
    const std::string native = path.native();
    return QFile::decodeName(QByteArray(native.data(), static_cast<qsizetype>(native.size())));
#endif
}

bool createRawSymbolicLink(const QString& target, const QString& path, bool directory = false)
{
    std::error_code error;
#ifdef Q_OS_WIN
    if (directory) {
        std::filesystem::create_directory_symlink(nativePath(target), nativePath(path), error);
    } else {
        std::filesystem::create_symlink(nativePath(target), nativePath(path), error);
    }
#else
    static_cast<void>(directory);
    std::filesystem::create_symlink(nativePath(target), nativePath(path), error);
#endif
    return !error;
}

QString rawSymbolicLinkTarget(const QString& path)
{
    std::error_code error;
    const std::filesystem::path target = std::filesystem::read_symlink(nativePath(path), error);
    return error ? QString{} : pathText(target);
}

rfm::core::LocalFileOperationRequest
request(quint64 id, rfm::core::LocalFileOperationKind kind, const QString& sourceDirectory,
        const QStringList& sources, const QString& destinationDirectory,
        rfm::core::LocalCollisionPolicy collisionPolicy = rfm::core::LocalCollisionPolicy::Fail)
{
    return {id, kind, sourceDirectory, sources, {}, destinationDirectory, collisionPolicy};
}

QStringList temporaryEntries(const QString& path)
{
    QStringList entries;
    for (const QFileInfo& info : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                          QDir::Hidden | QDir::System)) {
        if (info.fileName().contains(QStringLiteral(".rfm-"))) {
            entries.push_back(info.fileName());
        }
    }
    return entries;
}

class ControlledLocalBackend final : public rfm::core::LocalFileOperationBackend
{
  public:
    rfm::core::LocalRenameResult rename(const QString& source, const QString& destination) override
    {
        renames.push_back({source, destination});
        if (!crossDeviceReported && source == crossDeviceSource) {
            crossDeviceReported = true;
            return {rfm::core::LocalRenameError::CrossDevice,
                    QStringLiteral("different filesystems")};
        }
        if (source == failedRenameSource) {
            return {rfm::core::LocalRenameError::Failure, QStringLiteral("rename denied")};
        }
        if (!QDir().rename(source, destination)) {
            return {rfm::core::LocalRenameError::Failure, QStringLiteral("rename failed")};
        }
        if (afterSuccessfulRename) {
            afterSuccessfulRename(source, destination);
        }
        return {};
    }

    bool validateCopy(const QString& source, const QString& destination, QString& error) override
    {
        validations.push_back({source, destination});
        if (rejectValidation) {
            error = QStringLiteral("forced validation failure");
            return false;
        }
        if (afterValidation) {
            afterValidation();
        }
        return true;
    }

    QString crossDeviceSource;
    QString failedRenameSource;
    bool crossDeviceReported{false};
    bool rejectValidation{false};
    std::function<void()> afterValidation;
    std::function<void(const QString&, const QString&)> afterSuccessfulRename;
    QList<QPair<QString, QString>> renames;
    QList<QPair<QString, QString>> validations;
};

} // namespace

class LocalCopyMoveTest final : public QObject
{
    Q_OBJECT

  private slots:
    void copiesFile();
    void copiesMultipleFiles();
    void copiesDirectoryRecursively();
    void movesFileWithDirectRename();
    void movesDirectoryWithDirectRename();
    void reportsAndResolvesCollisionsExplicitly();
    void cancelsDuringLargeFileCopyAndCleansPartialData();
    void workerCancellationTargetsRequestedOperation();
    void workerRetainsMultipleCancellationRequests();
    void continuesAfterAnItemError();
    void movesAcrossFilesystemsOnlyAfterValidatedCopy();
    void preservesSourceWhenCrossFilesystemValidationFails();
    void doesNotFallbackForOrdinaryRenameFailure();
    void copiesSymbolicLinkWithoutFollowingRecursiveTarget();
    void refusesCopyIntoSourceBeforeCreatingStaging();
    void refusesMoveIntoSourceBeforeCreatingStaging();
    void refusesDestinationAliasedInsideSource();
    void cancellationBeforeOverwritePublicationRestoresEverything();
    void cancellationAfterPublicationCompletesMove();
    void preservesRawSymbolicLinkTargets();
    void crossFilesystemMovePreservesRelativeSymbolicLink();
    void changedSourceIsNotDeletedAfterPublication();
};

void LocalCopyMoveTest::copiesFile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/report.txt"));
    const QByteArray contents("local copy contents");
    QVERIFY(writeFile(source, contents));

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(1, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
                {source}, root.filePath(QStringLiteral("destination"))));

    QVERIFY2(result.allSucceeded(), qPrintable(result.items.constFirst().error));
    QCOMPARE(result.succeededCount(), qsizetype{1});
    QVERIFY(QFileInfo(source).isFile());
    QCOMPARE(readFile(root.filePath(QStringLiteral("destination/report.txt"))), contents);
    QVERIFY(temporaryEntries(root.filePath(QStringLiteral("destination"))).isEmpty());
}

void LocalCopyMoveTest::copiesMultipleFiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString first = root.filePath(QStringLiteral("source/first.txt"));
    const QString second = root.filePath(QStringLiteral("source/second.txt"));
    QVERIFY(writeFile(first, QByteArrayLiteral("first")));
    QVERIFY(writeFile(second, QByteArrayLiteral("second")));

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(2, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
                {first, second}, root.filePath(QStringLiteral("destination"))));

    QVERIFY(result.allSucceeded());
    QCOMPARE(result.items.size(), 2);
    QCOMPARE(readFile(root.filePath(QStringLiteral("destination/first.txt"))),
             QByteArrayLiteral("first"));
    QCOMPARE(readFile(root.filePath(QStringLiteral("destination/second.txt"))),
             QByteArrayLiteral("second"));
}

void LocalCopyMoveTest::copiesDirectoryRecursively()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("source/photos/album")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/photos"));
    QVERIFY(writeFile(root.filePath(QStringLiteral("source/photos/cover.jpg")),
                      QByteArrayLiteral("cover")));
    QVERIFY(writeFile(root.filePath(QStringLiteral("source/photos/album/image.jpg")),
                      QByteArrayLiteral("image")));

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(3, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
                {source}, root.filePath(QStringLiteral("destination"))));

    QVERIFY2(result.allSucceeded(), qPrintable(result.items.constFirst().error));
    QCOMPARE(readFile(root.filePath(QStringLiteral("destination/photos/cover.jpg"))),
             QByteArrayLiteral("cover"));
    QCOMPARE(readFile(root.filePath(QStringLiteral("destination/photos/album/image.jpg"))),
             QByteArrayLiteral("image"));
}

void LocalCopyMoveTest::movesFileWithDirectRename()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/movie.mkv"));
    const QString destination = root.filePath(QStringLiteral("destination/movie.mkv"));
    QVERIFY(writeFile(source, QByteArrayLiteral("movie")));
    ControlledLocalBackend backend;

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(4, rfm::core::LocalFileOperationKind::Move, root.filePath(QStringLiteral("source")),
                {source}, root.filePath(QStringLiteral("destination"))),
        &backend);

    QVERIFY(result.allSucceeded());
    QVERIFY(!QFileInfo(source).exists());
    QCOMPARE(readFile(destination), QByteArrayLiteral("movie"));
    QCOMPARE(backend.renames.size(), 1);
    QVERIFY(backend.validations.isEmpty());
}

void LocalCopyMoveTest::movesDirectoryWithDirectRename()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("source/folder/nested")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/folder"));
    QVERIFY(writeFile(root.filePath(QStringLiteral("source/folder/nested/data.txt")),
                      QByteArrayLiteral("data")));

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(5, rfm::core::LocalFileOperationKind::Move, root.filePath(QStringLiteral("source")),
                {source}, root.filePath(QStringLiteral("destination"))));

    QVERIFY2(result.allSucceeded(), qPrintable(result.items.constFirst().error));
    QVERIFY(!QFileInfo(source).exists());
    QCOMPARE(readFile(root.filePath(QStringLiteral("destination/folder/nested/data.txt"))),
             QByteArrayLiteral("data"));
}

void LocalCopyMoveTest::reportsAndResolvesCollisionsExplicitly()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/item.txt"));
    const QString destination = root.filePath(QStringLiteral("destination/item.txt"));
    QVERIFY(writeFile(source, QByteArrayLiteral("new")));
    QVERIFY(writeFile(destination, QByteArrayLiteral("old")));

    const auto collision = rfm::core::LocalFileSystem::executeOperation(
        request(6, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
                {source}, root.filePath(QStringLiteral("destination"))));
    QVERIFY(!collision.allSucceeded());
    QCOMPARE(collision.items.constFirst().outcome, rfm::core::LocalFileOperationOutcome::Collision);
    QCOMPARE(readFile(destination), QByteArrayLiteral("old"));

    const auto skipped = rfm::core::LocalFileSystem::executeOperation(
        request(7, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
                {source}, root.filePath(QStringLiteral("destination")),
                rfm::core::LocalCollisionPolicy::Skip));
    QCOMPARE(skipped.skippedCount(), qsizetype{1});
    QCOMPARE(readFile(destination), QByteArrayLiteral("old"));

    const auto cancelled = rfm::core::LocalFileSystem::executeOperation(
        request(8, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
                {source}, root.filePath(QStringLiteral("destination")),
                rfm::core::LocalCollisionPolicy::Cancel));
    QVERIFY(cancelled.cancelled);
    QCOMPARE(cancelled.items.constFirst().outcome, rfm::core::LocalFileOperationOutcome::Cancelled);
    QCOMPARE(readFile(destination), QByteArrayLiteral("old"));

    const auto overwritten = rfm::core::LocalFileSystem::executeOperation(
        request(8, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
                {source}, root.filePath(QStringLiteral("destination")),
                rfm::core::LocalCollisionPolicy::Overwrite));
    QVERIFY2(overwritten.allSucceeded(), qPrintable(overwritten.items.constFirst().error));
    QCOMPARE(readFile(destination), QByteArrayLiteral("new"));
    QVERIFY(temporaryEntries(root.filePath(QStringLiteral("destination"))).isEmpty());
}

void LocalCopyMoveTest::cancelsDuringLargeFileCopyAndCleansPartialData()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/large.bin"));
    QVERIFY(writeFile(source, QByteArray(2 * 1024 * 1024, 'x')));
    int cancellationChecks = 0;

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(9, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
                {source}, root.filePath(QStringLiteral("destination"))),
        nullptr, [&cancellationChecks] { return ++cancellationChecks >= 5; });

    QVERIFY(result.cancelled);
    QCOMPARE(result.items.constFirst().outcome, rfm::core::LocalFileOperationOutcome::Cancelled);
    QVERIFY(QFileInfo(source).exists());
    QVERIFY(!QFileInfo(root.filePath(QStringLiteral("destination/large.bin"))).exists());
    QVERIFY(temporaryEntries(root.filePath(QStringLiteral("destination"))).isEmpty());
}

void LocalCopyMoveTest::workerCancellationTargetsRequestedOperation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/pending.bin"));
    QVERIFY(writeFile(source, QByteArrayLiteral("pending")));
    rfm::core::LocalFileOperationWorker worker;
    QSignalSpy finished(&worker, &rfm::core::LocalFileOperationWorker::finished);

    worker.requestCancellation(20);
    worker.execute(request(20, rfm::core::LocalFileOperationKind::Copy,
                           root.filePath(QStringLiteral("source")), {source},
                           root.filePath(QStringLiteral("destination"))));

    QCOMPARE(finished.size(), 1);
    const auto result =
        finished.constFirst().constFirst().value<rfm::core::LocalFileOperationResult>();
    QVERIFY(result.cancelled);
    QVERIFY(QFileInfo(source).exists());
    QVERIFY(!QFileInfo(root.filePath(QStringLiteral("destination/pending.bin"))).exists());
}

void LocalCopyMoveTest::workerRetainsMultipleCancellationRequests()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString first = root.filePath(QStringLiteral("source/first.bin"));
    const QString second = root.filePath(QStringLiteral("source/second.bin"));
    QVERIFY(writeFile(first, QByteArray(32 * 1024 * 1024, 'a')));
    QVERIFY(writeFile(second, QByteArrayLiteral("second")));
    QThread thread;
    auto* const worker = new rfm::core::LocalFileOperationWorker;
    worker->moveToThread(&thread);
    connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    QSignalSpy started(worker, &rfm::core::LocalFileOperationWorker::started);
    QSignalSpy finished(worker, &rfm::core::LocalFileOperationWorker::finished);
    thread.start();
    QVERIFY(QMetaObject::invokeMethod(
        worker, "execute", Qt::QueuedConnection,
        Q_ARG(rfm::core::LocalFileOperationRequest,
              request(30, rfm::core::LocalFileOperationKind::Copy,
                      root.filePath(QStringLiteral("source")), {first},
                      root.filePath(QStringLiteral("destination"))))));
    QVERIFY(QMetaObject::invokeMethod(
        worker, "execute", Qt::QueuedConnection,
        Q_ARG(rfm::core::LocalFileOperationRequest,
              request(31, rfm::core::LocalFileOperationKind::Copy,
                      root.filePath(QStringLiteral("source")), {second},
                      root.filePath(QStringLiteral("destination"))))));
    QTRY_COMPARE(started.size(), 1);
    worker->requestCancellation(30);
    worker->requestCancellation(31);
    QTRY_COMPARE(finished.size(), 2);
    thread.quit();
    QVERIFY(thread.wait());
    for (const auto& arguments : finished) {
        QVERIFY(arguments.constFirst().value<rfm::core::LocalFileOperationResult>().cancelled);
    }
}

void LocalCopyMoveTest::continuesAfterAnItemError()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString missing = root.filePath(QStringLiteral("source/missing.txt"));
    const QString existing = root.filePath(QStringLiteral("source/existing.txt"));
    QVERIFY(writeFile(existing, QByteArrayLiteral("exists")));

    const auto result = rfm::core::LocalFileSystem::executeOperation(request(
        10, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
        {missing, existing}, root.filePath(QStringLiteral("destination"))));

    QVERIFY(!result.allSucceeded());
    QCOMPARE(result.failedCount(), qsizetype{1});
    QCOMPARE(result.succeededCount(), qsizetype{1});
    QCOMPARE(result.items.constFirst().source, missing);
    QVERIFY(!result.items.constFirst().error.isEmpty());
    QCOMPARE(readFile(root.filePath(QStringLiteral("destination/existing.txt"))),
             QByteArrayLiteral("exists"));
}

void LocalCopyMoveTest::movesAcrossFilesystemsOnlyAfterValidatedCopy()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    QVERIFY(root.mkpath(QStringLiteral("source/archive/nested")));
    const QString source = root.filePath(QStringLiteral("source/archive"));
    const QString destination = root.filePath(QStringLiteral("destination/archive"));
    QVERIFY(writeFile(root.filePath(QStringLiteral("source/archive/nested/data.txt")),
                      QByteArrayLiteral("archive")));
    ControlledLocalBackend backend;
    backend.crossDeviceSource = source;

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(11, rfm::core::LocalFileOperationKind::Move,
                root.filePath(QStringLiteral("source")), {source},
                root.filePath(QStringLiteral("destination"))),
        &backend);

    QVERIFY2(result.allSucceeded(), qPrintable(result.items.constFirst().error));
    QVERIFY(backend.crossDeviceReported);
    QCOMPARE(backend.validations.size(), 1);
    QVERIFY(!QFileInfo(source).exists());
    QCOMPARE(readFile(QDir(destination).filePath(QStringLiteral("nested/data.txt"))),
             QByteArrayLiteral("archive"));
}

void LocalCopyMoveTest::preservesSourceWhenCrossFilesystemValidationFails()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/important.txt"));
    const QString destination = root.filePath(QStringLiteral("destination/important.txt"));
    QVERIFY(writeFile(source, QByteArrayLiteral("important")));
    ControlledLocalBackend backend;
    backend.crossDeviceSource = source;
    backend.rejectValidation = true;

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(12, rfm::core::LocalFileOperationKind::Move,
                root.filePath(QStringLiteral("source")), {source},
                root.filePath(QStringLiteral("destination"))),
        &backend);

    QVERIFY(!result.allSucceeded());
    QVERIFY(result.items.constFirst().error.contains(QStringLiteral("validation")));
    QCOMPARE(readFile(source), QByteArrayLiteral("important"));
    QVERIFY(!QFileInfo(destination).exists());
    QVERIFY(temporaryEntries(root.filePath(QStringLiteral("destination"))).isEmpty());
}

void LocalCopyMoveTest::doesNotFallbackForOrdinaryRenameFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/protected.txt"));
    const QString destination = root.filePath(QStringLiteral("destination/protected.txt"));
    QVERIFY(writeFile(source, QByteArrayLiteral("protected")));
    ControlledLocalBackend backend;
    backend.failedRenameSource = source;

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(13, rfm::core::LocalFileOperationKind::Move,
                root.filePath(QStringLiteral("source")), {source},
                root.filePath(QStringLiteral("destination"))),
        &backend);

    QVERIFY(!result.allSucceeded());
    QVERIFY(result.items.constFirst().error.contains(QStringLiteral("denied")));
    QVERIFY(backend.validations.isEmpty());
    QCOMPARE(readFile(source), QByteArrayLiteral("protected"));
    QVERIFY(!QFileInfo(destination).exists());
}

void LocalCopyMoveTest::copiesSymbolicLinkWithoutFollowingRecursiveTarget()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("source/tree")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/tree"));
    QVERIFY(writeFile(root.filePath(QStringLiteral("source/tree/data.txt")),
                      QByteArrayLiteral("data")));
    const QString loop = root.filePath(QStringLiteral("source/tree/loop"));
    if (!QFile::link(source, loop)) {
        QSKIP("Symbolic links are not available in this test environment.");
    }

    const auto result = rfm::core::LocalFileSystem::executeOperation(request(
        14, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
        {source}, root.filePath(QStringLiteral("destination"))));

    QVERIFY2(result.allSucceeded(), qPrintable(result.items.constFirst().error));
    const QString copiedLoop = root.filePath(QStringLiteral("destination/tree/loop"));
    QVERIFY(QFileInfo(copiedLoop).isSymbolicLink());
    QCOMPARE(QDir(root.filePath(QStringLiteral("destination/tree")))
                 .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)
                 .size(),
             2);
}

void LocalCopyMoveTest::refusesCopyIntoSourceBeforeCreatingStaging()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("foo/bar")));
    const QString source = root.filePath(QStringLiteral("foo"));

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(15, rfm::core::LocalFileOperationKind::Copy, root.path(), {source},
                root.filePath(QStringLiteral("foo/bar"))));

    QVERIFY(!result.allSucceeded());
    QVERIFY(result.items.constFirst().error.contains(QStringLiteral("inside itself")));
    QVERIFY(temporaryEntries(root.filePath(QStringLiteral("foo/bar"))).isEmpty());
    QVERIFY(QFileInfo(source).isDir());
}

void LocalCopyMoveTest::refusesMoveIntoSourceBeforeCreatingStaging()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("foo/bar")));
    const QString source = root.filePath(QStringLiteral("foo"));

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(16, rfm::core::LocalFileOperationKind::Move, root.path(), {source},
                root.filePath(QStringLiteral("foo/bar"))));

    QVERIFY(!result.allSucceeded());
    QVERIFY(result.items.constFirst().error.contains(QStringLiteral("inside itself")));
    QVERIFY(temporaryEntries(root.filePath(QStringLiteral("foo/bar"))).isEmpty());
    QVERIFY(QFileInfo(source).isDir());
}

void LocalCopyMoveTest::refusesDestinationAliasedInsideSource()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("foo/bar")));
    const QString source = root.filePath(QStringLiteral("foo"));
    const QString alias = root.filePath(QStringLiteral("alias"));
    if (!createRawSymbolicLink(root.filePath(QStringLiteral("foo/bar")), alias, true)) {
        QSKIP("Symbolic links are not available in this test environment.");
    }
    ControlledLocalBackend backend;

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(17, rfm::core::LocalFileOperationKind::Copy, root.path(), {source}, alias),
        &backend);

    QVERIFY(!result.allSucceeded());
    QVERIFY(result.items.constFirst().error.contains(QStringLiteral("inside itself")));
    QVERIFY(backend.renames.isEmpty());
    QVERIFY(backend.validations.isEmpty());
    QVERIFY(temporaryEntries(root.filePath(QStringLiteral("foo/bar"))).isEmpty());

    const QString parentAlias = root.filePath(QStringLiteral("parent-alias"));
    if (!createRawSymbolicLink(root.path(), parentAlias, true)) {
        QSKIP("A second symbolic link could not be created in this test environment.");
    }
    const auto identicalAlias = rfm::core::LocalFileSystem::executeOperation(
        request(171, rfm::core::LocalFileOperationKind::Copy, root.path(), {source}, parentAlias,
                rfm::core::LocalCollisionPolicy::Overwrite),
        &backend);
    QVERIFY(!identicalAlias.allSucceeded());
    QVERIFY(backend.renames.isEmpty());
    QVERIFY(QFileInfo(source).isDir());
}

void LocalCopyMoveTest::cancellationBeforeOverwritePublicationRestoresEverything()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/item.txt"));
    const QString destination = root.filePath(QStringLiteral("destination/item.txt"));
    QVERIFY(writeFile(source, QByteArrayLiteral("new")));
    QVERIFY(writeFile(destination, QByteArrayLiteral("old")));
    ControlledLocalBackend backend;
    backend.crossDeviceSource = source;
    bool cancelled = false;
    backend.afterValidation = [&cancelled] { cancelled = true; };

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(18, rfm::core::LocalFileOperationKind::Move,
                root.filePath(QStringLiteral("source")), {source},
                root.filePath(QStringLiteral("destination")),
                rfm::core::LocalCollisionPolicy::Overwrite),
        &backend, [&cancelled] { return cancelled; });

    QVERIFY(result.cancelled);
    QCOMPARE(result.items.constFirst().outcome, rfm::core::LocalFileOperationOutcome::Cancelled);
    QCOMPARE(readFile(source), QByteArrayLiteral("new"));
    QCOMPARE(readFile(destination), QByteArrayLiteral("old"));
    QVERIFY(temporaryEntries(root.filePath(QStringLiteral("destination"))).isEmpty());
}

void LocalCopyMoveTest::cancellationAfterPublicationCompletesMove()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/item.txt"));
    const QString destination = root.filePath(QStringLiteral("destination/item.txt"));
    QVERIFY(writeFile(source, QByteArrayLiteral("new")));
    QVERIFY(writeFile(destination, QByteArrayLiteral("old")));
    ControlledLocalBackend backend;
    backend.crossDeviceSource = source;
    bool cancelled = false;
    backend.afterSuccessfulRename = [&cancelled, &destination](const QString& renamedSource,
                                                               const QString& renamedDestination) {
        if (renamedDestination == destination &&
            renamedSource.contains(QStringLiteral(".rfm-copy-"))) {
            cancelled = true;
        }
    };

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(19, rfm::core::LocalFileOperationKind::Move,
                root.filePath(QStringLiteral("source")), {source},
                root.filePath(QStringLiteral("destination")),
                rfm::core::LocalCollisionPolicy::Overwrite),
        &backend, [&cancelled] { return cancelled; });

    QVERIFY2(result.allSucceeded(), qPrintable(result.items.constFirst().error));
    QVERIFY(!result.cancelled);
    QVERIFY(!QFileInfo(source).exists());
    QCOMPARE(readFile(destination), QByteArrayLiteral("new"));
    QVERIFY(temporaryEntries(root.filePath(QStringLiteral("destination"))).isEmpty());
}

void LocalCopyMoveTest::preservesRawSymbolicLinkTargets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("source/tree")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    QVERIFY(writeFile(root.filePath(QStringLiteral("source/tree/data.txt")),
                      QByteArrayLiteral("data")));
    const QString relativeLink = root.filePath(QStringLiteral("source/tree/relative"));
    const QString absoluteLink = root.filePath(QStringLiteral("source/tree/absolute"));
    const QString brokenLink = root.filePath(QStringLiteral("source/tree/broken"));
    const QString absoluteTarget = root.filePath(QStringLiteral("source/tree/data.txt"));
    if (!createRawSymbolicLink(QStringLiteral("data.txt"), relativeLink) ||
        !createRawSymbolicLink(absoluteTarget, absoluteLink) ||
        !createRawSymbolicLink(QStringLiteral("missing.txt"), brokenLink)) {
        QSKIP("Symbolic links are not available in this test environment.");
    }

    const auto result = rfm::core::LocalFileSystem::executeOperation(request(
        20, rfm::core::LocalFileOperationKind::Copy, root.filePath(QStringLiteral("source")),
        {root.filePath(QStringLiteral("source/tree"))},
        root.filePath(QStringLiteral("destination"))));

    QVERIFY2(result.allSucceeded(), qPrintable(result.items.constFirst().error));
    const QString copiedTree = root.filePath(QStringLiteral("destination/tree"));
    QCOMPARE(rawSymbolicLinkTarget(QDir(copiedTree).filePath(QStringLiteral("relative"))),
             QStringLiteral("data.txt"));
    QCOMPARE(rawSymbolicLinkTarget(QDir(copiedTree).filePath(QStringLiteral("absolute"))),
             absoluteTarget);
    QCOMPARE(rawSymbolicLinkTarget(QDir(copiedTree).filePath(QStringLiteral("broken"))),
             QStringLiteral("missing.txt"));
    QCOMPARE(readFile(QDir(copiedTree).filePath(QStringLiteral("relative"))),
             QByteArrayLiteral("data"));
}

void LocalCopyMoveTest::crossFilesystemMovePreservesRelativeSymbolicLink()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("source/tree")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/tree"));
    QVERIFY(
        writeFile(QDir(source).filePath(QStringLiteral("data.txt")), QByteArrayLiteral("data")));
    if (!createRawSymbolicLink(QStringLiteral("data.txt"),
                               QDir(source).filePath(QStringLiteral("relative")))) {
        QSKIP("Symbolic links are not available in this test environment.");
    }
    ControlledLocalBackend backend;
    backend.crossDeviceSource = source;

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(21, rfm::core::LocalFileOperationKind::Move,
                root.filePath(QStringLiteral("source")), {source},
                root.filePath(QStringLiteral("destination"))),
        &backend);

    QVERIFY2(result.allSucceeded(), qPrintable(result.items.constFirst().error));
    const QString movedTree = root.filePath(QStringLiteral("destination/tree"));
    QCOMPARE(rawSymbolicLinkTarget(QDir(movedTree).filePath(QStringLiteral("relative"))),
             QStringLiteral("data.txt"));
    QCOMPARE(readFile(QDir(movedTree).filePath(QStringLiteral("relative"))),
             QByteArrayLiteral("data"));
    QVERIFY(!QFileInfo(source).exists());
}

void LocalCopyMoveTest::changedSourceIsNotDeletedAfterPublication()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    const QString source = root.filePath(QStringLiteral("source/item.txt"));
    const QString destination = root.filePath(QStringLiteral("destination/item.txt"));
    QVERIFY(writeFile(source, QByteArrayLiteral("original")));
    ControlledLocalBackend backend;
    backend.crossDeviceSource = source;
    backend.afterSuccessfulRename = [&source, &destination](const QString& renamedSource,
                                                            const QString& renamedDestination) {
        if (renamedDestination == destination &&
            renamedSource.contains(QStringLiteral(".rfm-copy-"))) {
            static_cast<void>(writeFile(source, QByteArrayLiteral("modified")));
        }
    };

    const auto result = rfm::core::LocalFileSystem::executeOperation(
        request(22, rfm::core::LocalFileOperationKind::Move,
                root.filePath(QStringLiteral("source")), {source},
                root.filePath(QStringLiteral("destination"))),
        &backend);

    QVERIFY(!result.allSucceeded());
    QVERIFY(result.items.constFirst().error.contains(QStringLiteral("source changed")));
    QVERIFY(result.items.constFirst().error.contains(QStringLiteral("source was preserved")));
    QCOMPARE(readFile(source), QByteArrayLiteral("modified"));
    QCOMPARE(readFile(destination), QByteArrayLiteral("original"));
    QVERIFY(temporaryEntries(root.filePath(QStringLiteral("destination"))).isEmpty());
}

QTEST_MAIN(LocalCopyMoveTest)

#include "test_local_copy_move.moc"
