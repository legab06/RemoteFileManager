#include "remotefilemanager/core/RemoteTransferBackend.hpp"
#include "remotefilemanager/core/TransferCoordinator.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

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

  private:
    [[nodiscard]] std::unique_ptr<SshSession> makeSession();
    [[nodiscard]] std::unique_ptr<rfm::core::TransferCoordinator>
    makeCoordinator(SshSession& session);
    [[nodiscard]] QString makeEmptySource(QTemporaryDir& directory, const QString& name);
    void startFirstTransfer(SshSession& session);

    QList<BackendMode> m_modes;
    int m_createdBackends{0};
};

void SshSessionTransferTest::init()
{
    qRegisterMetaType<rfm::core::TransferProgress>();
    m_modes.clear();
    m_createdBackends = 0;
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
        [] { return true; }, nullptr));
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
    QVERIFY(hasState(updates, 32, rfm::core::TransferState::Transferring));
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

} // namespace rfm::ssh

QTEST_MAIN(rfm::ssh::SshSessionTransferTest)
#include "test_ssh_transfer_lifecycle.moc"
