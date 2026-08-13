#pragma once

#include "remotefilemanager/core/OperationProgress.hpp"

#include <optional>

namespace rfm::core
{

class ServerSideCopyBackend
{
  public:
    virtual ~ServerSideCopyBackend() = default;
    [[nodiscard]] virtual RemoteProbeResult probe(const QString& path) = 0;
    [[nodiscard]] virtual RemoteBackendResult rename(const QString& source,
                                                     const QString& destination) = 0;
    [[nodiscard]] virtual RemoteBackendResult
    startCopy(const QString& source, const QString& destination, bool recursive) = 0;
    [[nodiscard]] virtual RemoteBackendResult reserveMoveStaging(const QString& path) = 0;
    [[nodiscard]] virtual RemoteBackendResult startMoveStagingCopy(const QString& source,
                                                                   const QString& destination) = 0;
    [[nodiscard]] virtual std::optional<RemoteBackendResult> pollCopy() = 0;
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
    ServerSideCopyJob(ServerSideCopyBackend& backend, quint64 id, QList<RemoteSelection> sources,
                      QString destinationDirectory,
                      RemoteOperationKind operationKind = RemoteOperationKind::Copy);

    void step();
    [[nodiscard]] bool requestCancel();
    [[nodiscard]] bool isFinished() const;
    [[nodiscard]] const OperationProgress& progress() const;
    [[nodiscard]] const RemoteOperationResult& result() const;

  private:
    enum class Phase {
        Prepare,
        RenameItem,
        PrepareFallback,
        StartItem,
        PollItem,
        PromoteItem,
        StartCleanup,
        PollCleanup,
        StartRemove,
        PollRemove,
        RequestCancellation,
        PollCancellation,
        Finished
    };

    enum class CleanupContinuation { None, FinishFailure, FinishCancellation, RemoveSource };

    void prepareItem();
    void finishItem(const RemoteBackendResult& result);
    void finishRemoval(const RemoteBackendResult& result);
    void finishCancellation(const RemoteBackendResult& result);
    void beginCleanup(CleanupContinuation continuation, const RemoteBackendResult& result);
    void finishCleanup(const RemoteBackendResult& cleanupResult);
    [[nodiscard]] RemoteBackendResult
    cleanupFailure(const RemoteBackendResult& cleanupResult) const;
    void finish();
    void appendCancelledItems();
    [[nodiscard]] QString describeError(const RemoteBackendResult& result) const;
    [[nodiscard]] QString fallbackDestination(const QString& destination) const;

    ServerSideCopyBackend& m_backend;
    QList<RemoteSelection> m_sources;
    QString m_destinationDirectory;
    OperationProgress m_progress;
    RemoteOperationResult m_result;
    RemoteOperationKind m_operationKind{RemoteOperationKind::Copy};
    QString m_fallbackDestination;
    RemoteBackendResult m_pendingResult;
    CleanupContinuation m_cleanupContinuation{CleanupContinuation::None};
    qsizetype m_sourceIndex{0};
    Phase m_phase{Phase::Prepare};
    bool m_copyActive{false};
    bool m_fallbackOwned{false};
};

} // namespace rfm::core
