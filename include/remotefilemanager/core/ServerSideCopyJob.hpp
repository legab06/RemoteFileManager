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
    [[nodiscard]] virtual RemoteBackendResult startCopy(const QString& source,
                                                        const QString& destination,
                                                        bool recursive) = 0;
    [[nodiscard]] virtual std::optional<RemoteBackendResult> pollCopy() = 0;
    [[nodiscard]] virtual RemoteBackendResult cancelCopy() = 0;
};

class ServerSideCopyJob final
{
  public:
    ServerSideCopyJob(ServerSideCopyBackend& backend, quint64 id,
                      QList<RemoteSelection> sources, QString destinationDirectory);

    void step();
    [[nodiscard]] bool requestCancel();
    [[nodiscard]] bool isFinished() const;
    [[nodiscard]] const OperationProgress& progress() const;
    [[nodiscard]] const RemoteOperationResult& result() const;

  private:
    enum class Phase { Prepare, StartItem, PollItem, Finished };

    void prepareItem();
    void finishItem(const RemoteBackendResult& result);
    void finish();
    void appendCancelledItems();
    [[nodiscard]] QString describeError(const RemoteBackendResult& result) const;

    ServerSideCopyBackend& m_backend;
    QList<RemoteSelection> m_sources;
    QString m_destinationDirectory;
    OperationProgress m_progress;
    RemoteOperationResult m_result;
    qsizetype m_sourceIndex{0};
    Phase m_phase{Phase::Prepare};
    bool m_copyActive{false};
};

} // namespace rfm::core
