#include "remotefilemanager/core/RemoteTransferBackend.hpp"
#include "remotefilemanager/core/TransferCoordinator.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include "../src/ssh/RemoteRemoveJob.hpp"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>

namespace rfm::ssh
{
namespace
{

enum class BackendMode { Success, FileIoFailure, TransportIoFailure };

class LifecycleBackend final : public rfm::core::RemoteTransferBackend
{
  public:
    explicit LifecycleBackend(BackendMode mode) : m_mode(mode) {}

    [[nodiscard]] bool connectionAlive() const override { return m_connectionAlive; }

    rfm::core::TransferStatResult stat(const QString& path) override
    {
        if (path.startsWith(QStringLiteral("/source-"))) {
            return {{}, {true, rfm::core::TransferNodeType::RegularFile, 4}};
        }
        return {{rfm::core::TransferBackendError::NotFound, {}}, {}};
    }

    rfm::core::TransferBackendResult openRead(const QString& /*path*/, quint64& handle) override
    {
        handle = 1;
        return {};
    }

    rfm::core::TransferBackendResult openWriteExclusive(const QString& /*path*/,
                                                        quint64& handle) override
    {
        handle = 1;
        return {};
    }

    rfm::core::TransferBackendResult read(quint64 /*handle*/, QByteArray& data,
                                          qsizetype /*maximum*/) override
    {
        if (m_mode == BackendMode::FileIoFailure) {
            return {rfm::core::TransferBackendError::Io, {}};
        }
        if (m_mode == BackendMode::TransportIoFailure) {
            m_connectionAlive = false;
            return {rfm::core::TransferBackendError::Io, {}};
        }
        if (!m_readCompleted) {
            data = QByteArrayLiteral("data");
            m_readCompleted = true;
            return {};
        }
        data.clear();
        return {};
    }

    rfm::core::TransferBackendResult write(quint64 /*handle*/, const QByteArray& /*data*/) override
    {
        if (m_mode == BackendMode::FileIoFailure) {
            return {rfm::core::TransferBackendError::Io, {}};
        }
        if (m_mode == BackendMode::TransportIoFailure) {
            m_connectionAlive = false;
            return {rfm::core::TransferBackendError::Io, {}};
        }
        return {};
    }

    rfm::core::TransferBackendResult close(quint64 /*handle*/) override { return {}; }

    rfm::core::TransferBackendResult createDirectory(const QString& /*path*/) override
    {
        return {};
    }

    rfm::core::TransferBackendResult rename(const QString& /*source*/,
                                            const QString& /*destination*/) override
    {
        return {};
    }

    rfm::core::TransferBackendResult remove(const QString& /*path*/) override { return {}; }

    rfm::core::TransferBackendResult openDirectory(const QString& /*path*/,
                                                   quint64& handle) override
    {
        handle = 1;
        return {};
    }

    rfm::core::TransferBackendResult
    readDirectory(quint64 /*handle*/,
                  std::optional<rfm::core::TransferDirectoryEntry>& entry) override
    {
        entry.reset();
        return {};
    }

    rfm::core::TransferBackendResult closeDirectory(quint64 /*handle*/) override { return {}; }

  private:
    BackendMode m_mode;
    bool m_connectionAlive{true};
    bool m_readCompleted{false};
};

struct SessionRemoveStats {
    int closedDirectories{0};
};

class SessionRemoveDirectory final : public RemoteRemoveDirectory
{
  public:
    SessionRemoveDirectory(int entries, std::shared_ptr<SessionRemoveStats> stats)
        : m_remaining(entries), m_stats(std::move(stats))
    {}

    ~SessionRemoveDirectory() override { ++m_stats->closedDirectories; }

    RemoteRemoveRead read() override
    {
        if (m_remaining-- > 0) {
            return {RemoteRemoveReadState::Entry, QStringLiteral("entry"), false, {}};
        }
        return {RemoteRemoveReadState::End, {}, false, {}};
    }

  private:
    int m_remaining{0};
    std::shared_ptr<SessionRemoveStats> m_stats;
};

class SessionRemoveBackend final : public RemoteRemoveBackend
{
  public:
    explicit SessionRemoveBackend(
        std::shared_ptr<SessionRemoveStats> stats = std::make_shared<SessionRemoveStats>())
        : m_stats(std::move(stats))
    {}

    rfm::core::RemoteBackendResult preflightDirectory(const QString&) override { return {}; }

    RemoteRemoveOpenResult openDirectory(const QString&) override
    {
        return {{},
                std::make_unique<SessionRemoveDirectory>(RemoteRemoveJob::operationsPerStep + 2,
                                                         m_stats)};
    }

    rfm::core::RemoteBackendResult removeFile(const QString&) override
    {
        ++removedFiles;
        return {};
    }

    rfm::core::RemoteBackendResult removeDirectory(const QString&) override
    {
        ++removedDirectories;
        return {};
    }

    int removedFiles{0};
    int removedDirectories{0};

  private:
    std::shared_ptr<SessionRemoveStats> m_stats;
};

bool isTerminal(rfm::core::TransferState state)
{
    return state == rfm::core::TransferState::Completed ||
           state == rfm::core::TransferState::Cancelled ||
           state == rfm::core::TransferState::Failed;
}

QList<rfm::core::TransferProgress> progressEvents(const QSignalSpy& updates)
{
    QList<rfm::core::TransferProgress> events;
    events.reserve(updates.size());
    for (const QList<QVariant>& arguments : updates) {
        events.push_back(qvariant_cast<rfm::core::TransferProgress>(arguments.constFirst()));
    }
    return events;
}

int terminalCount(const QSignalSpy& updates, quint64 id)
{
    int count = 0;
    for (const rfm::core::TransferProgress& progress : progressEvents(updates)) {
        if (progress.id == id && isTerminal(progress.state)) {
            ++count;
        }
    }
    return count;
}

rfm::core::TransferState terminalState(const QSignalSpy& updates, quint64 id)
{
    for (const rfm::core::TransferProgress& progress : progressEvents(updates)) {
        if (progress.id == id && isTerminal(progress.state)) {
            return progress.state;
        }
    }
    return rfm::core::TransferState::Queued;
}

bool hasState(const QSignalSpy& updates, quint64 id, rfm::core::TransferState state)
{
    for (const rfm::core::TransferProgress& progress : progressEvents(updates)) {
        if (progress.id == id && progress.state == state) {
            return true;
        }
    }
    return false;
}

QList<rfm::core::TransferState> statesFor(const QSignalSpy& updates, quint64 id)
{
    QList<rfm::core::TransferState> states;
    for (const rfm::core::TransferProgress& progress : progressEvents(updates)) {
        if (progress.id == id) {
            states.push_back(progress.state);
        }
    }
    return states;
}

int eventIndex(const QSignalSpy& updates, quint64 id, rfm::core::TransferState state)
{
    const QList<rfm::core::TransferProgress> events = progressEvents(updates);
    for (qsizetype index = 0; index < events.size(); ++index) {
        if (events.at(index).id == id && events.at(index).state == state) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

rfm::core::TransferRequest upload(quint64 id, const QString& source)
{
    return {id, rfm::core::TransferDirection::Upload, source,
            QStringLiteral("/destination-%1").arg(id)};
}

rfm::core::TransferRequest download(quint64 id, QTemporaryDir& directory)
{
    return {id, rfm::core::TransferDirection::Download, QStringLiteral("/source-%1").arg(id),
            directory.filePath(QStringLiteral("download-%1").arg(id))};
}

rfm::core::TransferProgress progress(quint64 id, rfm::core::TransferState state,
                                     quint64 transferredBytes, quint64 totalBytes)
{
    return {id,
            state,
            QStringLiteral("/source"),
            QStringLiteral("/destination"),
            transferredBytes,
            totalBytes,
            0,
            {},
            0,
            0,
            {},
            rfm::core::TransferDirection::Upload,
            false};
}

} // namespace

class SshSessionTransferTest final : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void shutdownCancelsActiveAndQueuedExactlyOnce();
    void forcedDisconnectCancelsActiveAndQueuedExactlyOnce();
    void connectionLossFailsActiveAndQueuedExactlyOnce();
    void normalFailureStartsNextTransfer();
    void cancellingQueuedTransferDoesNotCreateBackend();
    void pendingRemoteDeleteDisconnectFinishesExactlyOnce();
    void remoteRemoveRunsAcrossQueuedWorkerSteps();
    void disconnectCancelsActiveRemoteRemoveAndClosesDirectories();
    void throttlesProgressAndPublishesLatestValue();
    void publishesControlAndTerminalStatesImmediately();
    void controlRequestsBypassThrottle();
    void keepsThrottleStateIndependentAndClearsOnDisconnect();

  private:
    [[nodiscard]] std::unique_ptr<SshSession> makeSession();
    [[nodiscard]] std::unique_ptr<rfm::core::TransferCoordinator>
    makeCoordinator(SshSession& session);
    [[nodiscard]] QString makeEmptySource(QTemporaryDir& directory, const QString& name);
    void startFirstTransfer(SshSession& session);

    QList<BackendMode> m_modes;
    int m_createdBackends{0};
    qint64 m_clock{0};
};

void SshSessionTransferTest::init()
{
    qRegisterMetaType<rfm::core::TransferProgress>();
    m_modes.clear();
    m_createdBackends = 0;
    m_clock = 0;
}

std::unique_ptr<SshSession> SshSessionTransferTest::makeSession()
{
    return std::unique_ptr<SshSession>(new SshSession(
        [this] {
            const BackendMode mode = m_createdBackends < m_modes.size()
                                         ? m_modes.at(m_createdBackends)
                                         : BackendMode::Success;
            ++m_createdBackends;
            return std::make_unique<LifecycleBackend>(mode);
        },
        [] { return true; }, nullptr, [this] { return m_clock; }));
}

std::unique_ptr<rfm::core::TransferCoordinator>
SshSessionTransferTest::makeCoordinator(SshSession& session)
{
    auto coordinator = std::make_unique<rfm::core::TransferCoordinator>();
    connect(coordinator.get(), &rfm::core::TransferCoordinator::startTransferRequested, &session,
            &SshSession::startTransfer);
    connect(coordinator.get(), &rfm::core::TransferCoordinator::pauseActiveRequested, &session,
            &SshSession::pauseTransfer);
    connect(coordinator.get(), &rfm::core::TransferCoordinator::resumeActiveRequested, &session,
            &SshSession::resumeTransfer);
    connect(coordinator.get(), &rfm::core::TransferCoordinator::cancelActiveRequested, &session,
            &SshSession::cancelTransfer);
    connect(coordinator.get(), &rfm::core::TransferCoordinator::shutdownExecutorRequested, &session,
            &SshSession::shutdownTransfers);
    connect(coordinator.get(), &rfm::core::TransferCoordinator::disconnectExecutorRequested,
            &session, &SshSession::disconnectFromHost);
    connect(&session, &SshSession::transferUpdated, coordinator.get(),
            &rfm::core::TransferCoordinator::handleExecutorProgress);
    connect(&session, &SshSession::transferExecutorFailed, coordinator.get(),
            &rfm::core::TransferCoordinator::handleExecutorFailure);
    connect(&session, &SshSession::transferRejected, coordinator.get(),
            &rfm::core::TransferCoordinator::handleExecutorRejection);
    connect(&session, &SshSession::transfersShutdown, coordinator.get(),
            &rfm::core::TransferCoordinator::handleExecutorShutdown);
    coordinator->executorConnected();
    return coordinator;
}

QString SshSessionTransferTest::makeEmptySource(QTemporaryDir& directory, const QString& name)
{
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.close();
    return path;
}

void SshSessionTransferTest::startFirstTransfer(SshSession& session)
{
    session.processTransferStep();
    session.processTransferStep();
}

void SshSessionTransferTest::shutdownCancelsActiveAndQueuedExactlyOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = makeEmptySource(directory, QStringLiteral("source"));
    QVERIFY(!source.isEmpty());
    auto session = makeSession();
    auto coordinator = makeCoordinator(*session);
    QSignalSpy updates(coordinator.get(), &rfm::core::TransferCoordinator::transferUpdated);
    QSignalSpy shutdown(coordinator.get(), &rfm::core::TransferCoordinator::transfersShutdown);

    coordinator->enqueueTransfer(upload(1, source));
    coordinator->enqueueTransfer(upload(2, source));
    coordinator->enqueueTransfer(upload(3, source));
    startFirstTransfer(*session);
    QCOMPARE(m_createdBackends, 1);

    coordinator->shutdownTransfers();
    QTRY_COMPARE(shutdown.size(), 1);
    QCOMPARE(terminalCount(updates, 1), 1);
    QCOMPARE(terminalCount(updates, 2), 1);
    QCOMPARE(terminalCount(updates, 3), 1);
    QCOMPARE(terminalState(updates, 1), rfm::core::TransferState::Cancelled);
    QCOMPARE(terminalState(updates, 2), rfm::core::TransferState::Cancelled);
    QCOMPARE(terminalState(updates, 3), rfm::core::TransferState::Cancelled);
    QCOMPARE(m_createdBackends, 1);
}

void SshSessionTransferTest::forcedDisconnectCancelsActiveAndQueuedExactlyOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = makeEmptySource(directory, QStringLiteral("source"));
    QVERIFY(!source.isEmpty());
    auto session = makeSession();
    auto coordinator = makeCoordinator(*session);
    QSignalSpy updates(coordinator.get(), &rfm::core::TransferCoordinator::transferUpdated);
    QSignalSpy disconnected(session.get(), &SshSession::disconnected);

    coordinator->enqueueTransfer(upload(11, source));
    coordinator->enqueueTransfer(upload(12, source));
    startFirstTransfer(*session);
    coordinator->disconnectExecutor();

    QTRY_COMPARE(disconnected.size(), 1);
    QCOMPARE(terminalCount(updates, 11), 1);
    QCOMPARE(terminalCount(updates, 12), 1);
    QCOMPARE(terminalState(updates, 11), rfm::core::TransferState::Cancelled);
    QCOMPARE(terminalState(updates, 12), rfm::core::TransferState::Cancelled);
    QCOMPARE(m_createdBackends, 1);
}

void SshSessionTransferTest::connectionLossFailsActiveAndQueuedExactlyOnce()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    m_modes = {BackendMode::TransportIoFailure};
    auto session = makeSession();
    auto coordinator = makeCoordinator(*session);
    QSignalSpy updates(coordinator.get(), &rfm::core::TransferCoordinator::transferUpdated);
    QSignalSpy failures(session.get(), &SshSession::failed);
    QSignalSpy executorFailures(session.get(), &SshSession::transferExecutorFailed);

    coordinator->enqueueTransfer(download(21, directory));
    coordinator->enqueueTransfer(download(22, directory));
    coordinator->enqueueTransfer(download(23, directory));

    QTRY_COMPARE(failures.size(), 1);
    QCOMPARE(executorFailures.size(), 1);
    QCOMPARE(terminalCount(updates, 21), 1);
    QCOMPARE(terminalCount(updates, 22), 1);
    QCOMPARE(terminalCount(updates, 23), 1);
    QCOMPARE(terminalState(updates, 21), rfm::core::TransferState::Failed);
    QCOMPARE(terminalState(updates, 22), rfm::core::TransferState::Failed);
    QCOMPARE(terminalState(updates, 23), rfm::core::TransferState::Failed);
    QCOMPARE(statesFor(updates, 22),
             (QList<rfm::core::TransferState>{rfm::core::TransferState::Queued,
                                              rfm::core::TransferState::Failed}));
    QCOMPARE(statesFor(updates, 23),
             (QList<rfm::core::TransferState>{rfm::core::TransferState::Queued,
                                              rfm::core::TransferState::Failed}));
    QVERIFY(eventIndex(updates, 21, rfm::core::TransferState::Failed) <
            eventIndex(updates, 22, rfm::core::TransferState::Failed));
    QVERIFY(!hasState(updates, 22, rfm::core::TransferState::Preparing));
    QVERIFY(!hasState(updates, 22, rfm::core::TransferState::Transferring));
    QVERIFY(!hasState(updates, 22, rfm::core::TransferState::Finalizing));
    QVERIFY(!hasState(updates, 23, rfm::core::TransferState::Preparing));
    QVERIFY(!hasState(updates, 23, rfm::core::TransferState::Transferring));
    QVERIFY(!hasState(updates, 23, rfm::core::TransferState::Finalizing));
    QCOMPARE(m_createdBackends, 1);
}

void SshSessionTransferTest::normalFailureStartsNextTransfer()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    m_modes = {BackendMode::FileIoFailure, BackendMode::Success};
    auto session = makeSession();
    auto coordinator = makeCoordinator(*session);
    QSignalSpy updates(coordinator.get(), &rfm::core::TransferCoordinator::transferUpdated);
    QSignalSpy failures(session.get(), &SshSession::failed);
    QSignalSpy executorFailures(session.get(), &SshSession::transferExecutorFailed);

    coordinator->enqueueTransfer(download(31, directory));
    coordinator->enqueueTransfer(download(32, directory));

    QTRY_COMPARE(terminalCount(updates, 32), 1);
    QCOMPARE(terminalCount(updates, 31), 1);
    QCOMPARE(terminalState(updates, 31), rfm::core::TransferState::Failed);
    QCOMPARE(terminalState(updates, 32), rfm::core::TransferState::Completed);
    QVERIFY(hasState(updates, 32, rfm::core::TransferState::Preparing));
    QVERIFY(eventIndex(updates, 31, rfm::core::TransferState::Failed) <
            eventIndex(updates, 32, rfm::core::TransferState::Preparing));
    QCOMPARE(failures.size(), 0);
    QCOMPARE(executorFailures.size(), 0);
    QCOMPARE(m_createdBackends, 2);
}

void SshSessionTransferTest::cancellingQueuedTransferDoesNotCreateBackend()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = makeEmptySource(directory, QStringLiteral("source"));
    QVERIFY(!source.isEmpty());
    auto session = makeSession();
    auto coordinator = makeCoordinator(*session);
    QSignalSpy updates(coordinator.get(), &rfm::core::TransferCoordinator::transferUpdated);
    QSignalSpy shutdown(coordinator.get(), &rfm::core::TransferCoordinator::transfersShutdown);

    coordinator->enqueueTransfer(upload(41, source));
    coordinator->enqueueTransfer(upload(42, source));
    startFirstTransfer(*session);
    coordinator->cancelTransfer(42);

    QCOMPARE(terminalCount(updates, 42), 1);
    QCOMPARE(terminalState(updates, 42), rfm::core::TransferState::Cancelled);
    QCOMPARE(m_createdBackends, 1);

    coordinator->shutdownTransfers();
    QTRY_COMPARE(shutdown.size(), 1);
    QCOMPARE(terminalCount(updates, 41), 1);
    QCOMPARE(terminalCount(updates, 42), 1);
    QCOMPARE(m_createdBackends, 1);
}

void SshSessionTransferTest::pendingRemoteDeleteDisconnectFinishesExactlyOnce()
{
    auto session = makeSession();
    QSignalSpy finished(session.get(), &SshSession::operationFinished);
    QSignalSpy disconnected(session.get(), &SshSession::disconnected);
    session->stagePendingRemoteDeleteForTesting(
        51, {{QStringLiteral("/C:/Users/Administrateur/rfm-destination"), true}});

    session->disconnectFromHost();

    QCOMPARE(disconnected.size(), 1);
    QCOMPARE(finished.size(), 1);
    const auto result =
        finished.constFirst().constFirst().value<rfm::core::RemoteOperationResult>();
    QCOMPARE(result.id, quint64{51});
    QCOMPARE(result.items.size(), 1);
    QVERIFY(!result.items.constFirst().success);
    QVERIFY(result.items.constFirst().error.contains(QStringLiteral("closed")));

    session->processRemoteDeleteSafetyProbe();
    QCOMPARE(finished.size(), 1);
}

void SshSessionTransferTest::remoteRemoveRunsAcrossQueuedWorkerSteps()
{
    auto session = makeSession();
    auto backend = std::make_unique<SessionRemoveBackend>();
    QSignalSpy finished(session.get(), &SshSession::operationFinished);
    bool timerRan = false;

    session->stageRemoteRemoveForTesting(std::move(backend), 51, {{QStringLiteral("/tree"), true}},
                                         true);
    QTimer::singleShot(0, [&timerRan] { timerRan = true; });
    QTRY_VERIFY(timerRan);
    QCOMPARE(finished.size(), 0);
    QTRY_COMPARE(finished.size(), 1);
    const auto result =
        finished.constFirst().constFirst().value<rfm::core::RemoteOperationResult>();
    QCOMPARE(result.id, quint64{51});
    QVERIFY(result.allSucceeded());
}

void SshSessionTransferTest::disconnectCancelsActiveRemoteRemoveAndClosesDirectories()
{
    auto session = makeSession();
    const auto stats = std::make_shared<SessionRemoveStats>();
    QSignalSpy finished(session.get(), &SshSession::operationFinished);
    session->stageRemoteRemoveForTesting(std::make_unique<SessionRemoveBackend>(stats), 52,
                                         {{QStringLiteral("/tree"), true}}, true);

    session->processRemoteRemoveStep();
    session->disconnectFromHost();

    QCOMPARE(finished.size(), 1);
    const auto result =
        finished.constFirst().constFirst().value<rfm::core::RemoteOperationResult>();
    QCOMPARE(result.id, quint64{52});
    QVERIFY(!result.allSucceeded());
    QCOMPARE(stats->closedDirectories, 1);
}

void SshSessionTransferTest::throttlesProgressAndPublishesLatestValue()
{
    auto session = makeSession();
    QSignalSpy updates(session.get(), &SshSession::transferUpdated);

    session->publishTransferProgress(progress(61, rfm::core::TransferState::Preparing, 0, 1000));
    for (quint64 transferred = 1; transferred <= 1000; ++transferred) {
        ++m_clock;
        session->publishTransferProgress(
            progress(61, rfm::core::TransferState::Transferring, transferred, 1000));
    }

    const qsizetype intermediatePublications = updates.size();
    QVERIFY(intermediatePublications > 2);
    QVERIFY(intermediatePublications < 30);

    session->publishTransferProgress(progress(61, rfm::core::TransferState::Completed, 1000, 1000));
    const QList<rfm::core::TransferProgress> events = progressEvents(updates);
    QCOMPARE(events.constLast().state, rfm::core::TransferState::Completed);
    QCOMPARE(events.constLast().transferredBytes, quint64{1000});
    QCOMPARE(events.constLast().totalBytes, quint64{1000});
    const qsizetype eventCount = updates.size();
    QCoreApplication::processEvents();
    QCOMPARE(updates.size(), eventCount);
}

void SshSessionTransferTest::publishesControlAndTerminalStatesImmediately()
{
    auto session = makeSession();
    QSignalSpy updates(session.get(), &SshSession::transferUpdated);

    session->publishTransferProgress(progress(62, rfm::core::TransferState::Preparing, 0, 100));
    ++m_clock;
    session->publishTransferProgress(progress(62, rfm::core::TransferState::Transferring, 1, 100));
    QCOMPARE(updates.size(), 1);

    session->publishTransferProgress(progress(62, rfm::core::TransferState::Paused, 1, 100), true);
    session->publishTransferProgress(progress(62, rfm::core::TransferState::Transferring, 2, 100),
                                     true);
    session->publishTransferProgress(progress(62, rfm::core::TransferState::Cancelling, 2, 100),
                                     true);
    session->publishTransferProgress(progress(62, rfm::core::TransferState::Cancelled, 2, 100));

    const QList<rfm::core::TransferProgress> events = progressEvents(updates);
    QCOMPARE(events.size(), 5);
    QCOMPARE(events.at(1).state, rfm::core::TransferState::Paused);
    QCOMPARE(events.at(2).state, rfm::core::TransferState::Transferring);
    QCOMPARE(events.at(3).state, rfm::core::TransferState::Cancelling);
    QCOMPARE(events.at(4).state, rfm::core::TransferState::Cancelled);
    session->publishTransferProgress(progress(62, rfm::core::TransferState::Transferring, 3, 100));
    QCOMPARE(updates.size(), 5);
}

void SshSessionTransferTest::controlRequestsBypassThrottle()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString source = makeEmptySource(directory, QStringLiteral("control-source"));
    QVERIFY(!source.isEmpty());
    auto session = makeSession();
    QSignalSpy updates(session.get(), &SshSession::transferUpdated);

    session->startTransfer(upload(65, source));
    session->processTransferStep();
    session->processTransferStep();
    session->processTransferStep();
    updates.clear();

    session->pauseTransfer(65);
    session->resumeTransfer(65);
    session->cancelTransfer(65);

    const QList<rfm::core::TransferProgress> events = progressEvents(updates);
    QCOMPARE(events.size(), 3);
    QCOMPARE(events.at(0).state, rfm::core::TransferState::Paused);
    QCOMPARE(events.at(1).state, rfm::core::TransferState::Transferring);
    QCOMPARE(events.at(2).state, rfm::core::TransferState::Cancelling);
}

void SshSessionTransferTest::keepsThrottleStateIndependentAndClearsOnDisconnect()
{
    auto session = makeSession();
    QSignalSpy updates(session.get(), &SshSession::transferUpdated);
    session->publishTransferProgress(progress(63, rfm::core::TransferState::Preparing, 0, 10));
    session->publishTransferProgress(progress(64, rfm::core::TransferState::Preparing, 0, 10));
    QCOMPARE(updates.size(), 2);

    ++m_clock;
    session->publishTransferProgress(progress(63, rfm::core::TransferState::Transferring, 1, 10));
    m_clock += 49;
    session->publishTransferProgress(progress(64, rfm::core::TransferState::Transferring, 1, 10));
    QCOMPARE(updates.size(), 3);
    QCOMPARE(progressEvents(updates).constLast().id, quint64{64});

    session->disconnectFromHost();
    session->publishTransferProgress(progress(63, rfm::core::TransferState::Transferring, 2, 10));
    QCOMPARE(updates.size(), 4);
    QCOMPARE(progressEvents(updates).constLast().id, quint64{63});
    QCOMPARE(progressEvents(updates).constLast().transferredBytes, quint64{2});
}

} // namespace rfm::ssh

QTEST_MAIN(rfm::ssh::SshSessionTransferTest)
#include "test_ssh_transfer_lifecycle.moc"
