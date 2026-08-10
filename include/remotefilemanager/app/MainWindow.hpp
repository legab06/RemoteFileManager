#pragma once

#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/InternalTransfer.hpp"
#include "remotefilemanager/core/OperationProgress.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/TransferTypes.hpp"

#include <QMainWindow>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QStringList>

#include <memory>

class QAction;
class QLabel;
class QPoint;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QThread;
class QTimer;

namespace rfm::ssh
{
class SshSession;
}

namespace rfm::core
{
class OperationHistoryStore;
class ServerProfileStore;
}

namespace rfm::app
{

class PaneWorkspace;
class OperationPanel;
class ConnectionDialog;
class HomePage;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr, QString operationHistoryDirectory = {},
                        QString serverProfileDirectory = {});
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
    void createCentralPages();
    void showConnectionDialog();
    void showConnectionDialogForProfile(const rfm::core::ConnectionProfile& profile);
    void beginConnection(const rfm::core::ConnectionProfile& profile, const QString& password);
    [[nodiscard]] QString saveConnectedProfileIfRequested();
    Q_INVOKABLE void handleConnected(const QString& path,
                                     const QList<rfm::core::RemoteEntry>& entries);
    void showAboutDialog();
    void showHostKeyConfirmation(const QString& host, const QString& fingerprint);
    Q_INVOKABLE void showRemoteDirectory(const QString& path,
                                         const QList<rfm::core::RemoteEntry>& entries);
    Q_INVOKABLE void handleDirectoryListed(quint64 requestId, const QString& path,
                                           const QList<rfm::core::RemoteEntry>& entries);
    Q_INVOKABLE void handleDirectoryListingError(quint64 requestId, const QString& path,
                                                 const QString& error);
    Q_INVOKABLE void showConnectionError(const QString& message);
    void loadServerProfiles();
    void refreshServerProfileViews();
    void updateSelectedServerAction();
    void addServerProfile();
    void editSelectedServerProfile();
    void removeSelectedServerProfile();
    void connectToSelectedServerProfile();
    void connectToServerProfile(const QString& id);
    void showServerProfileContextMenu(const QPoint& position);
    void requestDisconnection();
    [[nodiscard]] rfm::core::ConnectionProfile selectedServerProfile() const;
    Q_INVOKABLE void handleDisconnected();
    void resetDisconnectedUi();
    void requestParentDirectory();
    void showFileContextMenu(const QPoint& globalPosition);
    void createRemoteDirectory();
    void renameSelectedEntry();
    void moveSelectedEntries();
    void copySelectedEntries();
    void moveSelectedToOtherPane();
    void copySelectedToOtherPane();
    void copySelectionToClipboard();
    void cutSelectionToClipboard();
    void pasteClipboard();
    void cancelPendingCut();
    void selectAllInActivePane();
    void focusActiveLocation();
    Q_INVOKABLE void handleInternalDrop(rfm::core::InternalTransferPayload payload,
                                        quint64 destinationPaneId,
                                        QString destinationDirectory);
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
    void updateTrackedOperation(rfm::core::OperationProgress operation);
    void loadOperationHistory();
    void scheduleOperationHistorySave();
    void saveOperationHistory();
    void removeTerminalOperation(quint64 id);
    void clearTerminalOperations();
    void updateOperationActions();
    void updateConnectionAction();
    void updatePaneTransferContexts();
    void updateCutAppearance();
    void clearInternalClipboard();
    [[nodiscard]] rfm::core::RemoteConnectionIdentity currentConnectionIdentity() const;
    [[nodiscard]] rfm::core::InternalTransferPayload transferPayload(
        quint64 paneId, const QList<rfm::core::RemoteSelection>& sources) const;
    bool startRemoteTransfer(
        rfm::core::InternalTransferAction action,
        const rfm::core::InternalTransferPayload& payload, quint64 destinationPaneId,
        const QString& destinationDirectory, bool clipboardMove = false);
    [[nodiscard]] QString transferValidationMessage(
        rfm::core::InternalTransferValidationError error) const;
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
    QAction* m_disconnectAction{nullptr};
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
    QAction* m_clipboardCopyAction{nullptr};
    QAction* m_clipboardCutAction{nullptr};
    QAction* m_clipboardPasteAction{nullptr};
    QAction* m_selectAllAction{nullptr};
    QAction* m_focusLocationAction{nullptr};
    QAction* m_switchPaneAction{nullptr};
    QAction* m_cancelCutAction{nullptr};
    PaneWorkspace* m_paneWorkspace{nullptr};
    HomePage* m_homePage{nullptr};
    QStackedWidget* m_centralStack{nullptr};
    OperationPanel* m_operationPanel{nullptr};
    QListWidget* m_serverProfileList{nullptr};
    QLabel* m_serverProfileErrorLabel{nullptr};
    QPushButton* m_connectServerProfileButton{nullptr};
    QTimer* m_autoRefreshTimer{nullptr};
    QTimer* m_refreshDebounceTimer{nullptr};
    QTimer* m_historySaveTimer{nullptr};
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
    QHash<quint64, rfm::core::OperationProgress> m_operations;
    QHash<quint64, quint64> m_transferPanes;
    rfm::core::InternalClipboard m_internalClipboard;
    QSet<quint64> m_clipboardMoveOperations;
    std::unique_ptr<rfm::core::OperationHistoryStore> m_operationHistoryStore;
    std::unique_ptr<rfm::core::ServerProfileStore> m_serverProfileStore;
    QList<rfm::core::ConnectionProfile> m_serverProfiles;
    QPointer<ConnectionDialog> m_connectionDialog;
    QString m_applicationInstanceId;
    quint64 m_activeDirectoryRequestId{0};
    quint64 m_nextOperationId{1};
    quint64 m_connectionGeneration{0};
    bool m_connected{false};
    bool m_busy{false};
};

} // namespace rfm::app
