#pragma once

#include "remotefilemanager/core/OperationProgress.hpp"

#include <QElapsedTimer>

#include <functional>
#include <optional>

namespace rfm::core
{

struct RemoteCopyTelemetry {
    quint64 transferredBytes{0};
    quint64 totalBytes{0};
    bool byteProgressAvailable{false};
};

struct RemoteCopyPoll {
    std::optional<RemoteBackendResult> result;
    RemoteCopyTelemetry telemetry;
};

struct RemoteCopyPreflightPoll {
    std::optional<RemoteBackendResult> result;
    std::optional<quint64> totalBytes;
};

class ServerSideCopyBackend
{
  public:
    virtual ~ServerSideCopyBackend() = default;
    [[nodiscard]] virtual RemoteProbeResult probe(const QString& path) = 0;
    [[nodiscard]] virtual RemoteBackendResult rename(const QString& source,
                                                     const QString& destination) = 0;
    [[nodiscard]] virtual RemoteBackendResult
    startCopy(const QString& source, const QString& destination, bool recursive) = 0;
    // Starts an optional, cooperative source-size preflight. Backends that cannot provide an
    // exact total leave the default terminal result unchanged and the job stays indeterminate.
    [[nodiscard]] virtual RemoteBackendResult startCopyPreflight(const QList<RemoteSelection>&)
    {
        return {};
    }
    // A missing result means the preflight should continue on a later worker step.
    [[nodiscard]] virtual RemoteCopyPreflightPoll pollCopyPreflight()
    {
        return {RemoteBackendResult{}, std::nullopt};
    }
    virtual void cancelCopyPreflight() {}
    [[nodiscard]] virtual RemoteBackendResult reserveStaging(const QString& path) = 0;
    [[nodiscard]] virtual RemoteBackendResult removeEmptyDirectory(const QString& path) = 0;
    [[nodiscard]] virtual RemoteBackendResult startMoveStagingCopy(const QString& source,
                                                                   const QString& destination) = 0;
    [[nodiscard]] virtual RemoteCopyPoll pollCopy() = 0;
    [[nodiscard]] virtual RemoteBackendResult startRemove(const QString& path, bool recursive,
                                                          bool protectMountPoint) = 0;
    [[nodiscard]] virtual std::optional<RemoteBackendResult> pollRemove() = 0;
    // A missing result means the termination request should be retried later.
    [[nodiscard]] virtual std::optional<RemoteBackendResult> requestCopyCancellation() = 0;
    // A missing result means the remote process/channel is not terminal yet.
    [[nodiscard]] virtual std::optional<RemoteBackendResult> pollCopyCancellation() = 0;
};

class ServerSideCopyJob final
{
  public:
    using MonotonicClock = std::function<qint64()>;

    ServerSideCopyJob(ServerSideCopyBackend& backend, quint64 id, QList<RemoteSelection> sources,
                      QString destinationDirectory,
                      RemoteOperationKind operationKind = RemoteOperationKind::Copy,
                      MonotonicClock monotonicClock = {});

    void step();
    void failTransport(QString error);
    [[nodiscard]] bool requestCancel();
    [[nodiscard]] bool isFinished() const;
    [[nodiscard]] std::optional<QString> ownedStagingPath() const;
    [[nodiscard]] bool ownsInternalPath(const QString& path) const;
    [[nodiscard]] bool hidesListingEntry(const QString& parentPath, const QString& entryName) const;
    [[nodiscard]] const OperationProgress& progress() const;
    [[nodiscard]] const RemoteOperationResult& result() const;

  private:
    enum class Phase {
        Preflight,
        Prepare,
        RenameItem,
        PrepareStaging,
        StartItem,
        PollItem,
        PromoteItem,
        RemoveEmptyStaging,
        StartCleanup,
        PollCleanup,
        StartRemove,
        PollRemove,
        RequestCancellation,
        PollCancellation,
        Finished
    };

    enum class CleanupContinuation {
        None,
        FinishSuccess,
        FinishFailure,
        FinishCancellation,
        RemoveSource
    };

    void prepareItem();
    void applyCopyTelemetry(const RemoteCopyTelemetry& telemetry);
    void completeCurrentItemByteProgress(const RemoteBackendResult& result);
    void disableByteProgress();
    void finishPreflight(const RemoteBackendResult& result);
    void startCopySpeedMeasurement();
    void updateCopySpeed();
    void resetCopySpeed();
    [[nodiscard]] qint64 monotonicMilliseconds() const;
    void finishItem(const RemoteBackendResult& result);
    void finishRemoval(const RemoteBackendResult& result);
    void finishCancellation(const RemoteBackendResult& result);
    void beginPostPromotionCleanup();
    void beginCleanup(CleanupContinuation continuation, const RemoteBackendResult& result);
    void finishCleanup(const RemoteBackendResult& cleanupResult);
    [[nodiscard]] RemoteBackendResult
    cleanupFailure(const RemoteBackendResult& cleanupResult) const;
    void finish();
    void appendCancelledItems();
    [[nodiscard]] QString describeError(const RemoteBackendResult& result) const;
    [[nodiscard]] QString stagingDirectory(const QString& destination) const;

    ServerSideCopyBackend& m_backend;
    QList<RemoteSelection> m_sources;
    QString m_destinationDirectory;
    OperationProgress m_progress;
    RemoteOperationResult m_result;
    RemoteOperationKind m_operationKind{RemoteOperationKind::Copy};
    QString m_stagingDirectory;
    RemoteBackendResult m_pendingResult;
    CleanupContinuation m_cleanupContinuation{CleanupContinuation::None};
    std::optional<quint64> m_preflightTotalBytes;
    qsizetype m_sourceIndex{0};
    Phase m_phase{Phase::Preflight};
    quint64 m_completedTransferredBytes{0};
    quint64 m_currentItemTransferredBytes{0};
    quint64 m_speedSampleBytes{0};
    qint64 m_speedSampleMilliseconds{0};
    MonotonicClock m_monotonicClock;
    QElapsedTimer m_speedTimer;
    bool m_copyActive{false};
    bool m_preflightActive{false};
    bool m_currentItemByteProgressAvailable{false};
    bool m_speedMeasurementActive{false};
    bool m_stagingOwned{false};
};

} // namespace rfm::core
