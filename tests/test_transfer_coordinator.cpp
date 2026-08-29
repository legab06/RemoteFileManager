#include "remotefilemanager/core/TransferCoordinator.hpp"

#include <QSignalSpy>
#include <QTest>

#include <algorithm>

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

rfm::core::RemoteOperationRequest
remoteRequest(quint64 id,
              rfm::core::RemoteOperationKind kind = rfm::core::RemoteOperationKind::Copy)
{
    return {id,
            kind,
            {{QStringLiteral("/source/item-%1").arg(id), false}},
            QStringLiteral("/destination")};
}

rfm::core::OperationProgress
remoteProgress(quint64 id, rfm::core::OperationState state,
               rfm::core::RemoteOperationKind kind = rfm::core::RemoteOperationKind::Copy)
{
    return rfm::core::operationProgress(remoteRequest(id, kind), state);
}

rfm::core::RemoteOperationResult
remoteResult(quint64 id, bool success,
             rfm::core::RemoteOperationKind kind = rfm::core::RemoteOperationKind::Copy)
{
    return {
        id,
        kind,
        {{QStringLiteral("/source/item-%1").arg(id), QStringLiteral("/destination/item-%1").arg(id),
          success, success ? QString{} : QStringLiteral("remote failure")}}};
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

quint64 dispatchedRemoteId(const QSignalSpy& spy, qsizetype index)
{
    return qvariant_cast<rfm::core::RemoteOperationRequest>(spy.at(index).constFirst()).id;
}

QList<rfm::core::OperationState> remoteStatesFor(const QSignalSpy& spy, quint64 id)
{
    QList<rfm::core::OperationState> states;
    for (const QList<QVariant>& arguments : spy) {
        const auto value = qvariant_cast<rfm::core::OperationProgress>(arguments.constFirst());
        if (value.id == id) {
            states.push_back(value.state);
        }
    }
    return states;
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
    void dispatchesRemoteOperationsInFifoOrder();
    void dispatchesTransferAndRemoteOperationIndependently();
    void preservesMixedCopyMoveOrder();
    void remoteNonTerminalUpdatesNeverDoubleDispatch();
    void remoteCompletionAndNormalFailureDispatchNext();
    void activeRemoteCancellationWaitsForCleanupResult();
    void cancelsQueuedCopyAndMoveWithoutDispatch();
    void rejectsRemoteDuplicatesAndCrossLaneCollisions();
    void remoteShutdownAndDisconnectCancelQueueWithoutDispatch();
    void remoteExecutorFailureFailsActiveAndQueueExactlyOnce();
};

void TransferCoordinatorTest::initTestCase()
{
    qRegisterMetaType<rfm::core::TransferRequest>();
    qRegisterMetaType<rfm::core::TransferProgress>();
    qRegisterMetaType<rfm::core::RemoteOperationRequest>();
    qRegisterMetaType<rfm::core::OperationProgress>();
    qRegisterMetaType<rfm::core::RemoteOperationResult>();
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

void TransferCoordinatorTest::dispatchesRemoteOperationsInFifoOrder()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator,
                          &rfm::core::TransferCoordinator::startRemoteOperationRequested);

    coordinator.enqueueRemoteOperation(remoteRequest(101));
    coordinator.enqueueRemoteOperation(remoteRequest(102));
    coordinator.enqueueRemoteOperation(remoteRequest(103));
    QCOMPARE(dispatched.size(), 1);
    QCOMPARE(dispatchedRemoteId(dispatched, 0), quint64{101});

    coordinator.handleRemoteExecutorProgress(
        remoteProgress(101, rfm::core::OperationState::Completed));
    coordinator.handleRemoteExecutorResult(remoteResult(101, true));
    QCOMPARE(dispatchedRemoteId(dispatched, 1), quint64{102});
    coordinator.handleRemoteExecutorProgress(
        remoteProgress(102, rfm::core::OperationState::Completed));
    coordinator.handleRemoteExecutorResult(remoteResult(102, true));
    QCOMPARE(dispatchedRemoteId(dispatched, 2), quint64{103});
}

void TransferCoordinatorTest::dispatchesTransferAndRemoteOperationIndependently()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy transfers(&coordinator, &rfm::core::TransferCoordinator::startTransferRequested);
    QSignalSpy remoteOperations(&coordinator,
                                &rfm::core::TransferCoordinator::startRemoteOperationRequested);

    coordinator.enqueueTransfer(request(106));
    coordinator.enqueueRemoteOperation(remoteRequest(107));

    QCOMPARE(transfers.size(), 1);
    QCOMPARE(remoteOperations.size(), 1);
    QCOMPARE(dispatchedId(transfers, 0), quint64{106});
    QCOMPARE(dispatchedRemoteId(remoteOperations, 0), quint64{107});
}

void TransferCoordinatorTest::preservesMixedCopyMoveOrder()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator,
                          &rfm::core::TransferCoordinator::startRemoteOperationRequested);

    coordinator.enqueueRemoteOperation(remoteRequest(111));
    coordinator.enqueueRemoteOperation(remoteRequest(112, rfm::core::RemoteOperationKind::Move));
    coordinator.enqueueRemoteOperation(remoteRequest(113));
    coordinator.handleRemoteExecutorProgress(
        remoteProgress(111, rfm::core::OperationState::Completed));
    coordinator.handleRemoteExecutorResult(remoteResult(111, true));
    coordinator.handleRemoteExecutorProgress(remoteProgress(
        112, rfm::core::OperationState::Completed, rfm::core::RemoteOperationKind::Move));
    coordinator.handleRemoteExecutorResult(
        remoteResult(112, true, rfm::core::RemoteOperationKind::Move));

    QCOMPARE(dispatched.size(), 3);
    QCOMPARE(dispatchedRemoteId(dispatched, 0), quint64{111});
    QCOMPARE(dispatchedRemoteId(dispatched, 1), quint64{112});
    QCOMPARE(dispatchedRemoteId(dispatched, 2), quint64{113});
    QCOMPARE(qvariant_cast<rfm::core::RemoteOperationRequest>(dispatched.at(1).constFirst()).kind,
             rfm::core::RemoteOperationKind::Move);
}

void TransferCoordinatorTest::remoteNonTerminalUpdatesNeverDoubleDispatch()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator,
                          &rfm::core::TransferCoordinator::startRemoteOperationRequested);
    coordinator.enqueueRemoteOperation(remoteRequest(121));
    coordinator.enqueueRemoteOperation(remoteRequest(122));

    coordinator.handleRemoteExecutorProgress(
        remoteProgress(121, rfm::core::OperationState::Preparing));
    coordinator.handleRemoteExecutorProgress(
        remoteProgress(121, rfm::core::OperationState::Running));
    coordinator.handleRemoteExecutorProgress(
        remoteProgress(121, rfm::core::OperationState::Finalizing));
    QCOMPARE(dispatched.size(), 1);
}

void TransferCoordinatorTest::remoteCompletionAndNormalFailureDispatchNext()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator,
                          &rfm::core::TransferCoordinator::startRemoteOperationRequested);
    coordinator.enqueueRemoteOperation(remoteRequest(131));
    coordinator.enqueueRemoteOperation(remoteRequest(132));
    coordinator.enqueueRemoteOperation(remoteRequest(133));

    coordinator.handleRemoteExecutorProgress(
        remoteProgress(131, rfm::core::OperationState::Completed));
    coordinator.handleRemoteExecutorResult(remoteResult(131, true));
    QCOMPARE(dispatchedRemoteId(dispatched, 1), quint64{132});
    coordinator.handleRemoteExecutorProgress(
        remoteProgress(132, rfm::core::OperationState::Failed));
    coordinator.handleRemoteExecutorResult(remoteResult(132, false));
    QCOMPARE(dispatchedRemoteId(dispatched, 2), quint64{133});
}

void TransferCoordinatorTest::activeRemoteCancellationWaitsForCleanupResult()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator,
                          &rfm::core::TransferCoordinator::startRemoteOperationRequested);
    QSignalSpy cancellations(&coordinator,
                             &rfm::core::TransferCoordinator::cancelActiveRemoteOperationRequested);
    coordinator.enqueueRemoteOperation(remoteRequest(141));
    coordinator.enqueueRemoteOperation(remoteRequest(142));

    coordinator.cancelRemoteOperation(141);
    QCOMPARE(cancellations.size(), 1);
    coordinator.handleRemoteExecutorProgress(
        remoteProgress(141, rfm::core::OperationState::Cancelling));
    coordinator.handleRemoteExecutorProgress(
        remoteProgress(141, rfm::core::OperationState::Cancelled));
    // The executor emits its result only after ServerSideCopyJob has completed staging cleanup.
    // A terminal progress update alone must not release the FIFO slot.
    QCOMPARE(dispatched.size(), 1);
    coordinator.handleRemoteExecutorResult(remoteResult(141, false));
    QCOMPARE(dispatched.size(), 2);
    QCOMPARE(dispatchedRemoteId(dispatched, 1), quint64{142});
}

void TransferCoordinatorTest::cancelsQueuedCopyAndMoveWithoutDispatch()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator,
                          &rfm::core::TransferCoordinator::startRemoteOperationRequested);
    QSignalSpy updates(&coordinator, &rfm::core::TransferCoordinator::remoteOperationUpdated);
    QSignalSpy results(&coordinator, &rfm::core::TransferCoordinator::remoteOperationFinished);
    coordinator.enqueueRemoteOperation(remoteRequest(151));
    coordinator.enqueueRemoteOperation(remoteRequest(152));
    coordinator.enqueueRemoteOperation(remoteRequest(153, rfm::core::RemoteOperationKind::Move));

    coordinator.cancelRemoteOperation(152);
    coordinator.cancelRemoteOperation(153);
    QCOMPARE(remoteStatesFor(updates, 152),
             (QList<rfm::core::OperationState>{rfm::core::OperationState::Queued,
                                               rfm::core::OperationState::Cancelled}));
    QCOMPARE(remoteStatesFor(updates, 153),
             (QList<rfm::core::OperationState>{rfm::core::OperationState::Queued,
                                               rfm::core::OperationState::Cancelled}));
    QCOMPARE(results.size(), 2);
    QCOMPARE(dispatched.size(), 1);
    coordinator.handleRemoteExecutorProgress(
        remoteProgress(151, rfm::core::OperationState::Completed));
    coordinator.handleRemoteExecutorResult(remoteResult(151, true));
    QCOMPARE(dispatched.size(), 1);
}

void TransferCoordinatorTest::rejectsRemoteDuplicatesAndCrossLaneCollisions()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy remoteRejected(&coordinator,
                              &rfm::core::TransferCoordinator::remoteOperationRejected);
    QSignalSpy transferRejected(&coordinator, &rfm::core::TransferCoordinator::transferRejected);
    coordinator.enqueueRemoteOperation(remoteRequest(161));
    coordinator.enqueueRemoteOperation(remoteRequest(162));
    coordinator.enqueueRemoteOperation(remoteRequest(161));
    coordinator.enqueueRemoteOperation(remoteRequest(162));
    coordinator.enqueueTransfer(request(161));
    coordinator.enqueueTransfer(request(162));
    QCOMPARE(remoteRejected.size(), 2);
    QCOMPARE(transferRejected.size(), 2);

    coordinator.enqueueTransfer(request(163));
    coordinator.enqueueRemoteOperation(remoteRequest(163));
    QCOMPARE(remoteRejected.size(), 3);
}

void TransferCoordinatorTest::remoteShutdownAndDisconnectCancelQueueWithoutDispatch()
{
    const auto verifyStop = [](bool disconnect) {
        rfm::core::TransferCoordinator coordinator;
        coordinator.executorConnected();
        QSignalSpy dispatched(&coordinator,
                              &rfm::core::TransferCoordinator::startRemoteOperationRequested);
        QSignalSpy updates(&coordinator, &rfm::core::TransferCoordinator::remoteOperationUpdated);
        QSignalSpy silentResults(&coordinator,
                                 &rfm::core::TransferCoordinator::remoteOperationResultSilent);
        coordinator.enqueueRemoteOperation(remoteRequest(171));
        coordinator.enqueueRemoteOperation(remoteRequest(172));
        coordinator.enqueueRemoteOperation(remoteRequest(173));

        if (disconnect) {
            coordinator.disconnectExecutor();
        } else {
            coordinator.shutdownTransfers();
        }
        QCOMPARE(remoteStatesFor(updates, 172).constLast(), rfm::core::OperationState::Cancelled);
        QCOMPARE(remoteStatesFor(updates, 173).constLast(), rfm::core::OperationState::Cancelled);
        coordinator.handleRemoteExecutorProgress(
            remoteProgress(171, rfm::core::OperationState::Cancelled));
        coordinator.handleRemoteExecutorResult(remoteResult(171, false));
        QCOMPARE(dispatched.size(), 1);
        QCOMPARE(silentResults.size(), 3);
    };

    verifyStop(false);
    verifyStop(true);
}

void TransferCoordinatorTest::remoteExecutorFailureFailsActiveAndQueueExactlyOnce()
{
    rfm::core::TransferCoordinator coordinator;
    coordinator.executorConnected();
    QSignalSpy dispatched(&coordinator,
                          &rfm::core::TransferCoordinator::startRemoteOperationRequested);
    QSignalSpy updates(&coordinator, &rfm::core::TransferCoordinator::remoteOperationUpdated);
    QSignalSpy results(&coordinator, &rfm::core::TransferCoordinator::remoteOperationFinished);
    QSignalSpy silentResults(&coordinator,
                             &rfm::core::TransferCoordinator::remoteOperationResultSilent);
    coordinator.enqueueRemoteOperation(remoteRequest(181));
    coordinator.enqueueRemoteOperation(remoteRequest(182));
    coordinator.enqueueRemoteOperation(remoteRequest(183));

    coordinator.handleRemoteExecutorFailure(QStringLiteral("connection lost"));
    QCOMPARE(dispatched.size(), 1);
    QCOMPARE(results.size(), 2);
    coordinator.handleRemoteExecutorProgress(
        remoteProgress(181, rfm::core::OperationState::Failed));
    const rfm::core::RemoteOperationResult partialResult{
        181,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/A"), QStringLiteral("/destination/A"), true, {}},
         {QStringLiteral("/source/B"), QStringLiteral("/destination/B"), false,
          QStringLiteral("connection lost; staging may remain")},
         {QStringLiteral("/source/C"), QStringLiteral("/destination/C"), false,
          QStringLiteral("not started")}}};
    coordinator.handleRemoteExecutorResult(partialResult);

    QCOMPARE(dispatched.size(), 1);
    QCOMPARE(results.size(), 3);
    QCOMPARE(silentResults.size(), 3);
    const auto publishedActive =
        qvariant_cast<rfm::core::RemoteOperationResult>(results.constLast().constFirst());
    QCOMPARE(publishedActive.items.size(), 3);
    QVERIFY(publishedActive.items.at(0).success);
    QVERIFY(!publishedActive.items.at(1).success);
    QVERIFY(!publishedActive.items.at(2).success);
    for (const quint64 id : {quint64{181}, quint64{182}, quint64{183}}) {
        const QList<rfm::core::OperationState> states = remoteStatesFor(updates, id);
        QCOMPARE(states.constFirst(), rfm::core::OperationState::Queued);
        QCOMPARE(states.constLast(), rfm::core::OperationState::Failed);
        QCOMPARE(std::ranges::count(states, rfm::core::OperationState::Failed), 1);
    }
}

QTEST_MAIN(TransferCoordinatorTest)
#include "test_transfer_coordinator.moc"
