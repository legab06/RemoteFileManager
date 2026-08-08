#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/TransferTypes.hpp"

#include <QMainWindow>
#include <QPoint>
#include <QSet>
#include <QStringList>

class QAction;
class QLineEdit;
class QTableWidget;
class QThread;
class QTimer;

namespace rfm::ssh
{
class SshSession;
}

namespace rfm::app
{

class TransferPanel;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

  signals:
    void connectionRequested(rfm::core::ConnectionProfile profile, QString password);
    void hostKeyDecision(bool accepted);
    void directoryRequested(QString path);
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
    void createNavigationBar();
    void createPlacesDock();
    void createTransferDock();
    void createEmptyState();
    void showConnectionDialog();
    void showAboutDialog();
    void showHostKeyConfirmation(const QString& host, const QString& fingerprint);
    Q_INVOKABLE void showRemoteDirectory(const QString& path,
                                         const QList<rfm::core::RemoteEntry>& entries);
    void showConnectionError(const QString& message);
    void openSelectedEntry(int row, int column);
    void requestParentDirectory();
    void showFileContextMenu(const QPoint& position);
    void createRemoteDirectory();
    void renameSelectedEntry();
    void moveSelectedEntries();
    void copySelectedEntries();
    void removeSelectedEntries();
    void chooseUploads();
    void chooseDownloadDirectory();
    Q_INVOKABLE void queueUploads(QStringList localPaths);
    Q_INVOKABLE void queueDownloads(QString localDirectory);
    void handleOperationResult(const rfm::core::RemoteOperationResult& result);
    Q_INVOKABLE void handleTransferProgress(const rfm::core::TransferProgress& progress);
    void updateOperationActions();
    void updateConnectionAction();
    void requestDirectoryListing(const QString& path, bool showBusy, bool deferIfActive);
    void scheduleCurrentDirectoryRefresh(bool showBusy);
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
    QAction* m_removeAction{nullptr};
    QAction* m_uploadAction{nullptr};
    QAction* m_downloadAction{nullptr};
    QLineEdit* m_remotePathEdit{nullptr};
    QTableWidget* m_fileTable{nullptr};
    TransferPanel* m_transferPanel{nullptr};
    QTimer* m_autoRefreshTimer{nullptr};
    QTimer* m_refreshDebounceTimer{nullptr};
    QThread* m_sshThread{nullptr};
    rfm::ssh::SshSession* m_sshSession{nullptr};
    rfm::core::ConnectionProfile m_activeProfile;
    QString m_currentPath;
    QStringList m_pendingSelectionNames;
    QSet<quint64> m_pendingTransferRequests;
    QSet<quint64> m_nonTerminalTransfers;
    QString m_deferredDirectoryPath;
    quint64 m_nextOperationId{1};
    bool m_connected{false};
    bool m_busy{false};
    bool m_listingInProgress{false};
    bool m_deferredDirectoryBusy{false};
    bool m_scheduledRefreshBusy{false};
};

} // namespace rfm::app
