#include "remotefilemanager/core/TransferDirectoryJob.hpp"
#include "remotefilemanager/core/TransferFileJob.hpp"
#include "remotefilemanager/core/TransferJob.hpp"
#include "remotefilemanager/core/TransferQueue.hpp"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

namespace
{

constexpr qsizetype testSize = 200'000;

class FakeBackend final : public rfm::core::RemoteTransferBackend
{
  public:
    struct DirectoryCursor {
        QList<rfm::core::TransferDirectoryEntry> entries;
        qsizetype index{0};
    };

    rfm::core::TransferStatResult stat(const QString& path) override
    {
        ++statCalls;
        const bool symbolic = symbolicLinks.contains(path);
        const bool directory = directories.contains(path);
        const bool file = files.contains(path);
        return {{},
                {symbolic || directory || file, directory, symbolic,
                 static_cast<quint64>(files.value(path).size())}};
    }

    rfm::core::TransferBackendResult openRead(const QString& path, quint64& handle) override
    {
        ++openReadCalls;
        if (!files.contains(path)) {
            return {rfm::core::TransferBackendError::NotFound, {}};
        }
        handle = nextHandle++;
        openPath = path;
        offset = 0;
        return {};
    }

    rfm::core::TransferBackendResult openWriteExclusive(const QString& path,
                                                        quint64& handle) override
    {
        ++openWriteCalls;
        if (files.contains(path) || directories.contains(path) || symbolicLinks.contains(path)) {
            return {rfm::core::TransferBackendError::AlreadyExists, {}};
        }
        handle = nextHandle++;
        openPath = path;
        files.insert(path, {});
        return {};
    }

    rfm::core::TransferBackendResult read(quint64 /* handle */, QByteArray& data,
                                          qsizetype maximum) override
    {
        ++readCalls;
        if (failReadAt > 0 && readCalls == failReadAt) {
            return {rfm::core::TransferBackendError::Io, {}};
        }
        data = files.value(openPath).mid(offset, maximum);
        offset += data.size();
        return {};
    }

    rfm::core::TransferBackendResult write(quint64 /* handle */, const QByteArray& data) override
    {
        ++writeCalls;
        if (failWriteAt > 0 && writeCalls == failWriteAt) {
            return {rfm::core::TransferBackendError::Io, {}};
        }
        files[openPath] += data;
        return {};
    }

    rfm::core::TransferBackendResult close(quint64 /* handle */) override
    {
        ++closeCalls;
        return failClose ? rfm::core::TransferBackendResult{rfm::core::TransferBackendError::Io, {}}
                         : rfm::core::TransferBackendResult{};
    }

    rfm::core::TransferBackendResult createDirectory(const QString& path) override
    {
        if (files.contains(path) || directories.contains(path) || symbolicLinks.contains(path)) {
            return {rfm::core::TransferBackendError::AlreadyExists, {}};
        }
        directories.insert(path);
        return {};
    }

    rfm::core::TransferBackendResult rename(const QString& source,
                                            const QString& destination) override
    {
        ++renameCalls;
        if (files.contains(destination) || directories.contains(destination) ||
            symbolicLinks.contains(destination)) {
            return {rfm::core::TransferBackendError::AlreadyExists, {}};
        }
        files.insert(destination, files.take(source));
        return {};
    }

    rfm::core::TransferBackendResult remove(const QString& path) override
    {
        ++removeCalls;
        if (failRemove) {
            return {rfm::core::TransferBackendError::Io, {}};
        }
        if (!files.contains(path)) {
            return {rfm::core::TransferBackendError::NotFound, {}};
        }
        files.remove(path);
        return {};
    }

    rfm::core::TransferBackendResult openDirectory(const QString& path, quint64& handle) override
    {
        if (!directories.contains(path)) {
            return {rfm::core::TransferBackendError::NotFound, {}};
        }
        const QString prefix = path == QStringLiteral("/") ? path : path + QChar{'/'};
        QSet<QString> names;
        const auto collect = [&](const auto& paths) {
            for (const QString& candidate : paths) {
                if (!candidate.startsWith(prefix)) {
                    continue;
                }
                const QString remainder = candidate.mid(prefix.size());
                if (!remainder.isEmpty() && !remainder.contains(QChar{'/'})) {
                    names.insert(remainder);
                }
            }
        };
        collect(files.keys());
        collect(directories.values());
        collect(symbolicLinks.values());
        QStringList sorted = names.values();
        sorted.sort();
        DirectoryCursor cursor;
        for (const QString& name : sorted) {
            const QString child = prefix + name;
            cursor.entries.push_back(
                {name,
                 {true, directories.contains(child), symbolicLinks.contains(child),
                  static_cast<quint64>(files.value(child).size())}});
        }
        handle = nextHandle++;
        directoryHandles.insert(handle, cursor);
        return {};
    }

    rfm::core::TransferBackendResult
    readDirectory(quint64 handle, std::optional<rfm::core::TransferDirectoryEntry>& entry) override
    {
        ++readDirectoryCalls;
        auto iterator = directoryHandles.find(handle);
        if (iterator == directoryHandles.end()) {
            return {rfm::core::TransferBackendError::Failure, {}};
        }
        if (iterator->index >= iterator->entries.size()) {
            entry.reset();
            return {};
        }
        entry = iterator->entries.at(iterator->index++);
        return {};
    }

    rfm::core::TransferBackendResult closeDirectory(quint64 handle) override
    {
        if (directoryHandles.remove(handle) == 0) {
            return {rfm::core::TransferBackendError::Failure, {}};
        }
        return {};
    }

    QHash<QString, QByteArray> files;
    QSet<QString> directories{QStringLiteral("/")};
    QSet<QString> symbolicLinks;
    QHash<quint64, DirectoryCursor> directoryHandles;
    QString openPath;
    qsizetype offset{0};
    quint64 nextHandle{1};
    int statCalls{0};
    int openReadCalls{0};
    int openWriteCalls{0};
    int readCalls{0};
    int writeCalls{0};
    int closeCalls{0};
    int renameCalls{0};
    int removeCalls{0};
    int readDirectoryCalls{0};
    int failReadAt{0};
    int failWriteAt{0};
    bool failClose{false};
    bool failRemove{false};
};

void writeLocalFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
}

int runToEnd(rfm::core::TransferFileJob& job)
{
    int steps = 0;
    while (!job.isFinished()) {
        job.step();
        ++steps;
        if (steps >= 1000) {
            return -1;
        }
    }
    return steps;
}

int runDirectoryToEnd(rfm::core::TransferDirectoryJob& job)
{
    int steps = 0;
    while (!job.isFinished()) {
        job.step();
        ++steps;
        if (steps >= 1000) {
            return -1;
        }
    }
    return steps;
}

QByteArray readLocalFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

} // namespace

class StepDriver final : public QObject
{
    Q_OBJECT

  public:
    explicit StepDriver(rfm::core::TransferJob& job) : m_job(job) {}

    void start() { QMetaObject::invokeMethod(this, &StepDriver::advance, Qt::QueuedConnection); }

    void pause()
    {
        const bool paused = m_job.requestPause();
        Q_UNUSED(paused);
    }

    void resume()
    {
        if (m_job.resume()) {
            start();
        }
    }

    [[nodiscard]] int callbacks() const { return m_callbacks; }

  signals:
    void updated(rfm::core::TransferProgress progress);
    void terminal(rfm::core::TransferProgress progress);

  private:
    void advance()
    {
        ++m_callbacks;
        m_job.step();
        emit updated(m_job.progress());
        if (m_job.isFinished()) {
            emit terminal(m_job.progress());
        } else if (!m_job.isPaused()) {
            start();
        }
    }

    rfm::core::TransferJob& m_job;
    int m_callbacks{0};
};

class TransferFileJobTest final : public QObject
{
    Q_OBJECT

  private slots:
    void uploadsIncrementally();
    void downloadsIncrementally();
    void failsDuringUploadAndDownload();
    void pausesAndResumesAtSameOffset();
    void cancelsActiveTransferAndCleansTemporary();
    void cancelsWhilePaused();
    void queueIsFifoAndRejectsDuplicateIds();
    void queueContinuesAfterSuccessAndFailure();
    void queuedCancellationPreservesOrderAndPerformsNoIo();
    void eventLoopRunsOneStepPerCallback();
    void eventLoopPauseAndResume();
    void uploadsDirectoryRecursively();
    void downloadsDirectoryRecursively();
    void transfersEmptyDirectories();
    void rejectsLocalAndRemoteSymbolicLinks();
    void refusesDirectoryDestinationCollisions();
    void pausesAndResumesDirectoryChild();
    void cancelsDirectoryDuringTraversal();
    void cancelsDirectoryChildAndWhilePaused();
    void reportsPartialDirectoryFailure();
    void readsOneRemoteDirectoryEntryPerStep();
    void eventLoopRunsDirectoryIncrementally();
};

void TransferFileJobTest::uploadsIncrementally()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray data(testSize, 'u');
    const QString source = directory.filePath(QStringLiteral("upload.bin"));
    writeLocalFile(source, data);

    FakeBackend backend;
    rfm::core::TransferFileJob job(
        backend, {1, rfm::core::TransferDirection::Upload, source, QStringLiteral("/final")});
    quint64 previous = 0;
    while (job.progress().transferredBytes == 0) {
        job.step();
    }
    QCOMPARE(backend.writeCalls, 1);
    QCOMPARE(job.progress().transferredBytes, quint64{65'536});
    QVERIFY(!backend.files.contains(QStringLiteral("/final")));
    QVERIFY(backend.files.contains(QStringLiteral("/final.rfm-part-1")));

    while (!job.isFinished()) {
        QVERIFY(job.progress().transferredBytes >= previous);
        previous = job.progress().transferredBytes;
        job.step();
    }
    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QCOMPARE(job.progress().transferredBytes, quint64{testSize});
    QCOMPARE(backend.writeCalls, 4);
    QCOMPARE(backend.renameCalls, 1);
    QCOMPARE(backend.files.value(QStringLiteral("/final")), data);
    const int writes = backend.writeCalls;
    const quint64 transferred = job.progress().transferredBytes;
    job.step();
    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QCOMPARE(job.progress().transferredBytes, transferred);
    QCOMPARE(backend.writeCalls, writes);
}

void TransferFileJobTest::downloadsIncrementally()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray data(testSize, 'd');
    const QString destination = directory.filePath(QStringLiteral("download.bin"));
    FakeBackend backend;
    backend.files.insert(QStringLiteral("/source"), data);
    rfm::core::TransferFileJob job(backend, {2, rfm::core::TransferDirection::Download,
                                             QStringLiteral("/source"), destination});

    while (job.progress().transferredBytes == 0) {
        job.step();
    }
    QCOMPARE(backend.readCalls, 1);
    QCOMPARE(job.progress().transferredBytes, quint64{65'536});
    QVERIFY(!QFile::exists(destination));
    const int steps = runToEnd(job);
    QVERIFY(steps > 4);
    QCOMPARE(backend.readCalls, 5);
    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QFile result(destination);
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(result.readAll(), data);
}

void TransferFileJobTest::failsDuringUploadAndDownload()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray data(testSize, 'e');
    const QString uploadSource = directory.filePath(QStringLiteral("source.bin"));
    writeLocalFile(uploadSource, data);

    FakeBackend uploadBackend;
    uploadBackend.failWriteAt = 3;
    rfm::core::TransferFileJob upload(uploadBackend, {3, rfm::core::TransferDirection::Upload,
                                                      uploadSource, QStringLiteral("/failed")});
    runToEnd(upload);
    QCOMPARE(upload.progress().state, rfm::core::TransferState::Failed);
    QCOMPARE(upload.progress().transferredBytes, quint64{131'072});
    QVERIFY(!uploadBackend.files.contains(QStringLiteral("/failed")));
    QVERIFY(!uploadBackend.files.contains(QStringLiteral("/failed.rfm-part-3")));

    FakeBackend downloadBackend;
    downloadBackend.files.insert(QStringLiteral("/source"), data);
    downloadBackend.failReadAt = 3;
    const QString downloadDestination = directory.filePath(QStringLiteral("failed-download"));
    rfm::core::TransferFileJob download(downloadBackend,
                                        {4, rfm::core::TransferDirection::Download,
                                         QStringLiteral("/source"), downloadDestination});
    runToEnd(download);
    QCOMPARE(download.progress().state, rfm::core::TransferState::Failed);
    QCOMPARE(download.progress().transferredBytes, quint64{131'072});
    QVERIFY(!QFile::exists(downloadDestination));

    const int reads = downloadBackend.readCalls;
    const quint64 transferred = download.progress().transferredBytes;
    download.step();
    QCOMPARE(downloadBackend.readCalls, reads);
    QCOMPARE(download.progress().transferredBytes, transferred);
    QCOMPARE(download.progress().state, rfm::core::TransferState::Failed);
}

void TransferFileJobTest::pausesAndResumesAtSameOffset()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray data(testSize, 'p');
    const QString source = directory.filePath(QStringLiteral("source.bin"));
    writeLocalFile(source, data);
    FakeBackend backend;
    rfm::core::TransferFileJob job(
        backend, {5, rfm::core::TransferDirection::Upload, source, QStringLiteral("/paused")});

    while (job.progress().transferredBytes < 131'072) {
        job.step();
    }
    QVERIFY(job.requestPause());
    QCOMPARE(job.progress().state, rfm::core::TransferState::Paused);
    const quint64 pausedBytes = job.progress().transferredBytes;
    const int pausedWrites = backend.writeCalls;
    for (int iteration = 0; iteration < 5; ++iteration) {
        job.step();
    }
    QCOMPARE(job.progress().transferredBytes, pausedBytes);
    QCOMPARE(backend.writeCalls, pausedWrites);

    QVERIFY(job.resume());
    job.step();
    QCOMPARE(job.progress().transferredBytes, quint64{196'608});
    runToEnd(job);
    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QCOMPARE(backend.files.value(QStringLiteral("/paused")), data);
}

void TransferFileJobTest::cancelsActiveTransferAndCleansTemporary()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("source.bin"));
    writeLocalFile(source, QByteArray(testSize, 'c'));
    FakeBackend backend;
    rfm::core::TransferFileJob job(
        backend, {6, rfm::core::TransferDirection::Upload, source, QStringLiteral("/cancelled")});
    while (job.progress().transferredBytes < 131'072) {
        job.step();
    }
    QVERIFY(job.requestCancel());
    QCOMPARE(job.progress().state, rfm::core::TransferState::Cancelling);
    const int writes = backend.writeCalls;
    runToEnd(job);
    QCOMPARE(job.progress().state, rfm::core::TransferState::Cancelled);
    QCOMPARE(job.progress().transferredBytes, quint64{131'072});
    QCOMPARE(backend.writeCalls, writes);
    QVERIFY(!backend.files.contains(QStringLiteral("/cancelled")));
    QVERIFY(!backend.files.contains(QStringLiteral("/cancelled.rfm-part-6")));
    const quint64 transferred = job.progress().transferredBytes;
    job.step();
    QCOMPARE(job.progress().state, rfm::core::TransferState::Cancelled);
    QCOMPARE(job.progress().transferredBytes, transferred);
    QCOMPARE(backend.writeCalls, writes);
}

void TransferFileJobTest::cancelsWhilePaused()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("source.bin"));
    writeLocalFile(source, QByteArray(testSize, 'x'));
    FakeBackend backend;
    rfm::core::TransferFileJob job(
        backend, {7, rfm::core::TransferDirection::Upload, source, QStringLiteral("/paused")});
    while (job.progress().transferredBytes == 0) {
        job.step();
    }
    QVERIFY(job.requestPause());
    QVERIFY(job.requestCancel());
    runToEnd(job);
    QCOMPARE(job.progress().state, rfm::core::TransferState::Cancelled);
    QVERIFY(!backend.files.contains(QStringLiteral("/paused")));
    QVERIFY(!backend.files.contains(QStringLiteral("/paused.rfm-part-7")));
}

void TransferFileJobTest::queueIsFifoAndRejectsDuplicateIds()
{
    rfm::core::TransferQueue queue;
    const rfm::core::TransferRequest first{11, rfm::core::TransferDirection::Upload, "a", "A",
                                           true};
    const rfm::core::TransferRequest second{12, rfm::core::TransferDirection::Upload, "b", "B"};
    const rfm::core::TransferRequest third{13, rfm::core::TransferDirection::Upload, "c", "C",
                                           true};
    QVERIFY(queue.enqueue(first));
    QVERIFY(queue.enqueue(second));
    QVERIFY(queue.enqueue(third));
    QVERIFY(!queue.enqueue(second));
    QCOMPARE(queue.size(), qsizetype{3});
    const auto firstQueued = queue.takeNext();
    QVERIFY(firstQueued.has_value());
    QCOMPARE(firstQueued->id, quint64{11});
    QVERIFY(firstQueued->directory);
    QCOMPARE(queue.takeNext()->id, quint64{12});
    const auto thirdQueued = queue.takeNext();
    QVERIFY(thirdQueued.has_value());
    QCOMPARE(thirdQueued->id, quint64{13});
    QVERIFY(thirdQueued->directory);
    QVERIFY(queue.isEmpty());
}

void TransferFileJobTest::queueContinuesAfterSuccessAndFailure()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString firstSource = directory.filePath(QStringLiteral("first"));
    const QString secondSource = directory.filePath(QStringLiteral("second"));
    const QString thirdSource = directory.filePath(QStringLiteral("third"));
    writeLocalFile(firstSource, QByteArray(10, 'a'));
    writeLocalFile(secondSource, QByteArray(10, 'b'));
    writeLocalFile(thirdSource, QByteArray(10, 'c'));

    rfm::core::TransferQueue queue;
    QVERIFY(queue.enqueue(
        {31, rfm::core::TransferDirection::Upload, firstSource, QStringLiteral("/first")}));
    QVERIFY(queue.enqueue(
        {32, rfm::core::TransferDirection::Upload, secondSource, QStringLiteral("/second")}));
    QVERIFY(queue.enqueue(
        {33, rfm::core::TransferDirection::Upload, thirdSource, QStringLiteral("/third")}));

    FakeBackend backend;
    const auto first = queue.takeNext();
    QVERIFY(first.has_value());
    QCOMPARE(queue.size(), qsizetype{2});
    rfm::core::TransferFileJob firstJob(backend, *first);
    QVERIFY(runToEnd(firstJob) > 0);
    QCOMPARE(firstJob.progress().state, rfm::core::TransferState::Completed);

    const auto second = queue.takeNext();
    QVERIFY(second.has_value());
    backend.failWriteAt = backend.writeCalls + 1;
    rfm::core::TransferFileJob secondJob(backend, *second);
    QVERIFY(runToEnd(secondJob) > 0);
    QCOMPARE(secondJob.progress().state, rfm::core::TransferState::Failed);

    const auto third = queue.takeNext();
    QVERIFY(third.has_value());
    QCOMPARE(third->id, quint64{33});
    backend.failWriteAt = 0;
    rfm::core::TransferFileJob thirdJob(backend, *third);
    QVERIFY(runToEnd(thirdJob) > 0);
    QCOMPARE(thirdJob.progress().state, rfm::core::TransferState::Completed);
    QVERIFY(queue.isEmpty());
}

void TransferFileJobTest::queuedCancellationPreservesOrderAndPerformsNoIo()
{
    rfm::core::TransferQueue queue;
    QVERIFY(queue.enqueue({21, rfm::core::TransferDirection::Upload, "a", "A"}));
    QVERIFY(queue.enqueue({22, rfm::core::TransferDirection::Upload, "b", "B"}));
    QVERIFY(queue.enqueue({23, rfm::core::TransferDirection::Upload, "c", "C"}));
    const auto activeRequest = queue.takeNext();
    QVERIFY(activeRequest.has_value());
    QCOMPARE(activeRequest->id, quint64{21});

    FakeBackend backend;
    rfm::core::TransferRequest cancelled;
    QVERIFY(queue.cancel(22, cancelled));
    QCOMPARE(cancelled.id, quint64{22});
    QCOMPARE(backend.statCalls, 0);
    QCOMPARE(backend.openReadCalls, 0);
    QCOMPARE(backend.openWriteCalls, 0);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("active"));
    writeLocalFile(source, QByteArray(testSize, 'a'));
    rfm::core::TransferFileJob active(backend,
                                      {activeRequest->id, rfm::core::TransferDirection::Upload,
                                       source, QStringLiteral("/active")});
    while (active.progress().transferredBytes == 0) {
        active.step();
    }
    QVERIFY(active.requestCancel());
    QVERIFY(runToEnd(active) > 0);
    QCOMPARE(active.progress().state, rfm::core::TransferState::Cancelled);

    const auto next = queue.takeNext();
    QVERIFY(next.has_value());
    QCOMPARE(next->id, quint64{23});
}

void TransferFileJobTest::eventLoopRunsOneStepPerCallback()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("event-loop"));
    writeLocalFile(source, QByteArray(testSize, 'l'));
    FakeBackend backend;
    rfm::core::TransferFileJob job(
        backend, {41, rfm::core::TransferDirection::Upload, source, QStringLiteral("/loop")});
    StepDriver driver(job);
    QSignalSpy terminal(&driver, &StepDriver::terminal);
    int previousWrites = 0;
    int maximumWritesPerCallback = 0;
    connect(&driver, &StepDriver::updated, this, [&](const auto&) {
        maximumWritesPerCallback =
            std::max(maximumWritesPerCallback, backend.writeCalls - previousWrites);
        previousWrites = backend.writeCalls;
    });

    driver.start();
    QTRY_COMPARE(terminal.size(), 1);
    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QCOMPARE(backend.writeCalls, 4);
    QCOMPARE(maximumWritesPerCallback, 1);
    QVERIFY(driver.callbacks() > backend.writeCalls);
}

void TransferFileJobTest::eventLoopPauseAndResume()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("pause-loop"));
    writeLocalFile(source, QByteArray(testSize, 'r'));
    FakeBackend backend;
    rfm::core::TransferFileJob job(
        backend, {42, rfm::core::TransferDirection::Upload, source, QStringLiteral("/resume")});
    StepDriver driver(job);
    QSignalSpy terminal(&driver, &StepDriver::terminal);
    bool pauseRequested = false;
    connect(&driver, &StepDriver::updated, this, [&](const auto& progress) {
        if (!pauseRequested && progress.transferredBytes == 65'536) {
            pauseRequested = true;
            driver.pause();
        }
    });

    driver.start();
    QTRY_VERIFY(job.isPaused());
    const int pausedWrites = backend.writeCalls;
    const quint64 pausedBytes = job.progress().transferredBytes;
    for (int iteration = 0; iteration < 5; ++iteration) {
        QCoreApplication::processEvents();
    }
    QCOMPARE(backend.writeCalls, pausedWrites);
    QCOMPARE(job.progress().transferredBytes, pausedBytes);

    driver.resume();
    QTRY_COMPARE(terminal.size(), 1);
    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QCOMPARE(job.progress().transferredBytes, quint64{testSize});
    QCOMPARE(backend.files.value(QStringLiteral("/resume")), QByteArray(testSize, 'r'));
}

void TransferFileJobTest::uploadsDirectoryRecursively()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("root"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral("sub/deep"))));
    writeLocalFile(QDir(root).filePath(QStringLiteral("a.bin")), QByteArray("alpha"));
    writeLocalFile(QDir(root).filePath(QStringLiteral("b.bin")), QByteArray("beta"));
    writeLocalFile(QDir(root).filePath(QStringLiteral("sub/c.bin")), QByteArray("charlie"));
    writeLocalFile(QDir(root).filePath(QStringLiteral("sub/deep/d.bin")), QByteArray("delta"));

    FakeBackend backend;
    rfm::core::TransferDirectoryJob job(backend, {61, rfm::core::TransferDirection::Upload, root,
                                                  QStringLiteral("/uploaded"), true});
    QVERIFY(runDirectoryToEnd(job) > 1);

    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QCOMPARE(job.progress().completedFiles, quint64{4});
    QVERIFY(backend.directories.contains(QStringLiteral("/uploaded")));
    QVERIFY(backend.directories.contains(QStringLiteral("/uploaded/sub")));
    QVERIFY(backend.directories.contains(QStringLiteral("/uploaded/sub/deep")));
    QCOMPARE(backend.files.value(QStringLiteral("/uploaded/a.bin")), QByteArray("alpha"));
    QCOMPARE(backend.files.value(QStringLiteral("/uploaded/b.bin")), QByteArray("beta"));
    QCOMPARE(backend.files.value(QStringLiteral("/uploaded/sub/c.bin")), QByteArray("charlie"));
    QCOMPARE(backend.files.value(QStringLiteral("/uploaded/sub/deep/d.bin")), QByteArray("delta"));
}

void TransferFileJobTest::downloadsDirectoryRecursively()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeBackend backend;
    backend.directories.insert(QStringLiteral("/remote"));
    backend.directories.insert(QStringLiteral("/remote/sub"));
    backend.directories.insert(QStringLiteral("/remote/sub/deep"));
    backend.files.insert(QStringLiteral("/remote/a.bin"), QByteArray("alpha"));
    backend.files.insert(QStringLiteral("/remote/sub/c.bin"), QByteArray("charlie"));
    backend.files.insert(QStringLiteral("/remote/sub/deep/d.bin"), QByteArray("delta"));
    const QString destination = temporary.filePath(QStringLiteral("downloaded"));

    rfm::core::TransferDirectoryJob job(backend, {62, rfm::core::TransferDirection::Download,
                                                  QStringLiteral("/remote"), destination, true});
    QVERIFY(runDirectoryToEnd(job) > 1);

    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QCOMPARE(job.progress().completedFiles, quint64{3});
    QCOMPARE(readLocalFile(QDir(destination).filePath(QStringLiteral("a.bin"))),
             QByteArray("alpha"));
    QCOMPARE(readLocalFile(QDir(destination).filePath(QStringLiteral("sub/c.bin"))),
             QByteArray("charlie"));
    QCOMPARE(readLocalFile(QDir(destination).filePath(QStringLiteral("sub/deep/d.bin"))),
             QByteArray("delta"));
}

void TransferFileJobTest::transfersEmptyDirectories()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString localEmpty = temporary.filePath(QStringLiteral("local-empty"));
    QVERIFY(QDir().mkdir(localEmpty));

    FakeBackend uploadBackend;
    rfm::core::TransferDirectoryJob upload(uploadBackend,
                                           {63, rfm::core::TransferDirection::Upload, localEmpty,
                                            QStringLiteral("/empty-upload"), true});
    QVERIFY(runDirectoryToEnd(upload) > 0);
    QCOMPARE(upload.progress().state, rfm::core::TransferState::Completed);
    QVERIFY(uploadBackend.directories.contains(QStringLiteral("/empty-upload")));

    FakeBackend downloadBackend;
    downloadBackend.directories.insert(QStringLiteral("/empty-remote"));
    const QString localDownload = temporary.filePath(QStringLiteral("empty-download"));
    rfm::core::TransferDirectoryJob download(
        downloadBackend, {64, rfm::core::TransferDirection::Download,
                          QStringLiteral("/empty-remote"), localDownload, true});
    QVERIFY(runDirectoryToEnd(download) > 0);
    QCOMPARE(download.progress().state, rfm::core::TransferState::Completed);
    QVERIFY(QFileInfo(localDownload).isDir());
}

void TransferFileJobTest::rejectsLocalAndRemoteSymbolicLinks()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString localRoot = temporary.filePath(QStringLiteral("links"));
    QVERIFY(QDir().mkdir(localRoot));
    const QString target = QDir(localRoot).filePath(QStringLiteral("target.bin"));
    const QString link = QDir(localRoot).filePath(QStringLiteral("link.bin"));
    writeLocalFile(target, QByteArray("secret"));
    QVERIFY(QFile::link(target, link));

    FakeBackend uploadBackend;
    rfm::core::TransferDirectoryJob upload(
        uploadBackend,
        {65, rfm::core::TransferDirection::Upload, localRoot, QStringLiteral("/links"), true});
    QVERIFY(runDirectoryToEnd(upload) > 0);
    QCOMPARE(upload.progress().state, rfm::core::TransferState::Failed);
    QVERIFY(upload.progress().error.contains(QStringLiteral("Symbolic links")));
    QVERIFY(!uploadBackend.directories.contains(QStringLiteral("/links")));

    FakeBackend downloadBackend;
    downloadBackend.directories.insert(QStringLiteral("/remote-links"));
    downloadBackend.files.insert(QStringLiteral("/outside"), QByteArray("outside"));
    downloadBackend.symbolicLinks.insert(QStringLiteral("/remote-links/link"));
    const QString localDestination = temporary.filePath(QStringLiteral("remote-links"));
    rfm::core::TransferDirectoryJob download(
        downloadBackend, {66, rfm::core::TransferDirection::Download,
                          QStringLiteral("/remote-links"), localDestination, true});
    QVERIFY(runDirectoryToEnd(download) > 0);
    QCOMPARE(download.progress().state, rfm::core::TransferState::Failed);
    QVERIFY(download.progress().error.contains(QStringLiteral("Symbolic links")));
    QVERIFY(!QFileInfo::exists(localDestination));
}

void TransferFileJobTest::refusesDirectoryDestinationCollisions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkdir(source));

    FakeBackend remoteDirectoryCollision;
    remoteDirectoryCollision.directories.insert(QStringLiteral("/existing"));
    rfm::core::TransferDirectoryJob uploadDirectory(
        remoteDirectoryCollision,
        {67, rfm::core::TransferDirection::Upload, source, QStringLiteral("/existing"), true});
    QVERIFY(runDirectoryToEnd(uploadDirectory) > 0);
    QCOMPARE(uploadDirectory.progress().state, rfm::core::TransferState::Failed);

    FakeBackend remoteFileCollision;
    remoteFileCollision.files.insert(QStringLiteral("/existing"), QByteArray("file"));
    rfm::core::TransferDirectoryJob uploadFile(
        remoteFileCollision,
        {68, rfm::core::TransferDirection::Upload, source, QStringLiteral("/existing"), true});
    QVERIFY(runDirectoryToEnd(uploadFile) > 0);
    QCOMPARE(uploadFile.progress().state, rfm::core::TransferState::Failed);

    FakeBackend downloadBackend;
    downloadBackend.directories.insert(QStringLiteral("/remote"));
    const QString existingLocal = temporary.filePath(QStringLiteral("existing-local"));
    QVERIFY(QDir().mkdir(existingLocal));
    rfm::core::TransferDirectoryJob download(downloadBackend,
                                             {69, rfm::core::TransferDirection::Download,
                                              QStringLiteral("/remote"), existingLocal, true});
    QVERIFY(runDirectoryToEnd(download) > 0);
    QCOMPARE(download.progress().state, rfm::core::TransferState::Failed);
}

void TransferFileJobTest::pausesAndResumesDirectoryChild()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("pause-tree"));
    QVERIFY(QDir().mkdir(root));
    writeLocalFile(QDir(root).filePath(QStringLiteral("large.bin")), QByteArray(testSize, 'p'));
    FakeBackend backend;
    rfm::core::TransferDirectoryJob job(backend, {70, rfm::core::TransferDirection::Upload, root,
                                                  QStringLiteral("/pause-tree"), true});
    while (job.progress().transferredBytes < 65'536) {
        job.step();
    }
    QVERIFY(job.requestPause());
    const quint64 pausedBytes = job.progress().transferredBytes;
    const int pausedWrites = backend.writeCalls;
    for (int iteration = 0; iteration < 5; ++iteration) {
        job.step();
    }
    QCOMPARE(job.progress().state, rfm::core::TransferState::Paused);
    QCOMPARE(job.progress().transferredBytes, pausedBytes);
    QCOMPARE(backend.writeCalls, pausedWrites);

    QVERIFY(job.resume());
    job.step();
    QVERIFY(job.progress().transferredBytes > pausedBytes);
    QVERIFY(runDirectoryToEnd(job) > 0);
    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QCOMPARE(backend.files.value(QStringLiteral("/pause-tree/large.bin")),
             QByteArray(testSize, 'p'));
}

void TransferFileJobTest::cancelsDirectoryDuringTraversal()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("cancel-discovery"));
    QVERIFY(QDir().mkdir(root));
    writeLocalFile(QDir(root).filePath(QStringLiteral("a.bin")), QByteArray("a"));
    writeLocalFile(QDir(root).filePath(QStringLiteral("b.bin")), QByteArray("b"));
    FakeBackend backend;
    rfm::core::TransferDirectoryJob job(backend, {71, rfm::core::TransferDirection::Upload, root,
                                                  QStringLiteral("/cancel-discovery"), true});
    job.step();
    job.step();
    QVERIFY(job.requestCancel());
    QVERIFY(runDirectoryToEnd(job) > 0);
    QCOMPARE(job.progress().state, rfm::core::TransferState::Cancelled);
    QCOMPARE(backend.writeCalls, 0);
    QVERIFY(!backend.directories.contains(QStringLiteral("/cancel-discovery")));

    FakeBackend remoteBackend;
    remoteBackend.directories.insert(QStringLiteral("/remote-discovery"));
    remoteBackend.files.insert(QStringLiteral("/remote-discovery/a"), QByteArray("a"));
    const QString destination = temporary.filePath(QStringLiteral("cancel-download"));
    rfm::core::TransferDirectoryJob remoteJob(
        remoteBackend, {75, rfm::core::TransferDirection::Download,
                        QStringLiteral("/remote-discovery"), destination, true});
    remoteJob.step();
    remoteJob.step();
    QCOMPARE(remoteBackend.directoryHandles.size(), qsizetype{1});
    QVERIFY(remoteJob.requestCancel());
    QVERIFY(runDirectoryToEnd(remoteJob) > 0);
    QCOMPARE(remoteJob.progress().state, rfm::core::TransferState::Cancelled);
    QVERIFY(remoteBackend.directoryHandles.isEmpty());
    QVERIFY(!QFileInfo::exists(destination));
}

void TransferFileJobTest::cancelsDirectoryChildAndWhilePaused()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("cancel-child"));
    QVERIFY(QDir().mkdir(root));
    writeLocalFile(QDir(root).filePath(QStringLiteral("a.bin")), QByteArray("done"));
    writeLocalFile(QDir(root).filePath(QStringLiteral("b.bin")), QByteArray(testSize, 'b'));
    FakeBackend backend;
    rfm::core::TransferDirectoryJob job(backend, {72, rfm::core::TransferDirection::Upload, root,
                                                  QStringLiteral("/cancel-child"), true});
    while (job.progress().completedFiles < 1 || job.progress().transferredBytes <= 4) {
        job.step();
    }
    QVERIFY(job.requestPause());
    QVERIFY(job.requestCancel());
    const int writes = backend.writeCalls;
    QVERIFY(runDirectoryToEnd(job) > 0);
    QCOMPARE(job.progress().state, rfm::core::TransferState::Cancelled);
    QCOMPARE(backend.writeCalls, writes);
    QCOMPARE(backend.files.value(QStringLiteral("/cancel-child/a.bin")), QByteArray("done"));
    QVERIFY(!backend.files.contains(QStringLiteral("/cancel-child/b.bin")));
    QVERIFY(!backend.files.contains(QStringLiteral("/cancel-child/b.bin.rfm-part-72")));
}

void TransferFileJobTest::reportsPartialDirectoryFailure()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("partial"));
    QVERIFY(QDir().mkdir(root));
    writeLocalFile(QDir(root).filePath(QStringLiteral("a.bin")), QByteArray("done"));
    writeLocalFile(QDir(root).filePath(QStringLiteral("b.bin")), QByteArray(testSize, 'b'));
    FakeBackend backend;
    backend.failWriteAt = 3;
    rfm::core::TransferDirectoryJob job(backend, {73, rfm::core::TransferDirection::Upload, root,
                                                  QStringLiteral("/partial"), true});
    quint64 previous = 0;
    while (!job.isFinished()) {
        QVERIFY(job.progress().transferredBytes >= previous);
        previous = job.progress().transferredBytes;
        job.step();
    }
    QCOMPARE(job.progress().state, rfm::core::TransferState::Failed);
    QCOMPARE(job.progress().completedFiles, quint64{1});
    QVERIFY(job.progress().error.contains(QStringLiteral("b.bin")));
    QCOMPARE(backend.files.value(QStringLiteral("/partial/a.bin")), QByteArray("done"));
    QVERIFY(!backend.files.contains(QStringLiteral("/partial/b.bin")));
    QVERIFY(!backend.files.contains(QStringLiteral("/partial/b.bin.rfm-part-73")));
}

void TransferFileJobTest::readsOneRemoteDirectoryEntryPerStep()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeBackend backend;
    backend.directories.insert(QStringLiteral("/incremental"));
    backend.files.insert(QStringLiteral("/incremental/a"), QByteArray("a"));
    backend.files.insert(QStringLiteral("/incremental/b"), QByteArray("b"));
    backend.files.insert(QStringLiteral("/incremental/c"), QByteArray("c"));
    rfm::core::TransferDirectoryJob job(
        backend, {74, rfm::core::TransferDirection::Download, QStringLiteral("/incremental"),
                  temporary.filePath(QStringLiteral("incremental")), true});
    int maximumReadsPerStep = 0;
    int previousReads = 0;
    int steps = 0;
    while (!job.isFinished()) {
        job.step();
        maximumReadsPerStep =
            std::max(maximumReadsPerStep, backend.readDirectoryCalls - previousReads);
        previousReads = backend.readDirectoryCalls;
        ++steps;
        QVERIFY(steps < 1000);
    }
    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QCOMPARE(maximumReadsPerStep, 1);
    QCOMPARE(backend.readDirectoryCalls, 4);
    QVERIFY(steps > backend.readDirectoryCalls);
}

void TransferFileJobTest::eventLoopRunsDirectoryIncrementally()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    FakeBackend backend;
    backend.directories.insert(QStringLiteral("/event-tree"));
    backend.files.insert(QStringLiteral("/event-tree/large.bin"), QByteArray(testSize, 'e'));
    rfm::core::TransferDirectoryJob job(
        backend, {76, rfm::core::TransferDirection::Download, QStringLiteral("/event-tree"),
                  temporary.filePath(QStringLiteral("event-tree")), true});
    StepDriver driver(job);
    QSignalSpy terminal(&driver, &StepDriver::terminal);
    int previousDirectoryReads = 0;
    int previousFileReads = 0;
    int maximumDirectoryReads = 0;
    int maximumFileReads = 0;
    connect(&driver, &StepDriver::updated, this, [&](const auto&) {
        maximumDirectoryReads =
            std::max(maximumDirectoryReads, backend.readDirectoryCalls - previousDirectoryReads);
        maximumFileReads = std::max(maximumFileReads, backend.readCalls - previousFileReads);
        previousDirectoryReads = backend.readDirectoryCalls;
        previousFileReads = backend.readCalls;
    });

    driver.start();
    QTRY_COMPARE(terminal.size(), 1);
    QCOMPARE(job.progress().state, rfm::core::TransferState::Completed);
    QCOMPARE(maximumDirectoryReads, 1);
    QCOMPARE(maximumFileReads, 1);
    QVERIFY(driver.callbacks() > backend.readCalls + backend.readDirectoryCalls);
    QCOMPARE(readLocalFile(temporary.filePath(QStringLiteral("event-tree/large.bin"))),
             QByteArray(testSize, 'e'));
}

QTEST_GUILESS_MAIN(TransferFileJobTest)

#include "test_transfer_file_job.moc"
