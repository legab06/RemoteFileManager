#pragma once

#include "remotefilemanager/core/RemoteTransferBackend.hpp"
#include "remotefilemanager/core/TransferFileJob.hpp"
#include "remotefilemanager/core/TransferJob.hpp"

#include <QDirIterator>
#include <QQueue>
#include <QVector>

#include <memory>

namespace rfm::core
{

class TransferDirectoryJob final : public TransferJob
{
  public:
    TransferDirectoryJob(RemoteTransferBackend& backend, TransferRequest request);

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
        DiscoverUpload,
        DiscoverRemoteOpen,
        DiscoverRemoteRead,
        DiscoverRemoteClose,
        CreateRoot,
        CreateDirectories,
        TransferFiles,
        Finalize,
        CancelChild,
        CancelCloseDirectory,
        Finished,
    };

    struct DirectoryTask {
        QString source;
        QString destination;
    };

    struct FileTask {
        QString source;
        QString destination;
    };

    void prepare();
    void discoverUploadEntry();
    void openRemoteDirectory();
    void readRemoteDirectoryEntry();
    void closeRemoteDirectory();
    void finishDiscovery();
    void createRootDirectory();
    void createNextDirectory();
    void transferNextFileStep();
    void cancelStep();
    void fail(const QString& error, const QString& item = {});
    void appendCleanupError(const QString& error);
    void updateFromChild();

    RemoteTransferBackend& m_backend;
    TransferRequest m_request;
    TransferProgress m_progress;
    Phase m_phase{Phase::Created};
    TransferState m_stateBeforePause{TransferState::Preparing};
    std::unique_ptr<QDirIterator> m_localIterator;
    QQueue<DirectoryTask> m_pendingRemoteDirectories;
    DirectoryTask m_currentRemoteDirectory;
    quint64 m_remoteDirectoryHandle{0};
    QVector<QString> m_directories;
    QVector<FileTask> m_files;
    qsizetype m_directoryIndex{0};
    qsizetype m_fileIndex{0};
    quint64 m_completedBytes{0};
    std::unique_ptr<TransferFileJob> m_child;
};

} // namespace rfm::core
