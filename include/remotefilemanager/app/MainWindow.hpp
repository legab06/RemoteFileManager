#pragma once

#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/OperationProgress.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/TransferTypes.hpp"

#include <QMainWindow>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QStringList>

class QAction;
class QPoint;
class QThread;
class QTimer;

namespace rfm::ssh
{
class SshSession;
}

namespace rfm::app
{

class PaneWorkspace;
class OperationPanel;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

  signals:
    void connectionRequested(rfm::core::ConnectionProfile profile, QString password);
    void hostKeyDecision(bool accepted);
    void directoryRequested(quint64 requestId, QString path);
    void createDirectoryRequested(quint64 id, QString parent, QString name);
    void renameRequested(quint64 id, QString source, QString newName);
    void moveRequested(quint64 id, QList<rfm::core::RemoteSelection> sources,
                       QString destinationDirectory);
    void copyRequested(quint64 id, QList<rfm::core::RemoteSelection> sources,
                       QString destinationDirectory);
    void removeRequested(quint64 id, QList<rfm::core::RemoteSelection> sources, bool recursive);
    void transferRequested(rfm::core::TransferRequest request);
    void pauseTransferRequested(quint64 id);
    void resumeTransferRequested(quint64 id);
    void cancelTransferRequested(quint64 id);
    void shutdownRequested();
    void disconnectionRequested();

  private:
    void createActions();
    void createMenus();
    void createPaneWorkspace();
    void connectBrowserPane(quint64 paneId);
    void createNavigationBar();
    void createPlacesDock();
    void createOperationDock();
    void createEmptyState();
    void showConnectionDialog();
    void showAboutDialog();
    void showHostKeyConfirmation(const QString& host, const QString& fingerprint);
    Q_INVOKABLE void showRemoteDirectory(const QString& path,
                                         const QList<rfm::core::RemoteEntry>& entries);
    Q_INVOKABLE void handleDirectoryListed(quint64 requestId, const QString& path,
                                           const QList<rfm::core::RemoteEntry>& entries);
    Q_INVOKABLE void handleDirectoryListingError(quint64 requestId, const QString& path,
                                                 const QString& error);
    void showConnectionError(const QString& message);
    void requestParentDirectory();
    void showFileContextMenu(const QPoint& globalPosition);
    void createRemoteDirectory();
    void renameSelectedEntry();
    void moveSelectedEntries();
    void copySelectedEntries();
    void moveSelectedToOtherPane();
    void copySelectedToOtherPane();
    void removeSelectedEntries();
    void chooseUploads();
    void chooseDownloadDirectory();
    Q_INVOKABLE void queueUploads(QStringList localPaths);
    Q_INVOKABLE void queueDownloads(QString localDirectory);
    Q_INVOKABLE void handleOperationResult(const rfm::core::RemoteOperationResult& result);
    Q_INVOKABLE void handleTransferProgress(const rfm::core::TransferProgress& progress);
    void beginTrackedRemoteOperation(quint64 id, rfm::core::OperationKind kind,
                                     const QList<rfm::core::RemoteSelection>& sources,
                                     const QString& destination);
    void updateOperationActions();
    void updateConnectionAction();
    void requestDirectoryListing(quint64 paneId, const QString& path, bool showBusy,
                                 bool coalesceIfPending,
                                 PaneNavigation navigation = PaneNavigation::Refresh);
    void startNextDirectoryListing();
    void cancelDirectoryRequests(quint64 paneId);
    void setPaneBusy(quint64 paneId, bool busy, const QString& message = {});
    void schedulePaneRefresh(quint64 paneId, bool showBusy);
    void scheduleVisiblePanesForPaths(const QSet<QString>& paths, bool showBusy);
    [[nodiscard]] bool confirmOtherPaneOperation(const QString& operation,
                                                 const QList<rfm::core::RemoteSelection>& sources,
                                                 const QString& destination);
    void stopAutomaticRefresh();
    [[nodiscard]] QList<rfm::core::RemoteSelection> selectedEntries() const;
    [[nodiscard]] QString askDestination(const QString& title);
    [[nodiscard]] quint64 nextOperationId();
    void setBusy(bool busy, const QString& message = {});

    QAction* m_newConnectionAction{nullptr};
    QAction* m_quitAction{nullptr};
    QAction* m_aboutAction{nullptr};
    QAction* m_backAction{nullptr};
    QAction* m_forwardAction{nullptr};
    QAction* m_upAction{nullptr};
    QAction* m_refreshAction{nullptr};
    QAction* m_createDirectoryAction{nullptr};
    QAction* m_renameAction{nullptr};
    QAction* m_moveAction{nullptr};
    QAction* m_copyAction{nullptr};
    QAction* m_moveToOtherPaneAction{nullptr};
    QAction* m_copyToOtherPaneAction{nullptr};
    QAction* m_removeAction{nullptr};
    QAction* m_uploadAction{nullptr};
    QAction* m_downloadAction{nullptr};
    QAction* m_splitViewAction{nullptr};
    PaneWorkspace* m_paneWorkspace{nullptr};
    OperationPanel* m_operationPanel{nullptr};
    QTimer* m_autoRefreshTimer{nullptr};
    QTimer* m_refreshDebounceTimer{nullptr};
    QThread* m_sshThread{nullptr};
    rfm::ssh::SshSession* m_sshSession{nullptr};
    rfm::core::ConnectionProfile m_activeProfile;
    QSet<quint64> m_pendingTransferRequests;
    QSet<quint64> m_nonTerminalTransfers;
    struct DirectoryRequest {
        quint64 id{0};
        quint64 paneId{0};
        QString path;
        PaneNavigation navigation{PaneNavigation::Refresh};
    };
    QHash<quint64, DirectoryRequest> m_directoryRequests;
    QQueue<quint64> m_directoryQueue;
    QHash<quint64, quint64> m_expectedDirectoryRequests;
    QSet<quint64> m_busyPanes;
    QHash<quint64, bool> m_scheduledPaneRefreshes;
    struct OperationContext {
        quint64 sourcePaneId{0};
        quint64 destinationPaneId{0};
        QString sourceDirectory;
        QString destinationDirectory;
    };
    QHash<quint64, OperationContext> m_operationContexts;
    QHash<quint64, rfm::core::OperationProgress> m_remoteOperations;
    QHash<quint64, quint64> m_transferPanes;
    quint64 m_activeDirectoryRequestId{0};
    quint64 m_nextOperationId{1};
    quint64 m_connectionGeneration{0};
    bool m_connected{false};
    bool m_busy{false};
};

} // namespace rfm::app
