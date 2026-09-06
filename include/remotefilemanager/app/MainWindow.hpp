#pragma once

#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/core/BrowserLocation.hpp"
#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/InternalTransfer.hpp"
#include "remotefilemanager/core/LocalFileSystem.hpp"
#include "remotefilemanager/core/OperationProgress.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/RemoteFilesystem.hpp"
#include "remotefilemanager/core/Storage.hpp"
#include "remotefilemanager/core/TransferTypes.hpp"
#include "remotefilemanager/core/VolumeService.hpp"
#include "remotefilemanager/ssh/SshAuthenticationPolicy.hpp"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QStringList>

#include <memory>
#include <optional>

class QAction;
class QLabel;
class QPoint;
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
class LocalFileSystemWorker;
class LocalFileOperationWorker;
class TransferCoordinator;
} // namespace rfm::core

namespace rfm::app
{

class PaneWorkspace;
class OperationPanel;
class ConnectionDialog;
class PasswordAuthenticationDialog;
class HomePage;
class NavigationTree;
class VolumeAuthenticationDialog;
struct RemoteMachineDescriptor;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr, QString operationHistoryDirectory = {},
                        QString serverProfileDirectory = {},
                        std::unique_ptr<rfm::core::VolumeService> volumeService = {});
    ~MainWindow() override;

  signals:
    void connectionRequested(rfm::core::ConnectionProfile profile);
    void passwordAuthenticationCancelled();
    void hostKeyDecision(bool accepted);
    void directoryRequested(quint64 requestId, QString path);
    void localDirectoryRequested(quint64 requestId, QString path);
    void remoteDirectoryCountRequested(quint64 requestId, QString path);
    void localDirectoryCountRequested(quint64 requestId, QString path);
    void localFileOperationRequested(rfm::core::LocalFileOperationRequest request);
    void localVolumesRequested();
    void localStorageProbeRequested(quint64 requestId);
    void volumeOperationRequested(rfm::core::VolumeOperationRequest request);
    void remoteVolumeOperationRequested(rfm::core::VolumeOperationRequest request);
    void remoteVolumeAuthenticationCancelled(quint64 operationId, quint64 authenticationToken);
    void remoteStorageRequested(quint64 requestId);
    void remoteStorageProbeRequested(quint64 requestId);
    void remoteFilesystemRelationRequested(quint64 requestId, QString sourceDirectory,
                                           QString destinationDirectory);
    void createDirectoryRequested(quint64 id, QString parent, QString name);
    void renameRequested(quint64 id, QString source, QString newName);
    void moveRequested(quint64 id, QList<rfm::core::RemoteSelection> sources,
                       QString destinationDirectory);
    void copyRequested(quint64 id, QList<rfm::core::RemoteSelection> sources,
                       QString destinationDirectory);
    void remoteOperationRequested(rfm::core::RemoteOperationRequest request);
    void removeRequested(quint64 id, QList<rfm::core::RemoteSelection> sources, bool recursive);
    void transferRequested(rfm::core::TransferRequest request);
    void pauseTransferRequested(quint64 id);
    void resumeTransferRequested(quint64 id);
    void cancelTransferRequested(quint64 id);
    void cancelRemoteOperationRequested(quint64 id);
    void shutdownRequested();
    void disconnectionRequested();

  private:
    void createActions();
    void createMenus();
    void createPaneWorkspace();
    void connectBrowserPane(quint64 paneId);
    void openLocalFile(const rfm::core::BrowserLocation& location);
    void createNavigationBar();
    void createPlacesDock();
    void createOperationDock();
    void createCentralPages();
    void showConnectionDialog();
    void beginConnection(const rfm::core::ConnectionProfile& profile);
    Q_INVOKABLE void showPasswordAuthentication();
    Q_INVOKABLE void
    showPasswordAuthenticationForReason(rfm::ssh::PasswordAuthenticationReason reason);
    Q_INVOKABLE void showPasswordAuthenticationError(const QString& message);
    void submitPasswordAuthentication();
    void cancelPasswordAuthentication();
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
    void requestDirectoryItemCount(quint64 paneId, const rfm::core::BrowserLocation& location,
                                   quint64 generation, const QString& name);
    Q_INVOKABLE void handleDirectoryCounted(quint64 requestId, const QString& path, quint64 count);
    Q_INVOKABLE void handleDirectoryCountFailed(quint64 requestId, const QString& path);
    Q_INVOKABLE void showConnectionError(const QString& message);
    void loadServerProfiles();
    void refreshServerProfileViews();
    void editSelectedServerProfile();
    void editServerProfile(const QString& id);
    void removeSelectedServerProfile();
    void connectToSelectedServerProfile();
    void connectToServerProfile(const QString& id);
    void showPlacesContextMenu(const QPoint& position);
    void showSelectedPlaceProperties();
    void showContextEntryProperties();
    void showPropertiesDialog(const QString& title, const QString& text);
    void requestDisconnection();
    [[nodiscard]] rfm::core::ConnectionProfile selectedServerProfile() const;
    Q_INVOKABLE void handleDisconnected();
    void resetDisconnectedUi();
    void requestParentDirectory();
    void showFileContextMenu(const QPoint& globalPosition);
    void createDirectory();
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
                                        rfm::core::InternalTransferAction action,
                                        quint64 destinationPaneId, QString destinationDirectory,
                                        bool actionWasExplicitlyRequested);
    Q_INVOKABLE void handleRemoteFilesystemRelation(quint64 requestId,
                                                    rfm::core::RemoteFilesystemRelation relation);
    void removeSelectedEntries();
    Q_INVOKABLE void handleOperationResult(const rfm::core::RemoteOperationResult& result);
    Q_INVOKABLE void handleRemoteOperationProgress(rfm::core::OperationProgress progress);
    Q_INVOKABLE void handleTransferProgress(const rfm::core::TransferProgress& progress);
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
    bool startLocalOperation(
        rfm::core::LocalFileOperationKind kind, quint64 sourcePaneId, quint64 destinationPaneId,
        const QString& sourceDirectory, const QString& destinationDirectory,
        const QList<rfm::core::RemoteSelection>& sources,
        rfm::core::LocalCollisionPolicy collisionPolicy = rfm::core::LocalCollisionPolicy::Fail,
        std::optional<quint64> clipboardGeneration = std::nullopt);
    [[nodiscard]] rfm::core::RemoteConnectionIdentity currentConnectionIdentity() const;
    [[nodiscard]] rfm::core::InternalTransferPayload
    transferPayload(quint64 paneId, const QList<rfm::core::RemoteSelection>& sources) const;
    bool startRemoteTransfer(rfm::core::InternalTransferAction action,
                             const rfm::core::InternalTransferPayload& payload,
                             quint64 destinationPaneId, const QString& destinationDirectory,
                             bool clipboardMove = false);
    bool startCrossSourceTransfer(rfm::core::InternalTransferAction action,
                                  const rfm::core::InternalTransferPayload& payload,
                                  quint64 destinationPaneId, const QString& destinationDirectory);
    void queueTransferRequest(const rfm::core::TransferRequest& request, quint64 paneId);
    void startRemoteFilesystemPreflight(rfm::core::InternalTransferPayload payload,
                                        quint64 destinationPaneId, QString destinationDirectory);
    [[nodiscard]] std::optional<rfm::core::InternalTransferAction>
    chooseCrossFilesystemTransferAction();
    void scheduleTransferDestinationRefresh(quint64 transferId);
    [[nodiscard]] QString
    transferValidationMessage(rfm::core::InternalTransferValidationError error) const;
    [[nodiscard]] QString listingStatusMessage(const QString& path,
                                               PaneNavigation navigation) const;
    void requestDirectoryListing(quint64 paneId, const QString& path, bool showBusy,
                                 bool coalesceIfPending,
                                 PaneNavigation navigation = PaneNavigation::Refresh,
                                 bool showStatusMessage = true);
    void requestLocalDirectoryListing(quint64 paneId, const QString& path, bool showBusy,
                                      PaneNavigation navigation = PaneNavigation::Refresh,
                                      bool treeRequest = false, bool showStatusMessage = true);
    void requestLocalTreeDirectoryRefresh(const QString& path);
    void requestRemoteTreeDirectory(const QString& profileId, const QString& path);
    void requestLocationListing(quint64 paneId, const rfm::core::BrowserLocation& location,
                                bool showBusy, PaneNavigation navigation,
                                bool showStatusMessage = true);
    Q_INVOKABLE void handleLocalDirectoryListed(quint64 requestId, const QString& path,
                                                const QList<rfm::core::RemoteEntry>& entries);
    Q_INVOKABLE void handleLocalDirectoryListingError(quint64 requestId, const QString& path,
                                                      const QString& error);
    Q_INVOKABLE void
    handleLocalFileOperationResult(const rfm::core::LocalFileOperationResult& result);
    Q_INVOKABLE void handleLocalFileOperationStarted(quint64 id);
    void openLocalLocation(const QString& path);
    void beginVolumeOperation(const rfm::core::StorageVolume& volume,
                              rfm::core::VolumeOperation operation);
    Q_INVOKABLE void handleVolumeOperationResult(const rfm::core::VolumeOperationResult& result);
    void beginRemoteVolumeOperation(const QString& machineId,
                                    const rfm::core::StorageVolume& volume,
                                    rfm::core::VolumeOperation operation);
    Q_INVOKABLE void
    handleRemoteVolumeOperationResult(const rfm::core::VolumeOperationResult& result);
    void showRemoteVolumeAuthentication(const rfm::core::VolumeOperationResult& result);
    void evacuateLocalPanesFromMountPoint(const QString& mountPoint);
    void evacuateRemotePanesFromMountPoint(const QString& machineId, const QString& mountPoint);
    [[nodiscard]] QString
    volumeOperationErrorMessage(const rfm::core::VolumeOperationResult& result) const;
    void openRemoteTreeLocation(const QString& profileId, const QString& path);
    void refreshStorage();
    Q_INVOKABLE void probeStorage();
    void probeLocalStorage();
    void probeRemoteStorage();
    Q_INVOKABLE void handleLocalStorageVolumes(const QList<rfm::core::StorageVolume>& volumes);
    Q_INVOKABLE void handleLocalStorageProbe(quint64 requestId, const QByteArray& fingerprint);
    Q_INVOKABLE void handleRemoteStorageVolumes(quint64 requestId,
                                                const QList<rfm::core::StorageVolume>& volumes);
    Q_INVOKABLE void handleRemoteStorageError(quint64 requestId, const QString& error);
    Q_INVOKABLE void handleRemoteStorageFingerprint(quint64 requestId,
                                                    const QByteArray& fingerprint);
    Q_INVOKABLE void handleRemoteStorageProbe(quint64 requestId, const QByteArray& fingerprint);
    Q_INVOKABLE void handleRemoteStorageProbeError(quint64 requestId, const QString& error);
    void updateNavigationActions();
    [[nodiscard]] QString activeRemoteMachineId() const;
    [[nodiscard]] RemoteMachineDescriptor activeRemoteMachine() const;
    void startNextDirectoryListing();
    [[nodiscard]] quint64 beginPaneNavigation(quint64 paneId, rfm::core::FileSource source);
    [[nodiscard]] bool isExpectedPaneNavigation(quint64 paneId, quint64 navigationGeneration,
                                                rfm::core::FileSource source,
                                                quint64 connectionGeneration = 0) const;
    void cancelDirectoryRequests(quint64 paneId);
    void setPaneBusy(quint64 paneId, bool busy, const QString& message = {},
                     int messageTimeout = 0);
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
    QAction* m_filePropertiesAction{nullptr};
    QAction* m_splitViewAction{nullptr};
    QAction* m_resetFileViewAction{nullptr};
    QAction* m_showHiddenFilesAction{nullptr};
    QAction* m_placesDockAction{nullptr};
    QAction* m_operationDockAction{nullptr};
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
    NavigationTree* m_navigationTree{nullptr};
    QLabel* m_serverProfileErrorLabel{nullptr};
    QTimer* m_autoRefreshTimer{nullptr};
    QTimer* m_refreshDebounceTimer{nullptr};
    QTimer* m_historySaveTimer{nullptr};
    QThread* m_sshThread{nullptr};
    QThread* m_localThread{nullptr};
    QThread* m_localOperationThread{nullptr};
    QThread* m_volumeThread{nullptr};
    rfm::ssh::SshSession* m_sshSession{nullptr};
    rfm::core::TransferCoordinator* m_transferCoordinator{nullptr};
    rfm::core::LocalFileSystemWorker* m_localFileSystem{nullptr};
    rfm::core::LocalFileOperationWorker* m_localFileOperationWorker{nullptr};
    rfm::core::VolumeOperationWorker* m_volumeOperationWorker{nullptr};
    rfm::core::ConnectionProfile m_activeProfile;
    QString m_activeSavedProfileId;
    QSet<quint64> m_pendingTransferRequests;
    QSet<quint64> m_nonTerminalTransfers;
    struct DirectoryRequest {
        quint64 id{0};
        quint64 paneId{0};
        QString path;
        PaneNavigation navigation{PaneNavigation::Refresh};
        bool treeRequest{false};
        QString profileId;
        quint64 connectionGeneration{0};
        quint64 navigationGeneration{0};
    };
    QHash<quint64, DirectoryRequest> m_directoryRequests;
    QQueue<quint64> m_directoryQueue;
    QHash<quint64, quint64> m_expectedDirectoryRequests;
    struct DirectoryCountRequest {
        quint64 paneId{0};
        rfm::core::BrowserLocation location;
        quint64 generation{0};
        QString name;
        QString path;
    };
    QHash<quint64, DirectoryCountRequest> m_directoryCountRequests;
    struct PendingRemoteFilesystemPreflight {
        rfm::core::InternalTransferPayload payload;
        quint64 destinationPaneId{0};
        QString destinationDirectory;
        rfm::core::RemoteConnectionIdentity connection;
    };
    QHash<quint64, PendingRemoteFilesystemPreflight> m_pendingRemoteFilesystemPreflights;
    QSet<quint64> m_busyPanes;
    struct LocalDirectoryRequest {
        quint64 paneId{0};
        PaneNavigation navigation{PaneNavigation::Refresh};
        bool treeRequest{false};
        quint64 navigationGeneration{0};
        QString path;
    };
    QHash<quint64, LocalDirectoryRequest> m_localDirectoryRequests;
    QHash<quint64, quint64> m_expectedLocalDirectoryRequests;
    QHash<quint64, quint64> m_paneNavigationGenerations;
    QHash<quint64, rfm::core::FileSource> m_expectedPaneSources;
    QHash<quint64, bool> m_scheduledPaneRefreshes;
    QHash<quint64, rfm::core::BrowserLocation> m_scheduledTransferRefreshLocations;
    struct OperationContext {
        quint64 sourcePaneId{0};
        quint64 destinationPaneId{0};
        QString sourceDirectory;
        QString destinationDirectory;
    };
    QHash<quint64, OperationContext> m_operationContexts;
    QHash<quint64, OperationContext> m_localOperationContexts;
    QHash<quint64, rfm::core::LocalFileOperationRequest> m_localOperationRequests;
    QHash<quint64, QList<rfm::core::LocalFileOperationItemResult>> m_localOperationItems;
    QHash<quint64, rfm::core::VolumeOperationRequest> m_volumeOperations;
    QSet<quint64> m_volumeOperationsAwaitingRefresh;
    struct RemoteVolumeOperationContext {
        rfm::core::VolumeOperationRequest request;
        QString machineId;
        quint64 connectionGeneration{0};
        quint64 authenticationToken{0};
        QPointer<VolumeAuthenticationDialog> authenticationDialog;
    };
    QHash<quint64, RemoteVolumeOperationContext> m_remoteVolumeOperations;
    QSet<quint64> m_remoteVolumeOperationsAwaitingRefresh;
    QHash<quint64, rfm::core::OperationProgress> m_remoteOperations;
    QSet<quint64> m_silentRemoteOperationResults;
    QHash<quint64, rfm::core::OperationProgress> m_operations;
    QHash<quint64, quint64> m_transferPanes;
    struct TransferRefreshContext {
        rfm::core::BrowserLocation destination;
        rfm::core::RemoteConnectionIdentity connection;
    };
    QHash<quint64, TransferRefreshContext> m_transferRefreshContexts;
    rfm::core::InternalClipboard m_internalClipboard;
    QHash<quint64, quint64> m_clipboardMoveOperations;
    QHash<quint64, quint64> m_localClipboardMoveOperations;
    std::unique_ptr<rfm::core::OperationHistoryStore> m_operationHistoryStore;
    std::unique_ptr<rfm::core::ServerProfileStore> m_serverProfileStore;
    QList<rfm::core::ConnectionProfile> m_serverProfiles;
    QPointer<ConnectionDialog> m_connectionDialog;
    QPointer<PasswordAuthenticationDialog> m_passwordAuthenticationDialog;
    QString m_applicationInstanceId;
    QString m_activeRemoteMachineId;
    QString m_remoteInitialPath;
    quint64 m_activeDirectoryRequestId{0};
    quint64 m_nextOperationId{1};
    quint64 m_connectionGeneration{0};
    quint64 m_nextStorageRequestId{1};
    quint64 m_remoteStorageRequestId{0};
    quint64 m_remoteStorageRequestConnectionGeneration{0};
    quint64 m_localStorageProbeRequestId{0};
    quint64 m_remoteStorageProbeRequestId{0};
    quint64 m_remoteStorageProbeConnectionGeneration{0};
    QByteArray m_localStorageFingerprint;
    QByteArray m_remoteStorageFingerprint;
    QByteArray m_pendingRemoteStorageFingerprint;
    QList<rfm::core::StorageVolume> m_localStorageVolumes;
    QList<rfm::core::StorageVolume> m_remoteStorageVolumes;
    bool m_connected{false};
    bool m_connecting{false};
    bool m_connectionErrorNotificationActive{false};
    bool m_busy{false};
    bool m_localStorageRefreshPending{false};
    bool m_localStorageRefreshAfterCurrent{false};
    bool m_remoteStorageRefreshPending{false};
    bool m_remoteStorageRefreshAfterCurrent{false};
    bool m_localStorageProbePending{false};
    bool m_remoteStorageProbePending{false};
};

} // namespace rfm::app
