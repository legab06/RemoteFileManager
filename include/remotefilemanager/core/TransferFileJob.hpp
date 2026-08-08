#pragma once

#include "remotefilemanager/core/RemoteTransferBackend.hpp"
#include "remotefilemanager/core/TransferJob.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryFile>

#include <memory>

namespace rfm::core
{

class TransferFileJob final : public TransferJob
{
  public:
    TransferFileJob(RemoteTransferBackend& backend, TransferRequest request);

    void step() override;
    [[nodiscard]] bool requestPause() override;
    [[nodiscard]] bool resume() override;
    [[nodiscard]] bool requestCancel() override;
    [[nodiscard]] bool isFinished() const override;
    [[nodiscard]] bool isPaused() const override;
    [[nodiscard]] const TransferProgress& progress() const override;

  private:
    enum class Phase {
        Created,
        OpenLocal,
        OpenRemote,
        Transfer,
        Close,
        Finalize,
        CancelCloseRemote,
        CancelCloseLocal,
        CancelRemoveTemporary,
        Finished,
    };

    void fail(const QString& error);
    void appendCleanupError(const QString& error);
    void updateSpeed();

    RemoteTransferBackend& m_backend;
    TransferRequest m_request;
    TransferProgress m_progress;
    Phase m_phase{Phase::Created};
    TransferState m_stateBeforePause{TransferState::Preparing};
    QFile m_localFile;
    std::unique_ptr<QTemporaryFile> m_temporary;
    quint64 m_handle{0};
    QString m_remoteTemporary;
    bool m_ownsRemoteTemporary{false};
    QElapsedTimer m_activeTimer;
    qint64 m_elapsedBeforePause{0};
};

} // namespace rfm::core
