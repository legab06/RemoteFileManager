#include "remotefilemanager/core/TransferCoordinator.hpp"

#include <QSignalSpy>
#include <QTest>

namespace
{

rfm::core::TransferRequest request(quint64 id)
{
    return {id, rfm::core::TransferDirection::Upload, QStringLiteral("source-%1").arg(id),
            QStringLiteral("destination-%1").arg(id)};
}

rfm::core::TransferProgress progress(quint64 id, rfm::core::TransferState state)
{
    const rfm::core::TransferRequest value = request(id);
    return {id, state, value.source,    value.destination, 0, 0, 0, {}, 0,
            0,  {},    value.direction, value.directory};
}

QList<rfm::core::TransferProgress> events(const QSignalSpy& spy)
{
    QList<rfm::core::TransferProgress> values;
    values.reserve(spy.size());
    for (const QList<QVariant>& arguments : spy) {
        values.push_back(qvariant_cast<rfm::core::TransferProgress>(arguments.constFirst()));
    }
    return values;
}

QList<rfm::core::TransferState> statesFor(const QSignalSpy& spy, quint64 id)
{
    QList<rfm::core::TransferState> states;
    for (const rfm::core::TransferProgress& value : events(spy)) {
        if (value.id == id) {
            states.push_back(value.state);
        }
    }
    return states;
}

quint64 dispatchedId(const QSignalSpy& spy, qsizetype index)
{
    return qvariant_cast<rfm::core::TransferRequest>(spy.at(index).constFirst()).id;
}

} // namespace

class TransferCoordinatorTest final : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void dispatchesInFifoOrder();
    void rejectsDuplicateIdsAcrossActiveAndQueue();
    void nonTerminalUpdatesNeverDoubleDispatch();
    void cancelsQueuedWithoutDispatch();
    void activeCancellationWaitsForTerminal();
    void normalFailureDispatchesNext();
    void executorFailureFailsQueueWithoutDispatch();
    void shutdownCancelsQueueAndStopsDispatch();
    void pauseAndResumeRouteOnlyToActive();
};

void TransferCoordinatorTest::initTestCase()
{
    qRegisterMetaType<rfm::core::TransferRequest>();
    qRegisterMetaType<rfm::core::TransferProgress>();
}

void TransferCoordinatorTest::dispatchesInFifoOrder()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator, &rfm::core::TransferCoordinator::startTransferRequested);

    coordinator.enqueueTransfer(request(1));
    coordinator.enqueueTransfer(request(2));
    coordinator.enqueueTransfer(request(3));
    QCOMPARE(dispatched.size(), 1);
    QCOMPARE(dispatchedId(dispatched, 0), quint64{1});

    coordinator.handleExecutorProgress(progress(1, rfm::core::TransferState::Completed));
    QCOMPARE(dispatched.size(), 2);
    QCOMPARE(dispatchedId(dispatched, 1), quint64{2});
    coordinator.handleExecutorProgress(progress(2, rfm::core::TransferState::Completed));
    QCOMPARE(dispatched.size(), 3);
    QCOMPARE(dispatchedId(dispatched, 2), quint64{3});
}

void TransferCoordinatorTest::rejectsDuplicateIdsAcrossActiveAndQueue()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator, &rfm::core::TransferCoordinator::startTransferRequested);
    QSignalSpy rejected(&coordinator, &rfm::core::TransferCoordinator::transferRejected);

    coordinator.enqueueTransfer(request(4));
    coordinator.enqueueTransfer(request(5));
    coordinator.enqueueTransfer(request(4));
    coordinator.enqueueTransfer(request(5));
    QCOMPARE(dispatched.size(), 1);
    QCOMPARE(rejected.size(), 2);
}

void TransferCoordinatorTest::nonTerminalUpdatesNeverDoubleDispatch()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator, &rfm::core::TransferCoordinator::startTransferRequested);
    coordinator.enqueueTransfer(request(11));
    coordinator.enqueueTransfer(request(12));

    coordinator.handleExecutorProgress(progress(11, rfm::core::TransferState::Preparing));
    coordinator.handleExecutorProgress(progress(11, rfm::core::TransferState::Transferring));
    coordinator.handleExecutorProgress(progress(11, rfm::core::TransferState::Paused));
    coordinator.handleExecutorProgress(progress(11, rfm::core::TransferState::Transferring));
    QCOMPARE(dispatched.size(), 1);
}

void TransferCoordinatorTest::cancelsQueuedWithoutDispatch()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator, &rfm::core::TransferCoordinator::startTransferRequested);
    QSignalSpy updates(&coordinator, &rfm::core::TransferCoordinator::transferUpdated);
    QSignalSpy activeCancellation(&coordinator,
                                  &rfm::core::TransferCoordinator::cancelActiveRequested);
    coordinator.enqueueTransfer(request(21));
    coordinator.enqueueTransfer(request(22));

    coordinator.cancelTransfer(22);
    QCOMPARE(dispatched.size(), 1);
    QCOMPARE(activeCancellation.size(), 0);
    QCOMPARE(statesFor(updates, 22),
             (QList<rfm::core::TransferState>{rfm::core::TransferState::Queued,
                                              rfm::core::TransferState::Cancelled}));
    coordinator.handleExecutorProgress(progress(21, rfm::core::TransferState::Completed));
    QCOMPARE(dispatched.size(), 1);
}

void TransferCoordinatorTest::activeCancellationWaitsForTerminal()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator, &rfm::core::TransferCoordinator::startTransferRequested);
    QSignalSpy cancellation(&coordinator, &rfm::core::TransferCoordinator::cancelActiveRequested);
    coordinator.enqueueTransfer(request(31));
    coordinator.enqueueTransfer(request(32));

    coordinator.cancelTransfer(31);
    QCOMPARE(cancellation.size(), 1);
    QCOMPARE(dispatched.size(), 1);
    coordinator.handleExecutorProgress(progress(31, rfm::core::TransferState::Cancelling));
    QCOMPARE(dispatched.size(), 1);
    coordinator.handleExecutorProgress(progress(31, rfm::core::TransferState::Cancelled));
    QCOMPARE(dispatched.size(), 2);
    QCOMPARE(dispatchedId(dispatched, 1), quint64{32});
}

void TransferCoordinatorTest::normalFailureDispatchesNext()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator, &rfm::core::TransferCoordinator::startTransferRequested);
    coordinator.enqueueTransfer(request(41));
    coordinator.enqueueTransfer(request(42));

    coordinator.handleExecutorProgress(progress(41, rfm::core::TransferState::Failed));
    QCOMPARE(dispatched.size(), 2);
    QCOMPARE(dispatchedId(dispatched, 1), quint64{42});
}

void TransferCoordinatorTest::executorFailureFailsQueueWithoutDispatch()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator, &rfm::core::TransferCoordinator::startTransferRequested);
    QSignalSpy updates(&coordinator, &rfm::core::TransferCoordinator::transferUpdated);
    coordinator.enqueueTransfer(request(51));
    coordinator.enqueueTransfer(request(52));
    coordinator.enqueueTransfer(request(53));

    coordinator.handleExecutorFailure(QStringLiteral("connection lost"));
    QCOMPARE(dispatched.size(), 1);
    QCOMPARE(statesFor(updates, 52),
             QList<rfm::core::TransferState>{rfm::core::TransferState::Queued});
    coordinator.handleExecutorProgress(progress(51, rfm::core::TransferState::Failed));

    QCOMPARE(dispatched.size(), 1);
    QCOMPARE(statesFor(updates, 51),
             (QList<rfm::core::TransferState>{rfm::core::TransferState::Queued,
                                              rfm::core::TransferState::Failed}));
    QCOMPARE(statesFor(updates, 52),
             (QList<rfm::core::TransferState>{rfm::core::TransferState::Queued,
                                              rfm::core::TransferState::Failed}));
    QCOMPARE(statesFor(updates, 53),
             (QList<rfm::core::TransferState>{rfm::core::TransferState::Queued,
                                              rfm::core::TransferState::Failed}));
}

void TransferCoordinatorTest::shutdownCancelsQueueAndStopsDispatch()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator, &rfm::core::TransferCoordinator::startTransferRequested);
    QSignalSpy updates(&coordinator, &rfm::core::TransferCoordinator::transferUpdated);
    QSignalSpy executorShutdown(&coordinator,
                                &rfm::core::TransferCoordinator::shutdownExecutorRequested);
    QSignalSpy shutdown(&coordinator, &rfm::core::TransferCoordinator::transfersShutdown);
    coordinator.enqueueTransfer(request(61));
    coordinator.enqueueTransfer(request(62));
    coordinator.enqueueTransfer(request(63));

    coordinator.shutdownTransfers();
    QCOMPARE(executorShutdown.size(), 1);
    QCOMPARE(statesFor(updates, 62).constLast(), rfm::core::TransferState::Cancelled);
    QCOMPARE(statesFor(updates, 63).constLast(), rfm::core::TransferState::Cancelled);
    coordinator.handleExecutorProgress(progress(61, rfm::core::TransferState::Cancelled));
    QCOMPARE(dispatched.size(), 1);
    coordinator.handleExecutorShutdown();
    QCOMPARE(shutdown.size(), 1);
}

void TransferCoordinatorTest::pauseAndResumeRouteOnlyToActive()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy pauses(&coordinator, &rfm::core::TransferCoordinator::pauseActiveRequested);
    QSignalSpy resumes(&coordinator, &rfm::core::TransferCoordinator::resumeActiveRequested);
    QSignalSpy rejected(&coordinator, &rfm::core::TransferCoordinator::transferRejected);
    coordinator.enqueueTransfer(request(71));
    coordinator.enqueueTransfer(request(72));

    coordinator.pauseTransfer(71);
    coordinator.resumeTransfer(71);
    coordinator.pauseTransfer(72);
    coordinator.resumeTransfer(72);
    QCOMPARE(pauses.size(), 1);
    QCOMPARE(resumes.size(), 1);
    QCOMPARE(rejected.size(), 2);
}

QTEST_MAIN(TransferCoordinatorTest)
#include "test_transfer_coordinator.moc"
