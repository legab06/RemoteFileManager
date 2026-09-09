#include "remotefilemanager/app/MainWindow.hpp"

#include "remotefilemanager/app/ConnectionDialog.hpp"
#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/app/HomePage.hpp"
#include "remotefilemanager/app/NavigationTree.hpp"
#include "remotefilemanager/app/OperationPanel.hpp"
#include "remotefilemanager/app/PasswordAuthenticationDialog.hpp"
#include "remotefilemanager/app/ServerProfileDialog.hpp"
#include "remotefilemanager/app/TransferRequestFactory.hpp"
#include "remotefilemanager/app/VolumeAuthenticationDialog.hpp"
#include "remotefilemanager/app/WorkspaceTabs.hpp"
#include "remotefilemanager/core/LocalFileSystem.hpp"
#include "remotefilemanager/core/OperationHistoryStore.hpp"
#include "remotefilemanager/core/RemotePath.hpp"
#include "remotefilemanager/core/ServerProfileStore.hpp"
#include "remotefilemanager/core/TransferCoordinator.hpp"
#include "remotefilemanager/ssh/LibsshRuntime.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <limits>
#include <utility>

namespace rfm::app
{
namespace
{

constexpr int ListingStatusMessageTimeoutMs = 3000;

QString remoteDisplayUrl(const rfm::core::ConnectionProfile& profile, const QString& path)
{
    const QString normalizedPath = rfm::core::RemotePath::normalize(path);
    QUrl url;
    url.setScheme(QStringLiteral("sftp"));
    url.setUserName(profile.username);
    url.setHost(profile.host);
    url.setPort(profile.port);
    if (normalizedPath.startsWith(QChar{'/'})) {
        url.setPath(normalizedPath);
    } else {
        url.setPath(normalizedPath == QStringLiteral(".") ? QStringLiteral("/~/")
                                                          : QStringLiteral("/~/") + normalizedPath);
    }
    return url.toDisplayString();
}

bool pathsUseSameConvention(const QString& first, const QString& second)
{
    return first.startsWith(QChar{'/'}) == second.startsWith(QChar{'/'});
}

bool profilesHaveSameConnectionSettings(const rfm::core::ConnectionProfile& first,
                                        const rfm::core::ConnectionProfile& second)
{
    return first.host.trimmed().compare(second.host.trimmed(), Qt::CaseInsensitive) == 0 &&
           first.username.trimmed() == second.username.trimmed() && first.port == second.port &&
           first.privateKeyPath.trimmed() == second.privateKeyPath.trimmed() &&
           first.allowPasswordAuthentication == second.allowPasswordAuthentication &&
           first.authenticationMode == second.authenticationMode;
}

QStringList knownMountPointsForDevice(const QList<rfm::core::StorageVolume>& volumes,
                                      const QString& device)
{
    const QString normalizedDevice = rfm::core::RemotePath::normalize(device.trimmed());
    QStringList mountPoints;
    for (const rfm::core::StorageVolume& volume : volumes) {
        if (rfm::core::RemotePath::normalize(volume.device.trimmed()) != normalizedDevice) {
            continue;
        }
        // Preserve every observation verbatim. Validation belongs to the service and an
        // inconsistent or duplicate sibling must make unmount less permissive, never safer.
        mountPoints.push_back(volume.mounted ? volume.rootPath : QString{});
    }
    return mountPoints;
}

} // namespace

MainWindow::MainWindow(QWidget* parent, QString operationHistoryDirectory,
                       QString serverProfileDirectory,
                       std::unique_ptr<rfm::core::VolumeService> volumeService)
    : QMainWindow(parent),
      m_operationHistoryStore(
          std::make_unique<rfm::core::OperationHistoryStore>(std::move(operationHistoryDirectory))),
      m_serverProfileStore(
          std::make_unique<rfm::core::ServerProfileStore>(std::move(serverProfileDirectory)))
{
    setObjectName(QStringLiteral("mainWindow"));
    m_applicationInstanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    setWindowTitle(tr("RemoteFileManager"));
    resize(1100, 700);
    setMinimumSize(760, 480);
    setUnifiedTitleAndToolBarOnMac(true);

    createWorkspaceTabs();
    createActions();
    createNavigationBar();
    createCentralPages();
    createPlacesDock();
    createOperationDock();
    createMenus();

    m_historySaveTimer = new QTimer(this);
    m_historySaveTimer->setObjectName(QStringLiteral("operationHistorySaveTimer"));
    m_historySaveTimer->setInterval(300);
    m_historySaveTimer->setSingleShot(true);
    connect(m_historySaveTimer, &QTimer::timeout, this, &MainWindow::saveOperationHistory);
    loadOperationHistory();

    m_autoRefreshTimer = new QTimer(this);
    m_autoRefreshTimer->setObjectName(QStringLiteral("autoRefreshTimer"));
    m_autoRefreshTimer->setInterval(3000);
    connect(m_autoRefreshTimer, &QTimer::timeout, this, [this] {
        probeStorage();
        for (const quint64 paneId : m_workspaceTabs->openPaneIds()) {
            FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
            if (pane->source() == rfm::core::FileSource::Ssh &&
                m_expectedPaneSources.value(paneId, pane->source()) == rfm::core::FileSource::Ssh) {
                requestDirectoryListing(paneId, pane->currentPath(), false, false);
            }
        }
    });
    m_refreshDebounceTimer = new QTimer(this);
    m_refreshDebounceTimer->setObjectName(QStringLiteral("refreshDebounceTimer"));
    m_refreshDebounceTimer->setInterval(150);
    m_refreshDebounceTimer->setSingleShot(true);
    connect(m_refreshDebounceTimer, &QTimer::timeout, this, [this] {
        const auto refreshes = std::exchange(m_scheduledPaneRefreshes, {});
        const auto transferDestinations = std::exchange(m_scheduledTransferRefreshLocations, {});
        for (auto iterator = refreshes.cbegin(); iterator != refreshes.cend(); ++iterator) {
            if (FileBrowserPane* const pane = m_workspaceTabs->pane(iterator.key());
                pane != nullptr && !pane->isHidden()) {
                const auto destination = transferDestinations.constFind(iterator.key());
                if (destination != transferDestinations.cend()) {
                    const bool stillDisplaysDestination =
                        destination->source == rfm::core::FileSource::Ssh
                            ? pane->source() == rfm::core::FileSource::Ssh &&
                                  pane->currentLocation().machineId == destination->machineId &&
                                  rfm::core::RemotePath::normalize(pane->currentPath()) ==
                                      destination->path
                            : pane->source() == rfm::core::FileSource::Local &&
                                  QDir::cleanPath(pane->currentPath()) == destination->path;
                    if (!stillDisplaysDestination) {
                        continue;
                    }
                }
                requestLocationListing(iterator.key(), pane->currentLocation(), iterator.value(),
                                       PaneNavigation::Refresh, false);
            }
        }
    });

    qRegisterMetaType<rfm::core::ConnectionProfile>();
    qRegisterMetaType<rfm::ssh::PasswordAuthenticationReason>();
    qRegisterMetaType<QList<rfm::core::RemoteEntry>>();
    qRegisterMetaType<QList<rfm::core::RemoteSelection>>();
    qRegisterMetaType<rfm::core::InternalTransferPayload>();
    qRegisterMetaType<rfm::core::InternalTransferAction>();
    qRegisterMetaType<rfm::core::RemoteFilesystemRelation>();
    qRegisterMetaType<rfm::core::RemoteOperationResult>();
    qRegisterMetaType<rfm::core::OperationProgress>();
    qRegisterMetaType<rfm::core::RemoteOperationRequest>();
    qRegisterMetaType<rfm::core::TransferRequest>();
    qRegisterMetaType<rfm::core::TransferProgress>();
    qRegisterMetaType<rfm::core::BrowserLocation>();
    qRegisterMetaType<rfm::core::LocalFileOperationRequest>();
    qRegisterMetaType<rfm::core::LocalFileOperationResult>();
    qRegisterMetaType<QList<rfm::core::StorageVolume>>();
    qRegisterMetaType<rfm::core::VolumeOperationRequest>();
    qRegisterMetaType<rfm::core::VolumeOperationResult>();

    m_localThread = new QThread(this);
    m_localFileSystem = new rfm::core::LocalFileSystemWorker;
    m_localFileSystem->moveToThread(m_localThread);
    connect(m_localThread, &QThread::finished, m_localFileSystem, &QObject::deleteLater);
    connect(this, &MainWindow::localDirectoryRequested, m_localFileSystem,
            &rfm::core::LocalFileSystemWorker::listDirectory);
    connect(this, &MainWindow::localDirectoryCountRequested, m_localFileSystem,
            &rfm::core::LocalFileSystemWorker::countDirectoryEntries);
    connect(this, &MainWindow::localVolumesRequested, m_localFileSystem,
            &rfm::core::LocalFileSystemWorker::listVolumes);
    connect(this, &MainWindow::localStorageProbeRequested, m_localFileSystem,
            &rfm::core::LocalFileSystemWorker::probeVolumes);
    connect(m_localFileSystem, &rfm::core::LocalFileSystemWorker::directoryListed, this,
            &MainWindow::handleLocalDirectoryListed);
    connect(m_localFileSystem, &rfm::core::LocalFileSystemWorker::directoryListingFailed, this,
            &MainWindow::handleLocalDirectoryListingError);
    connect(m_localFileSystem, &rfm::core::LocalFileSystemWorker::directoryCounted, this,
            &MainWindow::handleDirectoryCounted);
    connect(m_localFileSystem, &rfm::core::LocalFileSystemWorker::directoryCountFailed, this,
            &MainWindow::handleDirectoryCountFailed);
    connect(m_localFileSystem, &rfm::core::LocalFileSystemWorker::volumesListed, this,
            [this](const QList<rfm::core::StorageVolume>& volumes, const QByteArray& fingerprint) {
                m_localStorageFingerprint = fingerprint;
                handleLocalStorageVolumes(volumes);
            });
    connect(m_localFileSystem, &rfm::core::LocalFileSystemWorker::volumesProbed, this,
            &MainWindow::handleLocalStorageProbe);
    m_localThread->start();

    m_localOperationThread = new QThread(this);
    m_localFileOperationWorker = new rfm::core::LocalFileOperationWorker;
    m_localFileOperationWorker->moveToThread(m_localOperationThread);
    connect(m_localOperationThread, &QThread::finished, m_localFileOperationWorker,
            &QObject::deleteLater);
    connect(this, &MainWindow::localFileOperationRequested, m_localFileOperationWorker,
            &rfm::core::LocalFileOperationWorker::execute);
    connect(m_localFileOperationWorker, &rfm::core::LocalFileOperationWorker::started, this,
            &MainWindow::handleLocalFileOperationStarted);
    connect(m_localFileOperationWorker, &rfm::core::LocalFileOperationWorker::finished, this,
            &MainWindow::handleLocalFileOperationResult);
    m_localOperationThread->start();

    if (volumeService == nullptr) {
        volumeService = std::make_unique<rfm::core::LocalLinuxVolumeService>();
    }
    m_volumeThread = new QThread(this);
    m_volumeOperationWorker = new rfm::core::VolumeOperationWorker(std::move(volumeService));
    m_volumeOperationWorker->moveToThread(m_volumeThread);
    connect(m_volumeThread, &QThread::finished, m_volumeOperationWorker, &QObject::deleteLater);
    connect(this, &MainWindow::volumeOperationRequested, m_volumeOperationWorker,
            &rfm::core::VolumeOperationWorker::execute);
    connect(m_volumeOperationWorker, &rfm::core::VolumeOperationWorker::finished, this,
            &MainWindow::handleVolumeOperationResult);
    m_volumeThread->start();

    m_sshThread = new QThread(this);
    m_sshSession = new rfm::ssh::SshSession;
    m_transferCoordinator = new rfm::core::TransferCoordinator(this);
    m_sshSession->moveToThread(m_sshThread);
    connect(m_sshThread, &QThread::finished, m_sshSession, &QObject::deleteLater);
    connect(this, &MainWindow::connectionRequested, m_sshSession,
            &rfm::ssh::SshSession::connectToHost);
    connect(this, &MainWindow::hostKeyDecision, m_sshSession,
            &rfm::ssh::SshSession::confirmUnknownHost);
    connect(this, &MainWindow::passwordAuthenticationCancelled, m_sshSession,
            &rfm::ssh::SshSession::cancelPasswordAuthentication);
    connect(this, &MainWindow::directoryRequested, m_sshSession,
            &rfm::ssh::SshSession::listDirectory);
    connect(this, &MainWindow::remoteDirectoryCountRequested, m_sshSession,
            &rfm::ssh::SshSession::countDirectoryEntries);
    connect(this, &MainWindow::remoteStorageRequested, m_sshSession,
            &rfm::ssh::SshSession::listStorageVolumes);
    connect(this, &MainWindow::remoteStorageProbeRequested, m_sshSession,
            &rfm::ssh::SshSession::probeStorageMounts);
    connect(this, &MainWindow::remoteFilesystemRelationRequested, m_sshSession,
            &rfm::ssh::SshSession::compareRemoteFilesystems);
    connect(this, &MainWindow::remoteVolumeOperationRequested, m_sshSession,
            &rfm::ssh::SshSession::operateVolume);
    connect(this, &MainWindow::remoteVolumeAuthenticationCancelled, m_sshSession,
            &rfm::ssh::SshSession::cancelVolumeAuthentication);
    connect(this, &MainWindow::createDirectoryRequested, m_sshSession,
            &rfm::ssh::SshSession::createDirectory);
    connect(this, &MainWindow::renameRequested, m_sshSession, &rfm::ssh::SshSession::renameEntry);
    connect(this, &MainWindow::removeRequested, m_sshSession, &rfm::ssh::SshSession::removeEntries);
    connect(this, &MainWindow::transferRequested, m_transferCoordinator,
            &rfm::core::TransferCoordinator::enqueueTransfer);
    connect(this, &MainWindow::pauseTransferRequested, m_transferCoordinator,
            &rfm::core::TransferCoordinator::pauseTransfer);
    connect(this, &MainWindow::resumeTransferRequested, m_transferCoordinator,
            &rfm::core::TransferCoordinator::resumeTransfer);
    connect(this, &MainWindow::cancelTransferRequested, m_transferCoordinator,
            &rfm::core::TransferCoordinator::cancelTransfer);
    connect(this, &MainWindow::remoteOperationRequested, m_transferCoordinator,
            &rfm::core::TransferCoordinator::enqueueRemoteOperation);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::startTransferRequested,
            m_sshSession, &rfm::ssh::SshSession::startTransfer);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::pauseActiveRequested,
            m_sshSession, &rfm::ssh::SshSession::pauseTransfer);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::resumeActiveRequested,
            m_sshSession, &rfm::ssh::SshSession::resumeTransfer);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::cancelActiveRequested,
            m_sshSession, &rfm::ssh::SshSession::cancelTransfer);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::startRemoteOperationRequested,
            m_sshSession, &rfm::ssh::SshSession::startRemoteOperation);
    connect(this, &MainWindow::cancelRemoteOperationRequested, m_transferCoordinator,
            &rfm::core::TransferCoordinator::cancelRemoteOperation);
    connect(m_transferCoordinator,
            &rfm::core::TransferCoordinator::cancelActiveRemoteOperationRequested, m_sshSession,
            &rfm::ssh::SshSession::cancelRemoteOperation);
    connect(this, &MainWindow::shutdownRequested, m_transferCoordinator,
            &rfm::core::TransferCoordinator::shutdownTransfers);
    connect(this, &MainWindow::disconnectionRequested, m_transferCoordinator,
            &rfm::core::TransferCoordinator::disconnectExecutor);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::shutdownExecutorRequested,
            m_sshSession, &rfm::ssh::SshSession::shutdownTransfers);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::disconnectExecutorRequested,
            m_sshSession, &rfm::ssh::SshSession::disconnectFromHost);
    connect(m_sshSession, &rfm::ssh::SshSession::hostKeyConfirmationRequired, this,
            &MainWindow::showHostKeyConfirmation);
    connect(m_sshSession, &rfm::ssh::SshSession::passwordAuthenticationRequired, this,
            &MainWindow::showPasswordAuthenticationForReason);
    connect(m_sshSession, &rfm::ssh::SshSession::passwordAuthenticationRejected, this,
            &MainWindow::showPasswordAuthenticationError);
    connect(m_sshSession, &rfm::ssh::SshSession::connected, this, &MainWindow::handleConnected);
    connect(m_sshSession, &rfm::ssh::SshSession::directoryListed, this,
            &MainWindow::handleDirectoryListed);
    connect(m_sshSession, &rfm::ssh::SshSession::directoryListingFailed, this,
            &MainWindow::handleDirectoryListingError);
    connect(m_sshSession, &rfm::ssh::SshSession::directoryCounted, this,
            &MainWindow::handleDirectoryCounted);
    connect(m_sshSession, &rfm::ssh::SshSession::directoryCountFailed, this,
            &MainWindow::handleDirectoryCountFailed);
    connect(m_sshSession, &rfm::ssh::SshSession::remoteFilesystemsCompared, this,
            &MainWindow::handleRemoteFilesystemRelation);
    connect(m_sshSession, &rfm::ssh::SshSession::storageVolumesListed, this,
            &MainWindow::handleRemoteStorageVolumes);
    connect(m_sshSession, &rfm::ssh::SshSession::storageVolumeListingFailed, this,
            &MainWindow::handleRemoteStorageError);
    connect(m_sshSession, &rfm::ssh::SshSession::storageMountInfoFingerprint, this,
            &MainWindow::handleRemoteStorageFingerprint);
    connect(m_sshSession, &rfm::ssh::SshSession::storageMountsProbed, this,
            &MainWindow::handleRemoteStorageProbe);
    connect(m_sshSession, &rfm::ssh::SshSession::storageMountProbeFailed, this,
            &MainWindow::handleRemoteStorageProbeError);
    connect(m_sshSession, &rfm::ssh::SshSession::volumeOperationFinished, this,
            &MainWindow::handleRemoteVolumeOperationResult);
    connect(m_sshSession, &rfm::ssh::SshSession::failed, this, &MainWindow::showConnectionError);
    connect(m_sshSession, &rfm::ssh::SshSession::operationFinished, this,
            &MainWindow::handleOperationResult);
    connect(m_sshSession, &rfm::ssh::SshSession::remoteOperationFinished, m_transferCoordinator,
            &rfm::core::TransferCoordinator::handleRemoteExecutorResult);
    connect(m_sshSession, &rfm::ssh::SshSession::remoteOperationUpdated, m_transferCoordinator,
            &rfm::core::TransferCoordinator::handleRemoteExecutorProgress);
    connect(m_sshSession, &rfm::ssh::SshSession::transferUpdated, m_transferCoordinator,
            &rfm::core::TransferCoordinator::handleExecutorProgress);
    connect(m_sshSession, &rfm::ssh::SshSession::transferExecutorFailed, m_transferCoordinator,
            &rfm::core::TransferCoordinator::handleExecutorFailure);
    connect(m_sshSession, &rfm::ssh::SshSession::remoteOperationExecutorFailed,
            m_transferCoordinator, &rfm::core::TransferCoordinator::handleRemoteExecutorFailure);
    connect(m_sshSession, &rfm::ssh::SshSession::transferRejected, m_transferCoordinator,
            &rfm::core::TransferCoordinator::handleExecutorRejection);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::transferUpdated, this,
            &MainWindow::handleTransferProgress);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::remoteOperationUpdated, this,
            &MainWindow::handleRemoteOperationProgress);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::remoteOperationResultSilent,
            this, [this](quint64 id) { m_silentRemoteOperationResults.insert(id); });
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::remoteOperationFinished, this,
            &MainWindow::handleOperationResult);
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::transferRejected, this,
            [this](quint64 id, const QString& error) {
                if (m_pendingTransferRequests.remove(id) > 0) {
                    m_nonTerminalTransfers.remove(id);
                }
                m_transferPanes.remove(id);
                m_transferRefreshContexts.remove(id);
                updateConnectionAction();
                statusBar()->showMessage(error, 8000);
            });
    connect(m_transferCoordinator, &rfm::core::TransferCoordinator::remoteOperationRejected, this,
            [this](quint64 id, const QString& error) {
                m_operationContexts.remove(id);
                m_remoteOperations.remove(id);
                m_silentRemoteOperationResults.remove(id);
                m_clipboardMoveOperations.remove(id);
                updateConnectionAction();
                statusBar()->showMessage(error, 8000);
            });
    connect(m_sshSession, &rfm::ssh::SshSession::transfersShutdown, m_transferCoordinator,
            &rfm::core::TransferCoordinator::handleExecutorShutdown);
    connect(m_sshSession, &rfm::ssh::SshSession::disconnected, this,
            &MainWindow::handleDisconnected);
    m_sshThread->start();

    refreshStorage();
    m_autoRefreshTimer->start();

    statusBar()->showMessage(
        tr("Disconnected · libssh %1").arg(rfm::ssh::LibsshRuntime::version()));
}

MainWindow::~MainWindow()
{
    if (m_autoRefreshTimer != nullptr) {
        m_autoRefreshTimer->stop();
    }
    stopAutomaticRefresh();
    if (m_volumeThread != nullptr && m_volumeThread->isRunning()) {
        QObject::disconnect(m_volumeOperationWorker, nullptr, this, nullptr);
        QObject::disconnect(this, nullptr, m_volumeOperationWorker, nullptr);
        m_volumeOperationWorker->requestCancellation();
        m_volumeThread->quit();
        m_volumeThread->wait();
    }
    if (m_sshThread != nullptr && m_sshThread->isRunning()) {
        QEventLoop shutdownLoop;
        connect(m_transferCoordinator, &rfm::core::TransferCoordinator::transfersShutdown,
                &shutdownLoop, &QEventLoop::quit, Qt::QueuedConnection);
        emit shutdownRequested();
        shutdownLoop.exec(QEventLoop::ExcludeUserInputEvents);
        m_sshThread->quit();
        m_sshThread->wait();
    }
    if (m_historySaveTimer->isActive()) {
        m_historySaveTimer->stop();
        saveOperationHistory();
    }
    if (m_localOperationThread != nullptr && m_localOperationThread->isRunning()) {
        for (auto iterator = m_localOperationRequests.cbegin();
             iterator != m_localOperationRequests.cend(); ++iterator) {
            if (iterator->kind == rfm::core::LocalFileOperationKind::Copy ||
                iterator->kind == rfm::core::LocalFileOperationKind::Move) {
                m_localFileOperationWorker->requestCancellation(iterator.key());
            }
        }
        m_localOperationThread->quit();
        m_localOperationThread->wait();
    }
    if (m_localThread != nullptr && m_localThread->isRunning()) {
        m_localThread->quit();
        m_localThread->wait();
    }
}

void MainWindow::createActions()
{
    m_newConnectionAction =
        new QAction(style()->standardIcon(QStyle::SP_ComputerIcon), tr("New connection…"), this);
    m_newConnectionAction->setObjectName(QStringLiteral("newConnectionAction"));
    m_newConnectionAction->setShortcut(QKeySequence::New);
    connect(m_newConnectionAction, &QAction::triggered, this, &MainWindow::showConnectionDialog);

    m_disconnectAction =
        new QAction(style()->standardIcon(QStyle::SP_DialogCloseButton), tr("Disconnect"), this);
    m_disconnectAction->setObjectName(QStringLiteral("disconnectAction"));
    m_disconnectAction->setEnabled(false);
    connect(m_disconnectAction, &QAction::triggered, this, &MainWindow::requestDisconnection);

    m_quitAction = new QAction(tr("Quit"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    connect(m_quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_aboutAction = new QAction(tr("About RemoteFileManager"), this);
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAboutDialog);

    m_createDirectoryAction = new QAction(tr("New folder…"), this);
    m_createDirectoryAction->setObjectName(QStringLiteral("createDirectoryAction"));
    connect(m_createDirectoryAction, &QAction::triggered, this, &MainWindow::createDirectory);
    m_renameAction = new QAction(tr("Rename…"), this);
    m_renameAction->setObjectName(QStringLiteral("renameAction"));
    m_renameAction->setShortcut(QKeySequence(Qt::Key_F2));
    connect(m_renameAction, &QAction::triggered, this, &MainWindow::renameSelectedEntry);
    m_moveAction = new QAction(tr("Move to…"), this);
    m_moveAction->setObjectName(QStringLiteral("moveAction"));
    connect(m_moveAction, &QAction::triggered, this, &MainWindow::moveSelectedEntries);
    m_copyAction = new QAction(tr("Copy to…"), this);
    m_copyAction->setObjectName(QStringLiteral("copyAction"));
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::copySelectedEntries);
    m_moveToOtherPaneAction = new QAction(tr("Move to other pane"), this);
    m_moveToOtherPaneAction->setObjectName(QStringLiteral("moveToOtherPaneAction"));
    connect(m_moveToOtherPaneAction, &QAction::triggered, this,
            &MainWindow::moveSelectedToOtherPane);
    m_copyToOtherPaneAction = new QAction(tr("Copy to other pane"), this);
    m_copyToOtherPaneAction->setObjectName(QStringLiteral("copyToOtherPaneAction"));
    connect(m_copyToOtherPaneAction, &QAction::triggered, this,
            &MainWindow::copySelectedToOtherPane);
    m_openAction = new QAction(tr("Open"), this);
    m_openAction->setObjectName(QStringLiteral("openAction"));
    connect(m_openAction, &QAction::triggered, this, &MainWindow::openContextEntry);
    m_removeAction = new QAction(tr("Delete…"), this);
    m_removeAction->setObjectName(QStringLiteral("removeAction"));
    m_removeAction->setShortcut(QKeySequence::Delete);
    connect(m_removeAction, &QAction::triggered, this, &MainWindow::removeSelectedEntries);
    m_filePropertiesAction = new QAction(tr("Properties"), this);
    m_filePropertiesAction->setObjectName(QStringLiteral("filePropertiesAction"));
    connect(m_filePropertiesAction, &QAction::triggered, this,
            &MainWindow::showContextEntryProperties);

    m_clipboardCopyAction = new QAction(tr("Copy"), this);
    m_clipboardCopyAction->setObjectName(QStringLiteral("clipboardCopyAction"));
    m_clipboardCopyAction->setShortcut(QKeySequence::Copy);
    connect(m_clipboardCopyAction, &QAction::triggered, this,
            &MainWindow::copySelectionToClipboard);
    m_clipboardCutAction = new QAction(tr("Cut"), this);
    m_clipboardCutAction->setObjectName(QStringLiteral("clipboardCutAction"));
    m_clipboardCutAction->setShortcut(QKeySequence::Cut);
    connect(m_clipboardCutAction, &QAction::triggered, this, &MainWindow::cutSelectionToClipboard);
    m_clipboardPasteAction = new QAction(tr("Paste"), this);
    m_clipboardPasteAction->setObjectName(QStringLiteral("clipboardPasteAction"));
    m_clipboardPasteAction->setShortcut(QKeySequence::Paste);
    connect(m_clipboardPasteAction, &QAction::triggered, this, &MainWindow::pasteClipboard);
    m_selectAllAction = new QAction(tr("Select all"), this);
    m_selectAllAction->setObjectName(QStringLiteral("selectAllAction"));
    m_selectAllAction->setShortcut(QKeySequence::SelectAll);
    connect(m_selectAllAction, &QAction::triggered, this, &MainWindow::selectAllInActivePane);
    m_focusLocationAction = new QAction(tr("Focus location"), this);
    m_focusLocationAction->setObjectName(QStringLiteral("focusLocationAction"));
    m_focusLocationAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    connect(m_focusLocationAction, &QAction::triggered, this, &MainWindow::focusActiveLocation);
    m_switchPaneAction = new QAction(tr("Switch pane"), this);
    m_switchPaneAction->setObjectName(QStringLiteral("switchPaneAction"));
    m_switchPaneAction->setShortcut(QKeySequence(Qt::Key_F6));
    connect(m_switchPaneAction, &QAction::triggered, this,
            [this] { m_workspaceTabs->activeWorkspace()->activateOtherPane(); });
    m_cancelCutAction = new QAction(this);
    m_cancelCutAction->setObjectName(QStringLiteral("cancelCutAction"));
    m_cancelCutAction->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(m_cancelCutAction, &QAction::triggered, this, &MainWindow::cancelPendingCut);
    addAction(m_cancelCutAction);

    m_splitViewAction = new QAction(tr("Split view"), this);
    m_splitViewAction->setObjectName(QStringLiteral("splitViewAction"));
    m_splitViewAction->setCheckable(true);
    connect(m_splitViewAction, &QAction::toggled, this,
            [this](bool enabled) { m_workspaceTabs->activeWorkspace()->setSplit(enabled); });
    m_resetFileViewAction = new QAction(tr("Reset file view"), this);
    m_resetFileViewAction->setObjectName(QStringLiteral("resetFileViewAction"));
    connect(m_resetFileViewAction, &QAction::triggered, this,
            [this] { m_workspaceTabs->activeWorkspace()->resetFileView(); });
    m_showHiddenFilesAction = new QAction(tr("Show hidden files"), this);
    m_showHiddenFilesAction->setObjectName(QStringLiteral("showHiddenFilesAction"));
    m_showHiddenFilesAction->setCheckable(true);
    m_showHiddenFilesAction->setChecked(
        QSettings{}.value(QStringLiteral("ui/showHiddenFiles"), false).toBool());
    connect(m_showHiddenFilesAction, &QAction::toggled, this, [this](bool show) {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/showHiddenFiles"), show);
        for (const quint64 paneId : m_workspaceTabs->paneIds()) {
            m_workspaceTabs->pane(paneId)->setShowHiddenFiles(show);
        }
        if (m_navigationTree != nullptr) {
            m_navigationTree->setShowHiddenFiles(show);
        }
    });
    updateOperationActions();
}

void MainWindow::createMenus()
{
    QMenu* const fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_newConnectionAction);
    fileMenu->addAction(m_disconnectAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_createDirectoryAction);
    fileMenu->addAction(m_renameAction);
    fileMenu->addAction(m_moveAction);
    fileMenu->addAction(m_copyAction);
    fileMenu->addAction(m_moveToOtherPaneAction);
    fileMenu->addAction(m_copyToOtherPaneAction);
    fileMenu->addAction(m_removeAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_quitAction);

    QMenu* const editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(m_clipboardCopyAction);
    editMenu->addAction(m_clipboardCutAction);
    editMenu->addAction(m_clipboardPasteAction);
    editMenu->addSeparator();
    editMenu->addAction(m_selectAllAction);

    QMenu* const viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_placesDockAction);
    viewMenu->addAction(m_operationDockAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_splitViewAction);
    viewMenu->addAction(m_showHiddenFilesAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_resetFileViewAction);
    viewMenu->addAction(m_focusLocationAction);
    viewMenu->addAction(m_switchPaneAction);

    QMenu* const helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(m_aboutAction);
}

void MainWindow::createWorkspaceTabs()
{
    m_workspaceTabs = new WorkspaceTabs(this);
    connect(m_workspaceTabs, &WorkspaceTabs::paneAdded, this, &MainWindow::connectBrowserPane);
    connect(m_workspaceTabs, &WorkspaceTabs::paneRemoved, this, &MainWindow::removePaneContexts);
    connectBrowserPane(m_workspaceTabs->paneId(m_workspaceTabs->activeWorkspace()->primaryPane()));
    connect(m_workspaceTabs, &WorkspaceTabs::activePaneChanged, this, [this](quint64) {
        const QSignalBlocker blocker(m_splitViewAction);
        m_splitViewAction->setChecked(m_workspaceTabs->activeWorkspace()->isSplit());
        updateOperationActions();
        updateNavigationActions();
    });
    connect(m_workspaceTabs, &WorkspaceTabs::paneVisibilityChanged, this,
            [this](quint64 paneId, bool visible) {
                const QSignalBlocker blocker(m_splitViewAction);
                m_splitViewAction->setChecked(m_workspaceTabs->activeWorkspace()->isSplit());
                if (!visible) {
                    cancelDirectoryRequests(paneId);
                    updateOperationActions();
                    return;
                }
                connectBrowserPane(paneId);
                FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
                if (pane->source() == rfm::core::FileSource::Local) {
                    pane->setTransferContext(m_applicationInstanceId, {}, paneId);
                } else if (m_connected && pane->source() == rfm::core::FileSource::Ssh) {
                    pane->setTransferContext(m_applicationInstanceId, currentConnectionIdentity(),
                                             paneId);
                }
                const FileBrowserPane* const activePane =
                    m_workspaceTabs->workspaceForPane(paneId)->activePane();
                if (!pane->hasLocation() && activePane->hasLocation()) {
                    requestLocationListing(paneId, activePane->currentLocation(), true,
                                           PaneNavigation::Initial);
                }
                updateOperationActions();
            });
}

void MainWindow::connectBrowserPane(quint64 paneId)
{
    FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
    if (pane == nullptr || pane->property("mainWindowConnected").toBool()) {
        return;
    }
    pane->setProperty("mainWindowConnected", true);
    if (m_showHiddenFilesAction != nullptr) {
        pane->setShowHiddenFiles(m_showHiddenFilesAction->isChecked());
    }
    connect(pane, &FileBrowserPane::locationNavigationRequested, this,
            [this, paneId](const rfm::core::BrowserLocation& location, PaneNavigation navigation) {
                requestLocationListing(paneId, location, true, navigation);
            });
    connect(pane, &FileBrowserPane::fileOpenRequested, this, &MainWindow::openLocalFile);
    connect(pane, &FileBrowserPane::historyChanged, this, [this, paneId, pane] {
        updatePaneTransferContexts();
        if (pane == m_workspaceTabs->activePane()) {
            updateNavigationActions();
        }
    });
    connect(pane, &FileBrowserPane::selectionChanged, this, [this, paneId] {
        if (paneId == m_workspaceTabs->paneId(m_workspaceTabs->activePane())) {
            updateOperationActions();
        }
    });
    connect(pane, &FileBrowserPane::contextMenuRequested, this, &MainWindow::showFileContextMenu);
    connect(pane, &FileBrowserPane::directoryItemCountRequested, this,
            [this, paneId](const rfm::core::BrowserLocation& location, quint64 generation,
                           const QString& name) {
                requestDirectoryItemCount(paneId, location, generation, name);
            });
    connect(pane, &FileBrowserPane::internalDropRequested, this,
            [this, paneId](rfm::core::InternalTransferPayload payload,
                           rfm::core::InternalTransferAction action, const QString& destination,
                           bool actionWasExplicitlyRequested) {
                handleInternalDrop(std::move(payload), action, paneId, destination,
                                   actionWasExplicitlyRequested);
            });
    connect(pane, &FileBrowserPane::crossSourceMoveUnsupported, this, [this] {
        statusBar()->showMessage(tr("Moving between local and SSH locations is not supported yet."),
                                 8000);
    });
}

void MainWindow::openLocalFile(const rfm::core::BrowserLocation& location)
{
    if (location.source != rfm::core::FileSource::Local ||
        location.machineId != QString::fromLatin1(rfm::core::LocalMachineId) ||
        !QFileInfo(location.path).isFile()) {
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(location.path))) {
        statusBar()->showMessage(tr("Could not open the file with the default application."), 8000);
    }
}

void MainWindow::requestDirectoryItemCount(quint64 paneId,
                                           const rfm::core::BrowserLocation& location,
                                           quint64 generation, const QString& name)
{
    FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
    if (pane == nullptr || pane->currentLocation() != location || name.isEmpty()) {
        return;
    }

    QString path;
    if (location.source == rfm::core::FileSource::Local &&
        location.machineId == QString::fromLatin1(rfm::core::LocalMachineId)) {
        if (!rfm::core::LocalFileSystem::isValidName(name)) {
            pane->setDirectoryItemCount(location, generation, name, std::nullopt);
            return;
        }
        path = QDir(location.path).filePath(name);
    } else if (location.source == rfm::core::FileSource::Ssh && m_connected &&
               location.machineId == activeRemoteMachineId()) {
        if (!rfm::core::RemotePath::isValidName(name)) {
            pane->setDirectoryItemCount(location, generation, name, std::nullopt);
            return;
        }
        path = rfm::core::RemotePath::join(location.path, name);
    } else {
        pane->setDirectoryItemCount(location, generation, name, std::nullopt);
        return;
    }

    const quint64 requestId = nextOperationId();
    m_directoryCountRequests.insert(requestId, {paneId, location, generation, name, path});
    if (location.source == rfm::core::FileSource::Local) {
        emit localDirectoryCountRequested(requestId, path);
    } else {
        emit remoteDirectoryCountRequested(requestId, path);
    }
}

void MainWindow::handleDirectoryCounted(quint64 requestId, const QString& path, quint64 count)
{
    const auto iterator = m_directoryCountRequests.find(requestId);
    if (iterator == m_directoryCountRequests.end()) {
        return;
    }
    const DirectoryCountRequest request = iterator.value();
    m_directoryCountRequests.erase(iterator);
    if (FileBrowserPane* const pane = m_workspaceTabs->pane(request.paneId); pane != nullptr) {
        pane->setDirectoryItemCount(request.location, request.generation, request.name,
                                    path == request.path ? std::optional<quint64>{count}
                                                         : std::nullopt);
    }
}

void MainWindow::handleDirectoryCountFailed(quint64 requestId, const QString& path)
{
    Q_UNUSED(path)
    const auto iterator = m_directoryCountRequests.find(requestId);
    if (iterator == m_directoryCountRequests.end()) {
        return;
    }
    const DirectoryCountRequest request = iterator.value();
    m_directoryCountRequests.erase(iterator);
    if (FileBrowserPane* const pane = m_workspaceTabs->pane(request.paneId); pane != nullptr) {
        pane->setDirectoryItemCount(request.location, request.generation, request.name,
                                    std::nullopt);
    }
}

void MainWindow::createNavigationBar()
{
    auto* const navigationBar = addToolBar(tr("Navigation"));
    navigationBar->setObjectName(QStringLiteral("navigationToolBar"));
    navigationBar->setMovable(false);

    m_backAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_ArrowBack), tr("Back"));
    m_backAction->setObjectName(QStringLiteral("backAction"));
    m_backAction->setShortcut(QKeySequence(QStringLiteral("Alt+Left")));
    m_forwardAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_ArrowForward), tr("Forward"));
    m_forwardAction->setObjectName(QStringLiteral("forwardAction"));
    m_forwardAction->setShortcut(QKeySequence(QStringLiteral("Alt+Right")));
    m_upAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_ArrowUp), tr("Parent folder"));
    m_upAction->setObjectName(QStringLiteral("upAction"));
    m_upAction->setShortcut(QKeySequence(QStringLiteral("Alt+Up")));
    m_refreshAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), tr("Refresh"));
    m_refreshAction->setObjectName(QStringLiteral("refreshAction"));
    m_refreshAction->setShortcut(QKeySequence(Qt::Key_F5));

    m_backAction->setEnabled(false);
    m_forwardAction->setEnabled(false);
    m_upAction->setEnabled(false);
    m_refreshAction->setEnabled(false);
    connect(m_backAction, &QAction::triggered, this,
            [this] { m_workspaceTabs->activePane()->requestBack(); });
    connect(m_forwardAction, &QAction::triggered, this,
            [this] { m_workspaceTabs->activePane()->requestForward(); });
    connect(m_upAction, &QAction::triggered, this, &MainWindow::requestParentDirectory);
    connect(m_refreshAction, &QAction::triggered, this, [this] {
        m_workspaceTabs->activePane()->requestRefresh();
        refreshStorage();
    });

    navigationBar->addSeparator();
    navigationBar->addAction(m_splitViewAction);
    navigationBar->addAction(m_newConnectionAction);
    navigationBar->addAction(m_disconnectAction);
}

void MainWindow::createPlacesDock()
{
    auto* const placesDock = new QDockWidget(tr("Places"), this);
    placesDock->setObjectName(QStringLiteral("placesDock"));
    placesDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto* const container = new QWidget(placesDock);
    auto* const layout = new QVBoxLayout(container);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    m_serverProfileErrorLabel = new QLabel(container);
    m_serverProfileErrorLabel->setObjectName(QStringLiteral("serverProfileErrorLabel"));
    m_serverProfileErrorLabel->setWordWrap(true);
    m_serverProfileErrorLabel->setVisible(false);
    layout->addWidget(m_serverProfileErrorLabel);

    m_navigationTree = new NavigationTree(container);
    layout->addWidget(m_navigationTree);

    connect(m_navigationTree, &NavigationTree::newConnectionRequested, m_newConnectionAction,
            &QAction::trigger);
    connect(m_navigationTree, &NavigationTree::editProfileRequested, this,
            &MainWindow::editServerProfile);
    connect(m_navigationTree->tree(), &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (item != nullptr &&
                    item->data(0, Qt::UserRole).toInt() ==
                        static_cast<int>(NavigationTree::NodeKind::ServerProfile) &&
                    selectedServerProfile().isValidSavedProfile()) {
                    connectToSelectedServerProfile();
                }
            });
    connect(m_navigationTree->tree(), &QWidget::customContextMenuRequested, this,
            &MainWindow::showPlacesContextMenu);
    connect(m_navigationTree, &NavigationTree::localLocationActivated, this,
            &MainWindow::openLocalLocation);
    connect(m_navigationTree, &NavigationTree::localVolumeMountRequested, this,
            [this](const rfm::core::StorageVolume& volume) {
                beginVolumeOperation(volume, rfm::core::VolumeOperation::Mount);
            });
    connect(m_navigationTree, &NavigationTree::localVolumeUnmountRequested, this,
            [this](const rfm::core::StorageVolume& volume) {
                beginVolumeOperation(volume, rfm::core::VolumeOperation::Unmount);
            });
    connect(m_navigationTree, &NavigationTree::remoteVolumeMountRequested, this,
            [this](const QString& machineId, const rfm::core::StorageVolume& volume) {
                beginRemoteVolumeOperation(machineId, volume, rfm::core::VolumeOperation::Mount);
            });
    connect(m_navigationTree, &NavigationTree::remoteVolumeUnmountRequested, this,
            [this](const QString& machineId, const rfm::core::StorageVolume& volume) {
                beginRemoteVolumeOperation(machineId, volume, rfm::core::VolumeOperation::Unmount);
            });
    connect(m_navigationTree, &NavigationTree::remoteLocationActivated, this,
            &MainWindow::openRemoteTreeLocation);
    connect(m_navigationTree, &NavigationTree::localDirectoryExpansionRequested, this,
            [this](const QString& path) {
                requestLocalDirectoryListing(0, path, false, PaneNavigation::Refresh, true);
            });
    connect(m_navigationTree, &NavigationTree::remoteDirectoryExpansionRequested, this,
            &MainWindow::requestRemoteTreeDirectory);

    placesDock->setWidget(container);
    addDockWidget(Qt::LeftDockWidgetArea, placesDock);
    m_placesDockAction = placesDock->toggleViewAction();
    m_placesDockAction->setObjectName(QStringLiteral("placesDockAction"));
    m_navigationTree->setShowHiddenFiles(m_showHiddenFilesAction->isChecked());
    for (const quint64 paneId : m_workspaceTabs->paneIds()) {
        m_workspaceTabs->pane(paneId)->setShowHiddenFiles(m_showHiddenFilesAction->isChecked());
    }
    loadServerProfiles();
}

void MainWindow::refreshStorage()
{
    if (!m_localStorageRefreshPending) {
        m_localStorageRefreshPending = true;
        emit localVolumesRequested();
    }
    if (m_connected && !m_remoteStorageRefreshPending) {
        m_remoteStorageProbePending = false;
        m_remoteStorageProbeRequestId = 0;
        m_remoteStorageProbeConnectionGeneration = 0;
        m_remoteStorageRefreshPending = true;
        m_remoteStorageRequestId = m_nextStorageRequestId++;
        m_remoteStorageRequestConnectionGeneration = m_connectionGeneration;
        emit remoteStorageRequested(m_remoteStorageRequestId);
    }
}

void MainWindow::probeStorage()
{
    probeLocalStorage();
    probeRemoteStorage();
}

void MainWindow::probeLocalStorage()
{
    if (m_localStorageRefreshPending || m_localStorageProbePending) {
        return;
    }
    m_localStorageProbePending = true;
    m_localStorageProbeRequestId = m_nextStorageRequestId++;
    emit localStorageProbeRequested(m_localStorageProbeRequestId);
}

void MainWindow::probeRemoteStorage()
{
    if (!m_connected || m_remoteStorageRefreshPending || m_remoteStorageProbePending) {
        return;
    }
    m_remoteStorageProbePending = true;
    m_remoteStorageProbeRequestId = m_nextStorageRequestId++;
    m_remoteStorageProbeConnectionGeneration = m_connectionGeneration;
    emit remoteStorageProbeRequested(m_remoteStorageProbeRequestId);
}

void MainWindow::handleLocalStorageVolumes(const QList<rfm::core::StorageVolume>& volumes)
{
    m_localStorageRefreshPending = false;
    if (m_localStorageRefreshAfterCurrent) {
        m_localStorageRefreshAfterCurrent = false;
        refreshStorage();
        return;
    }

    for (const quint64 id : std::as_const(m_volumeOperationsAwaitingRefresh)) {
        const auto request = m_volumeOperations.constFind(id);
        if (request != m_volumeOperations.cend()) {
            m_navigationTree->setLocalVolumeOperation(request->target.device, std::nullopt);
            m_volumeOperations.remove(id);
        }
    }
    m_volumeOperationsAwaitingRefresh.clear();
    m_localStorageVolumes = volumes;
    m_navigationTree->setStorageVolumes(volumes);
}

void MainWindow::handleLocalStorageProbe(quint64 requestId, const QByteArray& fingerprint)
{
    if (requestId == 0 || requestId != m_localStorageProbeRequestId) {
        return;
    }
    m_localStorageProbePending = false;
    m_localStorageProbeRequestId = 0;
    if (fingerprint != m_localStorageFingerprint) {
        refreshStorage();
    }
}

void MainWindow::handleRemoteStorageVolumes(quint64 requestId,
                                            const QList<rfm::core::StorageVolume>& volumes)
{
    if (requestId == 0 || requestId != m_remoteStorageRequestId ||
        m_remoteStorageRequestConnectionGeneration != m_connectionGeneration) {
        return;
    }
    m_remoteStorageRefreshPending = false;
    m_remoteStorageRequestId = 0;
    m_remoteStorageRequestConnectionGeneration = 0;
    if (std::exchange(m_remoteStorageRefreshAfterCurrent, false)) {
        refreshStorage();
        return;
    }
    if (m_connected) {
        for (const quint64 id : std::as_const(m_remoteVolumeOperationsAwaitingRefresh)) {
            const auto context = m_remoteVolumeOperations.constFind(id);
            if (context != m_remoteVolumeOperations.cend()) {
                m_navigationTree->setVolumeOperation(context->machineId,
                                                     context->request.target.device, std::nullopt);
                m_remoteVolumeOperations.remove(id);
            }
        }
        m_remoteVolumeOperationsAwaitingRefresh.clear();
        m_remoteStorageFingerprint = std::exchange(m_pendingRemoteStorageFingerprint, {});
        m_remoteStorageVolumes = volumes;
        m_navigationTree->setRemoteStorageVolumes(activeRemoteMachineId(), volumes);
    }
}

void MainWindow::handleRemoteStorageError(quint64 requestId, const QString& error)
{
    if (requestId == 0 || requestId != m_remoteStorageRequestId ||
        m_remoteStorageRequestConnectionGeneration != m_connectionGeneration) {
        return;
    }
    m_remoteStorageRefreshPending = false;
    m_remoteStorageRequestId = 0;
    m_remoteStorageRequestConnectionGeneration = 0;
    m_pendingRemoteStorageFingerprint.clear();
    if (std::exchange(m_remoteStorageRefreshAfterCurrent, false) && m_connected) {
        refreshStorage();
        return;
    }
    for (const quint64 id : std::as_const(m_remoteVolumeOperationsAwaitingRefresh)) {
        const auto context = m_remoteVolumeOperations.constFind(id);
        if (context != m_remoteVolumeOperations.cend()) {
            m_navigationTree->setVolumeOperation(context->machineId, context->request.target.device,
                                                 std::nullopt);
            m_remoteVolumeOperations.remove(id);
        }
    }
    m_remoteVolumeOperationsAwaitingRefresh.clear();
    if (m_connected && !error.isEmpty()) {
        statusBar()->showMessage(tr("Unable to refresh server storage: %1").arg(error), 8000);
    }
}

void MainWindow::handleRemoteStorageFingerprint(quint64 requestId, const QByteArray& fingerprint)
{
    if (requestId != 0 && requestId == m_remoteStorageRequestId && m_connected &&
        m_remoteStorageRequestConnectionGeneration == m_connectionGeneration) {
        m_pendingRemoteStorageFingerprint = fingerprint;
    }
}

void MainWindow::handleRemoteStorageProbe(quint64 requestId, const QByteArray& fingerprint)
{
    if (requestId == 0 || requestId != m_remoteStorageProbeRequestId || !m_connected ||
        m_remoteStorageProbeConnectionGeneration != m_connectionGeneration) {
        return;
    }
    m_remoteStorageProbePending = false;
    m_remoteStorageProbeRequestId = 0;
    m_remoteStorageProbeConnectionGeneration = 0;
    if (fingerprint != m_remoteStorageFingerprint) {
        refreshStorage();
    }
}

void MainWindow::handleRemoteStorageProbeError(quint64 requestId, const QString& error)
{
    if (requestId == 0 || requestId != m_remoteStorageProbeRequestId) {
        return;
    }
    m_remoteStorageProbePending = false;
    m_remoteStorageProbeRequestId = 0;
    m_remoteStorageProbeConnectionGeneration = 0;
    if (m_connected && !error.isEmpty()) {
        statusBar()->showMessage(tr("Unable to probe server storage: %1").arg(error), 8000);
    }
}

void MainWindow::loadServerProfiles()
{
    QString error;
    m_serverProfiles = m_serverProfileStore->load(&error);
    refreshServerProfileViews();
    m_serverProfileErrorLabel->setText(error);
    m_serverProfileErrorLabel->setVisible(!error.isEmpty());
    if (!error.isEmpty()) {
        statusBar()->showMessage(tr("Unable to load server profiles: %1").arg(error), 10000);
    }
}

void MainWindow::refreshServerProfileViews()
{
    if (m_homePage != nullptr) {
        m_homePage->setProfiles(m_serverProfiles);
    }
    if (m_navigationTree == nullptr) {
        return;
    }
    const QString selectedId = selectedServerProfile().id;
    m_navigationTree->setProfiles(m_serverProfiles);
    if (m_connected) {
        m_navigationTree->setActiveServer(activeRemoteMachine(), m_remoteInitialPath);
    }
    if (!selectedId.isEmpty()) {
        QTreeWidgetItemIterator iterator(m_navigationTree->tree());
        while (*iterator != nullptr) {
            if ((*iterator)->data(0, Qt::UserRole + 5).toString() == selectedId &&
                (*iterator)->data(0, Qt::UserRole).toInt() ==
                    static_cast<int>(NavigationTree::NodeKind::ServerProfile)) {
                m_navigationTree->tree()->setCurrentItem(*iterator);
                break;
            }
            ++iterator;
        }
    }
}

rfm::core::ConnectionProfile MainWindow::selectedServerProfile() const
{
    if (m_navigationTree == nullptr) {
        return {};
    }
    const QString id = m_navigationTree->selectedProfileId();
    for (const rfm::core::ConnectionProfile& profile : m_serverProfiles) {
        if (profile.id == id) {
            return profile;
        }
    }
    return {};
}

void MainWindow::editSelectedServerProfile()
{
    const rfm::core::ConnectionProfile selected = selectedServerProfile();
    editServerProfile(selected.id);
}

void MainWindow::editServerProfile(const QString& id)
{
    const auto selected = std::ranges::find_if(
        m_serverProfiles, [&id](const auto& profile) { return profile.id == id; });
    if (selected == m_serverProfiles.cend() || !selected->isValidSavedProfile()) {
        return;
    }
    ServerProfileDialog dialog(this);
    dialog.setProfile(*selected);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    QString error;
    if (!m_serverProfileStore->upsert(dialog.profile(), &error)) {
        QMessageBox::warning(this, tr("Unable to save server"), error);
        return;
    }
    loadServerProfiles();
}

void MainWindow::removeSelectedServerProfile()
{
    const rfm::core::ConnectionProfile selected = selectedServerProfile();
    if (!selected.isValidSavedProfile()) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Remove server"),
        tr("Remove “%1” from saved servers?").arg(selected.effectiveDisplayName()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!m_serverProfileStore->remove(selected.id, &error)) {
        QMessageBox::warning(this, tr("Unable to remove server"), error);
        return;
    }
    loadServerProfiles();
}

void MainWindow::connectToSelectedServerProfile()
{
    const rfm::core::ConnectionProfile selected = selectedServerProfile();
    if (!selected.isValidSavedProfile()) {
        return;
    }
    if (m_connected && profilesHaveSameConnectionSettings(selected, m_activeProfile)) {
        requestDisconnection();
    } else if (!m_connected) {
        connectToServerProfile(selected.id);
    }
}

void MainWindow::connectToServerProfile(const QString& id)
{
    if (m_connected || !m_newConnectionAction->isEnabled()) {
        return;
    }
    const auto profile = std::ranges::find_if(
        m_serverProfiles, [&id](const auto& candidate) { return candidate.id == id; });
    if (profile != m_serverProfiles.cend() && profile->isValidSavedProfile()) {
        beginConnection(*profile);
    }
}

void MainWindow::showPlacesContextMenu(const QPoint& position)
{
    const QList<NavigationTree::ContextAction> actions =
        m_navigationTree->contextActionsAt(position);
    if (actions.isEmpty()) {
        return;
    }
    m_navigationTree->selectItemAt(position);
    const bool serverContext = actions.contains(NavigationTree::ContextAction::Connect) ||
                               actions.contains(NavigationTree::ContextAction::Disconnect);
    QMenu menu(m_navigationTree->tree());
    for (const NavigationTree::ContextAction action : actions) {
        switch (action) {
        case NavigationTree::ContextAction::Connect:
            menu.addAction(tr("Connect"), this, &MainWindow::connectToSelectedServerProfile)
                ->setEnabled(!m_connected && m_newConnectionAction->isEnabled());
            break;
        case NavigationTree::ContextAction::Disconnect:
            menu.addAction(m_disconnectAction);
            break;
        case NavigationTree::ContextAction::RemoveServer:
            menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete"),
                                            style()->standardIcon(QStyle::SP_TrashIcon)),
                           tr("Remove server"), this, &MainWindow::removeSelectedServerProfile);
            break;
        case NavigationTree::ContextAction::Open:
            menu.addAction(tr("Open"), m_navigationTree, &NavigationTree::activateSelectedItem);
            break;
        case NavigationTree::ContextAction::Mount:
            menu.addAction(tr("Mount"), m_navigationTree, &NavigationTree::mountSelectedVolume);
            break;
        case NavigationTree::ContextAction::Unmount:
            menu.addAction(tr("Unmount"), m_navigationTree, &NavigationTree::unmountSelectedVolume);
            break;
        case NavigationTree::ContextAction::Properties:
            if (serverContext) {
                menu.addAction(tr("Properties"), this, &MainWindow::editSelectedServerProfile);
            } else {
                menu.addAction(tr("Properties"), this, &MainWindow::showSelectedPlaceProperties);
            }
            break;
        }
    }
    menu.exec(m_navigationTree->tree()->viewport()->mapToGlobal(position));
}

void MainWindow::showSelectedPlaceProperties()
{
    showPropertiesDialog(m_navigationTree->selectedPropertiesTitle(),
                         m_navigationTree->selectedPropertiesText());
}

void MainWindow::showContextEntryProperties()
{
    const auto properties = m_workspaceTabs->activePane()->contextEntryProperties();
    if (properties.has_value()) {
        showPropertiesDialog(properties->title, properties->text);
    }
}

void MainWindow::showPropertiesDialog(const QString& title, const QString& text)
{
    if (!title.isEmpty() && !text.isEmpty()) {
        QMessageBox dialog(QMessageBox::Information, tr("Properties — %1").arg(title), text,
                           QMessageBox::Ok, this);
        dialog.setTextFormat(Qt::PlainText);
        dialog.exec();
    }
}

void MainWindow::requestDisconnection()
{
    if (!m_disconnectAction->isEnabled()) {
        return;
    }
    setBusy(true, tr("Disconnecting…"));
    emit disconnectionRequested();
}

void MainWindow::createOperationDock()
{
    auto* const operationDock = new QDockWidget(tr("Operations"), this);
    operationDock->setObjectName(QStringLiteral("operationDock"));
    operationDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    auto* const titleBar = new QWidget(operationDock);
    auto* const titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(4, 0, 2, 0);
    titleLayout->setSpacing(2);
    auto* const title = new QLabel(operationDock->windowTitle(), titleBar);
    titleLayout->addWidget(title);
    titleLayout->addStretch();
    auto* const clearButton = new QToolButton(titleBar);
    clearButton->setObjectName(QStringLiteral("clearOperationHistoryButton"));
    clearButton->setIcon(QIcon::fromTheme(QStringLiteral("edit-clear"),
                                          style()->standardIcon(QStyle::SP_DialogResetButton)));
    clearButton->setToolTip(tr("Clear operation history"));
    clearButton->setAccessibleName(clearButton->toolTip());
    clearButton->setEnabled(false);
    titleLayout->addWidget(clearButton);
    auto* const maximizeButton = new QToolButton(titleBar);
    maximizeButton->setObjectName(QStringLiteral("operationDockMaximizeButton"));
    maximizeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));
    maximizeButton->setToolTip(tr("Maximize"));
    maximizeButton->setAccessibleName(maximizeButton->toolTip());
    titleLayout->addWidget(maximizeButton);
    auto* const closeButton = new QToolButton(titleBar);
    closeButton->setObjectName(QStringLiteral("operationDockCloseButton"));
    closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    closeButton->setToolTip(tr("Close"));
    closeButton->setAccessibleName(closeButton->toolTip());
    titleLayout->addWidget(closeButton);
    operationDock->setTitleBarWidget(titleBar);
    m_operationPanel = new OperationPanel(operationDock);
    operationDock->setWidget(m_operationPanel);
    addDockWidget(Qt::BottomDockWidgetArea, operationDock);
    m_operationDockAction = operationDock->toggleViewAction();
    m_operationDockAction->setObjectName(QStringLiteral("operationDockAction"));

    connect(m_operationPanel, &OperationPanel::pauseRequested, this,
            &MainWindow::pauseTransferRequested);
    connect(m_operationPanel, &OperationPanel::resumeRequested, this,
            &MainWindow::resumeTransferRequested);
    connect(m_operationPanel, &OperationPanel::cancelRequested, this, [this](quint64 id) {
        if (m_localOperationRequests.contains(id)) {
            auto operation = m_operations.value(id);
            operation.state = rfm::core::OperationState::Cancelling;
            updateTrackedOperation(operation);
            m_localFileOperationWorker->requestCancellation(id);
        } else if (m_remoteOperations.contains(id)) {
            emit cancelRemoteOperationRequested(id);
        } else {
            emit cancelTransferRequested(id);
        }
    });
    connect(m_operationPanel, &OperationPanel::removeTerminalRequested, this,
            &MainWindow::removeTerminalOperation);
    connect(m_operationPanel, &OperationPanel::clearTerminalRequested, this,
            &MainWindow::clearTerminalOperations);
    connect(clearButton, &QToolButton::clicked, m_operationPanel,
            &OperationPanel::clearTerminalRequested);
    connect(m_operationPanel, &OperationPanel::terminalOperationsAvailableChanged, clearButton,
            &QToolButton::setEnabled);
    connect(maximizeButton, &QToolButton::clicked, operationDock,
            [operationDock] { operationDock->setFloating(!operationDock->isFloating()); });
    connect(closeButton, &QToolButton::clicked, operationDock, &QDockWidget::close);
}

void MainWindow::createCentralPages()
{
    m_centralStack = new QStackedWidget(this);
    m_centralStack->setObjectName(QStringLiteral("centralStack"));
    m_homePage = new HomePage(m_centralStack);
    m_centralStack->addWidget(m_homePage);
    m_centralStack->addWidget(m_workspaceTabs);
    m_centralStack->setCurrentWidget(m_homePage);
    setCentralWidget(m_centralStack);

    connect(m_homePage, &HomePage::connectProfileRequested, this,
            &MainWindow::connectToServerProfile);
    connect(m_homePage, &HomePage::editProfileRequested, this, &MainWindow::editServerProfile);
    connect(m_homePage, &HomePage::newConnectionRequested, m_newConnectionAction,
            &QAction::trigger);
    m_homePage->setProfiles(m_serverProfiles);
}

void MainWindow::showConnectionDialog()
{
    if (m_connectionDialog != nullptr) {
        m_connectionDialog->raise();
        m_connectionDialog->activateWindow();
        return;
    }

    auto* const dialog = new ConnectionDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setProfile({});
    m_connectionDialog = dialog;
    connect(dialog, &ConnectionDialog::connectionRequested, this, &MainWindow::beginConnection);
    dialog->open();
}

void MainWindow::beginConnection(const rfm::core::ConnectionProfile& profile)
{
    if (m_connected || m_connecting) {
        return;
    }
    if (!profile.isValid()) {
        showConnectionError(tr("Invalid connection settings."));
        return;
    }
    clearInternalClipboard();
    updatePaneTransferContexts();
    stopAutomaticRefresh();
    m_activeProfile = profile;
    m_connecting = true;
    setBusy(true, tr("Connecting securely to %1…").arg(m_activeProfile.host));
    emit connectionRequested(m_activeProfile);
}

void MainWindow::showPasswordAuthentication()
{
    showPasswordAuthenticationForReason(rfm::ssh::SshAuthenticationPolicy::passwordPromptReason(
        m_activeProfile.authenticationMode, rfm::ssh::AuthenticationResult::Denied,
        !m_activeProfile.privateKeyPath.trimmed().isEmpty()));
}

void MainWindow::showPasswordAuthenticationForReason(rfm::ssh::PasswordAuthenticationReason reason)
{
    if (!m_connecting || !m_activeProfile.isValid()) {
        return;
    }
    QString message;
    switch (reason) {
    case rfm::ssh::PasswordAuthenticationReason::ExplicitKeyFailed:
        message = tr("SSH key authentication failed. Enter your password to continue.");
        break;
    case rfm::ssh::PasswordAuthenticationReason::KeyOrAgentFailed:
        message = tr("SSH key or agent authentication failed. Enter your password to continue.");
        break;
    case rfm::ssh::PasswordAuthenticationReason::AdditionalPasswordRequired:
        message = tr("Additional password authentication is required.");
        break;
    case rfm::ssh::PasswordAuthenticationReason::PasswordOnly:
        message = tr("Password authentication is required to continue.");
        break;
    }
    setBusy(false);
    if (m_passwordAuthenticationDialog != nullptr) {
        m_passwordAuthenticationDialog->setAuthenticationMessage(message);
        m_passwordAuthenticationDialog->raise();
        m_passwordAuthenticationDialog->activateWindow();
        return;
    }

    auto* const dialog = new PasswordAuthenticationDialog(m_activeProfile, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAuthenticationMessage(message);
    m_passwordAuthenticationDialog = dialog;
    connect(dialog, &PasswordAuthenticationDialog::authenticationRequested, this,
            &MainWindow::submitPasswordAuthentication);
    connect(dialog, &QDialog::rejected, this, &MainWindow::cancelPasswordAuthentication);
    dialog->open();
}

void MainWindow::showPasswordAuthenticationError(const QString& message)
{
    setBusy(false);
    if (m_passwordAuthenticationDialog != nullptr) {
        m_passwordAuthenticationDialog->showAuthenticationError(message);
    }
}

void MainWindow::submitPasswordAuthentication()
{
    if (m_passwordAuthenticationDialog == nullptr) {
        return;
    }
    rfm::core::SecurePassword password = m_passwordAuthenticationDialog->takePassword();
    if (password.isEmpty()) {
        m_passwordAuthenticationDialog->showAuthenticationError(tr("Enter a password."));
        return;
    }
    setBusy(true, tr("Authenticating…"));
    m_sshSession->postPasswordAuthentication(std::move(password));
}

void MainWindow::cancelPasswordAuthentication()
{
    emit passwordAuthenticationCancelled();
    m_connecting = false;
    setBusy(false);
    if (m_connectionDialog != nullptr) {
        m_connectionDialog->connectionCancelled();
    }
}

QString MainWindow::saveConnectedProfileIfRequested()
{
    if (m_connectionDialog == nullptr || !m_connectionDialog->saveServerRequested() ||
        !m_activeProfile.id.trimmed().isEmpty()) {
        return {};
    }
    for (const rfm::core::ConnectionProfile& existing : std::as_const(m_serverProfiles)) {
        if (profilesHaveSameConnectionSettings(existing, m_activeProfile)) {
            m_activeProfile.id = existing.id;
            m_activeProfile.displayName = existing.displayName;
            return {};
        }
    }

    rfm::core::ConnectionProfile profile = m_activeProfile;
    profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString error;
    if (!m_serverProfileStore->upsert(profile, &error)) {
        return error;
    }
    m_activeProfile = profile;
    loadServerProfiles();
    return {};
}

void MainWindow::handleConnected(const QString& path, const QList<rfm::core::RemoteEntry>& entries)
{
    const QString profileSaveError = saveConnectedProfileIfRequested();
    if (m_connectionDialog != nullptr) {
        m_connectionDialog->connectionSucceeded();
    }
    if (m_passwordAuthenticationDialog != nullptr) {
        m_passwordAuthenticationDialog->authenticationSucceeded();
    }
    m_connecting = false;
    m_connected = true;
    m_activeRemoteMachineId = QStringLiteral("ssh:%1@%2:%3")
                                  .arg(m_activeProfile.username, m_activeProfile.host,
                                       QString::number(m_activeProfile.port));
    m_activeSavedProfileId.clear();
    for (const rfm::core::ConnectionProfile& profile : std::as_const(m_serverProfiles)) {
        if (profile.id == m_activeProfile.id &&
            profilesHaveSameConnectionSettings(profile, m_activeProfile)) {
            m_activeSavedProfileId = profile.id;
            break;
        }
    }
    m_remoteInitialPath = path;
    showRemoteDirectory(path, entries);
    refreshServerProfileViews();
    m_navigationTree->setActiveServer(activeRemoteMachine(), path);
    m_navigationTree->setRemoteDirectory(activeRemoteMachineId(), path, entries);
    refreshStorage();
    statusBar()->showMessage(
        profileSaveError.isEmpty()
            ? tr("Connected securely to %1").arg(m_activeProfile.host)
            : tr("Connected securely to %1, but the server profile could not be saved: %2")
                  .arg(m_activeProfile.host, profileSaveError));
}

void MainWindow::showHostKeyConfirmation(const QString& host, const QString& fingerprint)
{
    QMessageBox confirmation(
        QMessageBox::Warning, tr("Unknown SSH host key"),
        tr("This is the first connection to %1.\n\nSHA-256 fingerprint:\n%2\n\n"
           "Verify this fingerprint with the server administrator before continuing.")
            .arg(host, fingerprint),
        QMessageBox::NoButton, this);
    auto* const trustButton =
        confirmation.addButton(tr("Trust and connect"), QMessageBox::AcceptRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.exec();
    const bool accepted = confirmation.clickedButton() == trustButton;
    if (accepted) {
        setBusy(true, tr("Authenticating…"));
    }
    emit hostKeyDecision(accepted);
}

void MainWindow::showRemoteDirectory(const QString& path,
                                     const QList<rfm::core::RemoteEntry>& entries)
{
    m_connected = true;
    m_transferCoordinator->executorConnected();
    if (m_activeRemoteMachineId.isEmpty()) {
        m_activeRemoteMachineId = QStringLiteral("ssh:%1@%2:%3")
                                      .arg(m_activeProfile.username, m_activeProfile.host,
                                           QString::number(m_activeProfile.port));
    }
    ++m_connectionGeneration;
    clearInternalClipboard();
    m_centralStack->setCurrentWidget(m_workspaceTabs);
    FileBrowserPane* const pane = m_workspaceTabs->activePane();
    const quint64 paneId = m_workspaceTabs->paneId(pane);
    static_cast<void>(beginPaneNavigation(paneId, rfm::core::FileSource::Ssh));
    const QString displayPath = remoteDisplayUrl(m_activeProfile, path);
    pane->showDirectory({rfm::core::FileSource::Ssh, activeRemoteMachineId(), path}, displayPath,
                        entries, PaneNavigation::Initial);
    pane->setProperty("connectionGeneration", QVariant::fromValue(m_connectionGeneration));
    updatePaneTransferContexts();
    setBusy(false);
    updateOperationActions();
    if (!m_autoRefreshTimer->isActive()) {
        m_autoRefreshTimer->start();
    }
    if (m_workspaceTabs->activeWorkspace()->isSplit()) {
        const quint64 activePaneId = paneId;
        for (const quint64 paneId : m_workspaceTabs->visiblePaneIds()) {
            FileBrowserPane* const otherPane = m_workspaceTabs->pane(paneId);
            if (paneId != activePaneId && (otherPane->source() == rfm::core::FileSource::None ||
                                           otherPane->source() == rfm::core::FileSource::Ssh)) {
                requestDirectoryListing(paneId, path, true, true, PaneNavigation::Initial);
            }
        }
    }
}

void MainWindow::handleDirectoryListed(quint64 requestId, const QString& path,
                                       const QList<rfm::core::RemoteEntry>& entries)
{
    if (requestId == 0 || requestId != m_activeDirectoryRequestId) {
        return;
    }
    if (!m_directoryRequests.contains(requestId)) {
        m_activeDirectoryRequestId = 0;
        startNextDirectoryListing();
        return;
    }

    const DirectoryRequest request = m_directoryRequests.take(requestId);
    m_activeDirectoryRequestId = 0;
    if (request.treeRequest) {
        if (request.connectionGeneration == m_connectionGeneration && m_connected) {
            m_navigationTree->setRemoteDirectory(request.profileId, path, entries);
        }
        startNextDirectoryListing();
        return;
    }
    const bool expected =
        m_expectedDirectoryRequests.value(request.paneId) == requestId &&
        isExpectedPaneNavigation(request.paneId, request.navigationGeneration,
                                 rfm::core::FileSource::Ssh, request.connectionGeneration);
    FileBrowserPane* const pane = m_workspaceTabs->pane(request.paneId);
    if (expected && pane != nullptr && !pane->isHidden()) {
        const QString displayPath = remoteDisplayUrl(m_activeProfile, path);
        pane->showDirectory({rfm::core::FileSource::Ssh, activeRemoteMachineId(), path},
                            displayPath, entries, request.navigation);
        pane->setProperty("connectionGeneration", QVariant::fromValue(m_connectionGeneration));
        m_expectedDirectoryRequests.remove(request.paneId);
        setPaneBusy(request.paneId, false);
    }
    startNextDirectoryListing();
}

void MainWindow::handleDirectoryListingError(quint64 requestId, const QString& path,
                                             const QString& error)
{
    if (requestId == 0 || requestId != m_activeDirectoryRequestId) {
        return;
    }
    if (!m_directoryRequests.contains(requestId)) {
        m_activeDirectoryRequestId = 0;
        startNextDirectoryListing();
        return;
    }

    const DirectoryRequest request = m_directoryRequests.take(requestId);
    m_activeDirectoryRequestId = 0;
    if (request.treeRequest) {
        if (request.connectionGeneration == m_connectionGeneration) {
            m_navigationTree->setDirectoryError(false, request.profileId, path, error);
        }
        startNextDirectoryListing();
        return;
    }
    const bool expected =
        m_expectedDirectoryRequests.value(request.paneId) == requestId &&
        isExpectedPaneNavigation(request.paneId, request.navigationGeneration,
                                 rfm::core::FileSource::Ssh, request.connectionGeneration);
    FileBrowserPane* const pane = m_workspaceTabs->pane(request.paneId);
    if (expected && pane != nullptr && !pane->isHidden()) {
        m_expectedDirectoryRequests.remove(request.paneId);
        setPaneBusy(request.paneId, false);
        statusBar()->showMessage(tr("Unable to list %1: %2").arg(path, error), 8000);
    }
    startNextDirectoryListing();
}

void MainWindow::showConnectionError(const QString& message)
{
    if (m_connectionErrorNotificationActive) {
        return;
    }
    const bool initialConnectionAttempt =
        m_connectionDialog != nullptr &&
        m_connectionDialog->state() == ConnectionDialog::State::Connecting;
    if (m_passwordAuthenticationDialog != nullptr) {
        disconnect(m_passwordAuthenticationDialog, &QDialog::rejected, this,
                   &MainWindow::cancelPasswordAuthentication);
        m_passwordAuthenticationDialog->connectionFailed();
    }
    for (auto operation : std::as_const(m_remoteOperations)) {
        operation.state = rfm::core::OperationState::Failed;
        operation.error = message;
        updateTrackedOperation(operation);
    }
    resetDisconnectedUi();
    if (initialConnectionAttempt && m_connectionDialog != nullptr) {
        m_connectionDialog->showConnectionError(message);
    } else {
        m_connectionErrorNotificationActive = true;
        QMessageBox::critical(this, tr("SSH connection error"), message);
        QTimer::singleShot(0, this, [this] { m_connectionErrorNotificationActive = false; });
    }
}

void MainWindow::handleDisconnected() { resetDisconnectedUi(); }

void MainWindow::resetDisconnectedUi()
{
    m_connecting = false;
    m_connected = false;
    m_transferCoordinator->executorDisconnected();
    m_remoteStorageRefreshPending = false;
    m_remoteStorageRefreshAfterCurrent = false;
    m_remoteStorageRequestId = 0;
    m_remoteStorageRequestConnectionGeneration = 0;
    m_remoteStorageProbePending = false;
    m_remoteStorageProbeRequestId = 0;
    m_remoteStorageProbeConnectionGeneration = 0;
    m_remoteStorageFingerprint.clear();
    m_pendingRemoteStorageFingerprint.clear();
    m_remoteStorageVolumes.clear();
    for (const RemoteVolumeOperationContext& context : std::as_const(m_remoteVolumeOperations)) {
        if (context.authenticationDialog != nullptr) {
            context.authenticationDialog->reject();
        }
    }
    for (const RemoteVolumeOperationContext& context : std::as_const(m_remoteVolumeOperations)) {
        m_navigationTree->setVolumeOperation(context.machineId, context.request.target.device,
                                             std::nullopt);
    }
    m_remoteVolumeOperations.clear();
    m_remoteVolumeOperationsAwaitingRefresh.clear();
    clearInternalClipboard();
    updatePaneTransferContexts();
    stopAutomaticRefresh();
    m_refreshDebounceTimer->stop();
    m_scheduledPaneRefreshes.clear();
    m_scheduledTransferRefreshLocations.clear();
    m_directoryRequests.clear();
    m_directoryQueue.clear();
    m_expectedDirectoryRequests.clear();
    m_pendingRemoteFilesystemPreflights.clear();
    m_expectedLocalDirectoryRequests.clear();
    m_paneNavigationGenerations.clear();
    m_expectedPaneSources.clear();
    m_busyPanes.clear();
    m_activeDirectoryRequestId = 0;
    m_pendingTransferRequests.clear();
    m_nonTerminalTransfers.clear();
    m_operationContexts.clear();
    m_remoteOperations.clear();
    m_silentRemoteOperationResults.clear();
    m_transferPanes.clear();
    m_transferRefreshContexts.clear();
    bool hasLocalPane = false;
    for (const quint64 paneId : m_workspaceTabs->paneIds()) {
        FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
        if (pane->source() == rfm::core::FileSource::Ssh) {
            pane->clear();
        } else {
            pane->removeHistoryForSource(rfm::core::FileSource::Ssh);
        }
        hasLocalPane = hasLocalPane || pane->source() == rfm::core::FileSource::Local;
    }
    m_centralStack->setCurrentWidget(hasLocalPane ? static_cast<QWidget*>(m_workspaceTabs)
                                                  : static_cast<QWidget*>(m_homePage));
    m_activeProfile = {};
    m_activeSavedProfileId.clear();
    m_activeRemoteMachineId.clear();
    m_remoteInitialPath.clear();
    m_navigationTree->clearActiveServer();
    setBusy(false);
    refreshServerProfileViews();
    statusBar()->showMessage(tr("Disconnected"));
}

void MainWindow::showFileContextMenu(const QPoint& globalPosition)
{
    if (m_busy) {
        return;
    }
    updateOperationActions();
    QMenu menu(this);
    const bool hasSelection = !selectedEntries().isEmpty();
    if (m_openAction->isEnabled()) {
        menu.addAction(m_openAction);
        menu.addSeparator();
    }
    if (hasSelection) {
        menu.addAction(m_clipboardCopyAction);
        menu.addAction(m_clipboardCutAction);
    }
    menu.addAction(m_clipboardPasteAction);
    menu.addSeparator();
    if (hasSelection) {
        if (m_workspaceTabs->activeWorkspace()->isSplit()) {
            menu.addAction(m_copyToOtherPaneAction);
            menu.addAction(m_moveToOtherPaneAction);
        }
        menu.addAction(m_copyAction);
        menu.addAction(m_moveAction);
        menu.addSeparator();
        menu.addAction(m_renameAction);
        menu.addAction(m_removeAction);
        menu.addSeparator();
    }
    menu.addAction(m_createDirectoryAction);
    if (m_workspaceTabs->activePane()->contextEntryProperties().has_value()) {
        menu.addSeparator();
        menu.addAction(m_filePropertiesAction);
    }
    menu.exec(globalPosition);
}

void MainWindow::openContextEntry()
{
    FileBrowserPane* const pane = m_workspaceTabs->activePane();
    if (pane != nullptr && selectedEntries().size() == 1) {
        pane->requestOpenContextEntry();
    }
}

void MainWindow::createDirectory()
{
    FileBrowserPane* const pane = m_workspaceTabs->activePane();
    const bool local = pane->source() == rfm::core::FileSource::Local;
    const bool remote = pane->source() == rfm::core::FileSource::Ssh && m_connected &&
                        pane->currentLocation().machineId == activeRemoteMachineId();
    if (!local && !remote) {
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("New folder"), tr("Folder name:"),
                                               QLineEdit::Normal, {}, &accepted);
    if (!accepted) {
        return;
    }
    const bool validName = local ? rfm::core::LocalFileSystem::isValidName(name)
                                 : rfm::core::RemotePath::isValidName(name);
    if (!validName) {
        QMessageBox::warning(this, tr("Invalid folder name"),
                             tr("The name must not be empty, '.', '..', or contain a path "
                                "separator or another character invalid on this platform."));
        return;
    }
    const quint64 id = nextOperationId();
    const quint64 paneId = m_workspaceTabs->paneId(pane);
    if (local) {
        m_localOperationContexts.insert(id,
                                        {paneId, paneId, pane->currentPath(), pane->currentPath()});
        setPaneBusy(paneId, true, tr("Creating folder %1…").arg(name));
        emit localFileOperationRequested({id,
                                          rfm::core::LocalFileOperationKind::CreateDirectory,
                                          pane->currentPath(),
                                          {},
                                          name});
        return;
    }
    setBusy(true, tr("Creating folder %1…").arg(name));
    m_operationContexts.insert(id, {paneId, paneId, pane->currentPath(), pane->currentPath()});
    emit createDirectoryRequested(id, pane->currentPath(), name);
}

void MainWindow::renameSelectedEntry()
{
    const QList<rfm::core::RemoteSelection> selection = selectedEntries();
    if (selection.size() != 1) {
        return;
    }
    FileBrowserPane* const pane = m_workspaceTabs->activePane();
    const bool local = pane->source() == rfm::core::FileSource::Local;
    const bool remote = pane->source() == rfm::core::FileSource::Ssh && m_connected &&
                        pane->currentLocation().machineId == activeRemoteMachineId();
    if (!local && !remote) {
        return;
    }
    const QString oldName = local ? QFileInfo(selection.constFirst().path).fileName()
                                  : rfm::core::RemotePath::fileName(selection.constFirst().path);
    bool accepted = false;
    const QString newName = QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                                  QLineEdit::Normal, oldName, &accepted);
    if (!accepted) {
        return;
    }
    const bool validName = local ? rfm::core::LocalFileSystem::isValidName(newName)
                                 : rfm::core::RemotePath::isValidName(newName);
    if (!validName) {
        QMessageBox::warning(this, tr("Invalid name"),
                             tr("The name must not be empty, '.', '..', or contain a path "
                                "separator or another character invalid on this platform."));
        return;
    }
    const quint64 id = nextOperationId();
    const quint64 paneId = m_workspaceTabs->paneId(pane);
    if (local) {
        m_localOperationContexts.insert(id,
                                        {paneId, paneId, pane->currentPath(), pane->currentPath()});
        setPaneBusy(paneId, true, tr("Renaming %1…").arg(oldName));
        emit localFileOperationRequested({id,
                                          rfm::core::LocalFileOperationKind::Rename,
                                          pane->currentPath(),
                                          {selection.constFirst().path},
                                          newName});
        return;
    }
    setBusy(true, tr("Renaming %1…").arg(oldName));
    m_operationContexts.insert(id, {paneId, paneId, pane->currentPath(), pane->currentPath()});
    emit renameRequested(id, selection.constFirst().path, newName);
}

void MainWindow::moveSelectedEntries()
{
    FileBrowserPane* const sourcePane = m_workspaceTabs->activePane();
    const quint64 sourcePaneId = m_workspaceTabs->paneId(sourcePane);
    const QList<rfm::core::RemoteSelection> selection = selectedEntries();
    if (selection.isEmpty()) {
        return;
    }
    QString destination;
    if (sourcePane->source() == rfm::core::FileSource::Local) {
        destination =
            QFileDialog::getExistingDirectory(this, tr("Select the move destination folder"),
                                              sourcePane->currentPath(), QFileDialog::ShowDirsOnly);
    } else {
        destination = askDestination(tr("Move selected items"));
    }
    if (destination.isEmpty()) {
        return;
    }
    if (sourcePane->source() == rfm::core::FileSource::Local) {
        startLocalOperation(rfm::core::LocalFileOperationKind::Move, sourcePaneId, sourcePaneId,
                            sourcePane->currentPath(), destination, selection);
    } else {
        startRemoteTransfer(rfm::core::InternalTransferAction::Move,
                            transferPayload(sourcePaneId, selection), 0, destination);
    }
}

void MainWindow::copySelectedEntries()
{
    FileBrowserPane* const sourcePane = m_workspaceTabs->activePane();
    const quint64 sourcePaneId = m_workspaceTabs->paneId(sourcePane);
    const QList<rfm::core::RemoteSelection> selection = selectedEntries();
    if (selection.isEmpty()) {
        return;
    }
    QString destination;
    if (sourcePane->source() == rfm::core::FileSource::Local) {
        destination =
            QFileDialog::getExistingDirectory(this, tr("Select the copy destination folder"),
                                              sourcePane->currentPath(), QFileDialog::ShowDirsOnly);
    } else {
        destination = askDestination(tr("Copy selected items"));
    }
    if (destination.isEmpty()) {
        return;
    }
    if (sourcePane->source() == rfm::core::FileSource::Local) {
        startLocalOperation(rfm::core::LocalFileOperationKind::Copy, sourcePaneId, sourcePaneId,
                            sourcePane->currentPath(), destination, selection);
    } else {
        startRemoteTransfer(rfm::core::InternalTransferAction::Copy,
                            transferPayload(sourcePaneId, selection), 0, destination);
    }
}

void MainWindow::moveSelectedToOtherPane()
{
    FileBrowserPane* const sourcePane = m_workspaceTabs->activePane();
    const quint64 sourcePaneId = m_workspaceTabs->paneId(sourcePane);
    const QList<rfm::core::RemoteSelection> selection = sourcePane->selectedEntries();
    FileBrowserPane* const destinationPane = m_workspaceTabs->otherVisiblePane(sourcePaneId);
    const quint64 destinationPaneId = m_workspaceTabs->paneId(destinationPane);
    const QString sourceDirectory = sourcePane->currentPath();
    const QString destination =
        destinationPane != nullptr ? destinationPane->currentPath() : QString{};
    if (selection.isEmpty() || sourcePaneId == 0 || destinationPaneId == 0 ||
        sourcePaneId == destinationPaneId || destinationPane == nullptr ||
        destinationPane->isHidden() || destination.isEmpty()) {
        return;
    }
    if (sourcePane->source() == rfm::core::FileSource::Local) {
        if (destinationPane->source() != rfm::core::FileSource::Local) {
            return;
        }
        startLocalOperation(rfm::core::LocalFileOperationKind::Move, sourcePaneId,
                            destinationPaneId, sourceDirectory, destination, selection);
        return;
    }
    if (rfm::core::RemotePath::normalize(sourceDirectory) ==
        rfm::core::RemotePath::normalize(destination)) {
        statusBar()->showMessage(tr("Source and destination folders are identical."), 8000);
        return;
    }
    if (!pathsUseSameConvention(sourceDirectory, destination)) {
        statusBar()->showMessage(
            tr("Source and destination use incompatible remote path conventions."), 8000);
        return;
    }
    if (!confirmOtherPaneOperation(tr("Move"), selection, destination)) {
        return;
    }
    startRemoteTransfer(rfm::core::InternalTransferAction::Move,
                        transferPayload(sourcePaneId, selection), destinationPaneId, destination);
}

void MainWindow::copySelectedToOtherPane()
{
    FileBrowserPane* const sourcePane = m_workspaceTabs->activePane();
    const quint64 sourcePaneId = m_workspaceTabs->paneId(sourcePane);
    const QList<rfm::core::RemoteSelection> selection = sourcePane->selectedEntries();
    FileBrowserPane* const destinationPane = m_workspaceTabs->otherVisiblePane(sourcePaneId);
    const quint64 destinationPaneId = m_workspaceTabs->paneId(destinationPane);
    const QString sourceDirectory = sourcePane->currentPath();
    const QString destination =
        destinationPane != nullptr ? destinationPane->currentPath() : QString{};
    if (selection.isEmpty() || sourcePaneId == 0 || destinationPaneId == 0 ||
        sourcePaneId == destinationPaneId || destinationPane == nullptr ||
        destinationPane->isHidden() || destination.isEmpty()) {
        return;
    }
    const rfm::core::InternalTransferPayload payload = transferPayload(sourcePaneId, selection);
    if (sourcePane->source() != destinationPane->source()) {
        startCrossSourceTransfer(rfm::core::InternalTransferAction::Copy, payload,
                                 destinationPaneId, destination);
        return;
    }
    if (sourcePane->source() == rfm::core::FileSource::Local) {
        startLocalOperation(rfm::core::LocalFileOperationKind::Copy, sourcePaneId,
                            destinationPaneId, sourceDirectory, destination, selection);
        return;
    }
    if (rfm::core::RemotePath::normalize(sourceDirectory) ==
        rfm::core::RemotePath::normalize(destination)) {
        statusBar()->showMessage(tr("Source and destination folders are identical."), 8000);
        return;
    }
    if (!pathsUseSameConvention(sourceDirectory, destination)) {
        statusBar()->showMessage(
            tr("Source and destination use incompatible remote path conventions."), 8000);
        return;
    }
    if (!confirmOtherPaneOperation(tr("Copy"), selection, destination)) {
        return;
    }
    startRemoteTransfer(rfm::core::InternalTransferAction::Copy, payload, destinationPaneId,
                        destination);
}

void MainWindow::copySelectionToClipboard()
{
    FileBrowserPane* const pane = m_workspaceTabs->activePane();
    const QList<rfm::core::RemoteSelection> selection = pane->selectedEntries();
    if (selection.isEmpty() || pane->source() == rfm::core::FileSource::None) {
        return;
    }
    const quint64 paneId = m_workspaceTabs->paneId(pane);
    m_internalClipboard.set(rfm::core::InternalTransferAction::Copy,
                            transferPayload(paneId, selection));
    updateCutAppearance();
    updateOperationActions();
    statusBar()->showMessage(tr("%1 item(s) copied").arg(selection.size()), 3000);
}

void MainWindow::cutSelectionToClipboard()
{
    FileBrowserPane* const pane = m_workspaceTabs->activePane();
    const QList<rfm::core::RemoteSelection> selection = pane->selectedEntries();
    if (selection.isEmpty() || pane->source() == rfm::core::FileSource::None) {
        return;
    }
    const quint64 paneId = m_workspaceTabs->paneId(pane);
    m_internalClipboard.set(rfm::core::InternalTransferAction::Move,
                            transferPayload(paneId, selection));
    updateCutAppearance();
    updateOperationActions();
    statusBar()->showMessage(tr("%1 item(s) ready to move").arg(selection.size()), 3000);
}

void MainWindow::pasteClipboard()
{
    if (!m_internalClipboard.content().has_value()) {
        return;
    }
    const rfm::core::ClipboardEntry entry = *m_internalClipboard.content();
    FileBrowserPane* const destinationPane = m_workspaceTabs->activePane();
    const quint64 destinationPaneId = m_workspaceTabs->paneId(destinationPane);
    const rfm::core::InternalTransferValidation validation = rfm::core::validateInternalTransfer(
        entry.payload, m_applicationInstanceId, destinationPane->currentLocation(),
        destinationPane->source() == rfm::core::FileSource::Ssh
            ? currentConnectionIdentity()
            : rfm::core::RemoteConnectionIdentity{});
    FileBrowserPane* const sourcePane = m_workspaceTabs->pane(entry.payload.sourcePaneId);
    const bool panesMatchPayload =
        sourcePane != nullptr && sourcePane->source() == entry.payload.source &&
        sourcePane->currentLocation().machineId == entry.payload.sourceMachineId;
    if (!validation.accepted() || !panesMatchPayload) {
        statusBar()->showMessage(transferValidationMessage(
                                     !panesMatchPayload
                                         ? rfm::core::InternalTransferValidationError::InvalidSource
                                         : validation.error),
                                 8000);
        return;
    }
    if (entry.payload.source == rfm::core::FileSource::Local) {
        startLocalOperation(entry.action == rfm::core::InternalTransferAction::Move
                                ? rfm::core::LocalFileOperationKind::Move
                                : rfm::core::LocalFileOperationKind::Copy,
                            entry.payload.sourcePaneId, destinationPaneId,
                            QFileInfo(entry.payload.sources.constFirst().path).absolutePath(),
                            destinationPane->currentPath(), entry.payload.sources,
                            rfm::core::LocalCollisionPolicy::Fail,
                            entry.action == rfm::core::InternalTransferAction::Move
                                ? std::optional<quint64>{m_internalClipboard.generation()}
                                : std::nullopt);
        return;
    }
    startRemoteTransfer(entry.action, entry.payload, destinationPaneId,
                        destinationPane->currentPath(),
                        entry.action == rfm::core::InternalTransferAction::Move);
}

void MainWindow::cancelPendingCut()
{
    if (!m_internalClipboard.isCut()) {
        return;
    }
    clearInternalClipboard();
    statusBar()->showMessage(tr("Pending move cancelled"), 3000);
}

bool MainWindow::startLocalOperation(rfm::core::LocalFileOperationKind kind, quint64 sourcePaneId,
                                     quint64 destinationPaneId, const QString& sourceDirectory,
                                     const QString& destinationDirectory,
                                     const QList<rfm::core::RemoteSelection>& sources,
                                     rfm::core::LocalCollisionPolicy collisionPolicy,
                                     std::optional<quint64> clipboardGeneration)
{
    FileBrowserPane* const sourcePane = m_workspaceTabs->pane(sourcePaneId);
    FileBrowserPane* const destinationPane = m_workspaceTabs->pane(destinationPaneId);
    if (sourcePane == nullptr || destinationPane == nullptr || sources.isEmpty() ||
        sourcePane->source() != rfm::core::FileSource::Local ||
        destinationPane->source() != rfm::core::FileSource::Local || sourceDirectory.isEmpty() ||
        destinationDirectory.isEmpty() || !QFileInfo(sourceDirectory).isDir() ||
        !QFileInfo(destinationDirectory).isDir() || !QFileInfo(destinationDirectory).isWritable()) {
        statusBar()->showMessage(tr("The local destination folder is invalid."), 8000);
        return false;
    }
    const quint64 id = nextOperationId();
    QStringList paths;
    for (const auto& source : sources) {
        paths.push_back(source.path);
    }
    const rfm::core::LocalFileOperationRequest request{
        id, kind, sourceDirectory, paths, {}, destinationDirectory, collisionPolicy};
    m_localOperationContexts.insert(
        id, {sourcePaneId, destinationPaneId, sourceDirectory, destinationDirectory});
    m_localOperationRequests.insert(id, request);
    rfm::core::OperationProgress progress;
    progress.id = id;
    progress.kind = kind == rfm::core::LocalFileOperationKind::Move
                        ? rfm::core::OperationKind::LocalMove
                        : rfm::core::OperationKind::LocalCopy;
    progress.state = rfm::core::OperationState::Queued;
    progress.sources = paths;
    progress.destination = destinationDirectory;
    progress.totalItems = static_cast<quint64>(paths.size());
    progress.cancellationSupported = true;
    updateTrackedOperation(progress);
    if (clipboardGeneration.has_value()) {
        m_localClipboardMoveOperations.insert(id, *clipboardGeneration);
    }
    setPaneBusy(sourcePaneId, true,
                kind == rfm::core::LocalFileOperationKind::Move
                    ? tr("Moving %1 item(s)…").arg(paths.size())
                    : tr("Copying %1 item(s)…").arg(paths.size()));
    emit localFileOperationRequested(request);
    return true;
}

void MainWindow::selectAllInActivePane()
{
    if (m_workspaceTabs->activePane()->hasLocation()) {
        m_workspaceTabs->activePane()->fileTable()->selectAll();
    }
}

void MainWindow::focusActiveLocation()
{
    if (m_workspaceTabs->activePane()->hasLocation()) {
        m_workspaceTabs->activePane()->focusLocation();
    }
}

std::optional<rfm::core::InternalTransferAction> MainWindow::chooseCrossFilesystemTransferAction()
{
    QMessageBox choice(QMessageBox::Question, tr("Cross-filesystem file operation"),
                       tr("The source and destination are on different filesystems. "
                          "Choose the operation to perform."),
                       QMessageBox::NoButton, this);
    QPushButton* const copyButton = choice.addButton(tr("Copy"), QMessageBox::AcceptRole);
    QPushButton* const moveButton = choice.addButton(tr("Move"), QMessageBox::AcceptRole);
    QAbstractButton* const cancelButton = choice.addButton(QMessageBox::Cancel);
    copyButton->setObjectName(QStringLiteral("crossFilesystemDropCopyButton"));
    moveButton->setObjectName(QStringLiteral("crossFilesystemDropMoveButton"));
    cancelButton->setObjectName(QStringLiteral("crossFilesystemDropCancelButton"));
    choice.setDefaultButton(copyButton);
    choice.exec();
    if (choice.clickedButton() == copyButton) {
        return rfm::core::InternalTransferAction::Copy;
    }
    if (choice.clickedButton() == moveButton) {
        return rfm::core::InternalTransferAction::Move;
    }
    return std::nullopt;
}

void MainWindow::handleInternalDrop(rfm::core::InternalTransferPayload payload,
                                    rfm::core::InternalTransferAction action,
                                    quint64 destinationPaneId, QString destinationDirectory,
                                    bool actionWasExplicitlyRequested)
{
    FileBrowserPane* const destinationPane = m_workspaceTabs->pane(destinationPaneId);
    FileBrowserPane* const sourcePane = m_workspaceTabs->pane(payload.sourcePaneId);
    rfm::core::BrowserLocation destinationLocation = destinationPane == nullptr
                                                         ? rfm::core::BrowserLocation{}
                                                         : destinationPane->currentLocation();
    destinationLocation.path = std::move(destinationDirectory);
    const rfm::core::RemoteConnectionIdentity destinationConnection =
        (payload.source == rfm::core::FileSource::Ssh ||
         destinationLocation.source == rfm::core::FileSource::Ssh)
            ? currentConnectionIdentity()
            : rfm::core::RemoteConnectionIdentity{};
    const rfm::core::InternalTransferValidation validation = rfm::core::validateInternalTransfer(
        payload, m_applicationInstanceId, destinationLocation, destinationConnection,
        rfm::core::InternalTransferCompatibility::AllowLocalAndSsh);
    const bool panesMatchPayload =
        sourcePane != nullptr && sourcePane->source() == payload.source &&
        sourcePane->currentLocation().machineId == payload.sourceMachineId;
    if (!validation.accepted() || !panesMatchPayload || destinationPane == nullptr ||
        destinationPane->isHidden()) {
        rfm::core::InternalTransferValidationError error = validation.error;
        if (validation.accepted()) {
            error = !panesMatchPayload
                        ? rfm::core::InternalTransferValidationError::InvalidSource
                        : rfm::core::InternalTransferValidationError::InvalidDestination;
        }
        statusBar()->showMessage(transferValidationMessage(error), 8000);
        return;
    }

    if (payload.source != destinationLocation.source) {
        startCrossSourceTransfer(action, payload, destinationPaneId, destinationLocation.path);
        return;
    }

    if (payload.source == rfm::core::FileSource::Local) {
        const QString firstDestination =
            QDir(destinationLocation.path)
                .filePath(QFileInfo(payload.sources.constFirst().path).fileName());
        if (!actionWasExplicitlyRequested &&
            rfm::core::LocalFileSystem::pathsUseDifferentFileSystems(
                payload.sources.constFirst().path, firstDestination)) {
            const auto selectedAction = chooseCrossFilesystemTransferAction();
            if (!selectedAction.has_value()) {
                return;
            }
            action = *selectedAction;
        }
        startLocalOperation(action == rfm::core::InternalTransferAction::Move
                                ? rfm::core::LocalFileOperationKind::Move
                                : rfm::core::LocalFileOperationKind::Copy,
                            payload.sourcePaneId, destinationPaneId, sourcePane->currentPath(),
                            destinationLocation.path, payload.sources);
        return;
    }
    if (!actionWasExplicitlyRequested && action == rfm::core::InternalTransferAction::Copy) {
        startRemoteFilesystemPreflight(std::move(payload), destinationPaneId,
                                       destinationLocation.path);
        return;
    }
    startRemoteTransfer(action, payload, destinationPaneId, destinationLocation.path);
}

void MainWindow::handleRemoteFilesystemRelation(quint64 requestId,
                                                rfm::core::RemoteFilesystemRelation relation)
{
    const auto pending = m_pendingRemoteFilesystemPreflights.find(requestId);
    if (pending == m_pendingRemoteFilesystemPreflights.end()) {
        return;
    }
    const PendingRemoteFilesystemPreflight request = pending.value();
    m_pendingRemoteFilesystemPreflights.erase(pending);
    if (!m_connected || request.connection != currentConnectionIdentity()) {
        return;
    }
    rfm::core::InternalTransferAction action = rfm::core::InternalTransferAction::Copy;
    if (relation == rfm::core::RemoteFilesystemRelation::Different) {
        const auto selectedAction = chooseCrossFilesystemTransferAction();
        if (!selectedAction.has_value()) {
            return;
        }
        action = *selectedAction;
    }
    startRemoteTransfer(action, request.payload, request.destinationPaneId,
                        request.destinationDirectory);
}

void MainWindow::removeSelectedEntries()
{
    const QList<rfm::core::RemoteSelection> selection = selectedEntries();
    if (selection.isEmpty()) {
        return;
    }
    QStringList names;
    bool recursive = false;
    const FileBrowserPane* const pane = m_workspaceTabs->activePane();
    const bool local = pane->source() == rfm::core::FileSource::Local;
    const bool remote = pane->source() == rfm::core::FileSource::Ssh && m_connected &&
                        pane->currentLocation().machineId == activeRemoteMachineId();
    if (!local && !remote) {
        return;
    }
    for (const auto& item : selection) {
        names.push_back(local ? QFileInfo(item.path).fileName()
                              : rfm::core::RemotePath::fileName(item.path));
        recursive = recursive || item.directory;
    }
    const QString warning =
        recursive
            ? tr("The selected folders and all their contents will be permanently deleted.\n\n%1")
                  .arg(names.join(QChar{'\n'}))
            : tr("The selected files will be permanently deleted.\n\n%1")
                  .arg(names.join(QChar{'\n'}));
    if (QMessageBox::warning(this, tr("Confirm permanent deletion"), warning,
                             QMessageBox::Yes | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    const quint64 id = nextOperationId();
    const quint64 paneId = m_workspaceTabs->paneId(pane);
    if (local) {
        QStringList paths;
        paths.reserve(selection.size());
        for (const rfm::core::RemoteSelection& item : selection) {
            paths.push_back(item.path);
        }
        m_localOperationContexts.insert(id, {paneId, 0, pane->currentPath(), {}});
        setPaneBusy(paneId, true, tr("Deleting %1 item(s)…").arg(selection.size()));
        emit localFileOperationRequested(
            {id, rfm::core::LocalFileOperationKind::Remove, pane->currentPath(), paths, {}});
        return;
    }
    setBusy(true, tr("Deleting %1 item(s)…").arg(selection.size()));
    m_operationContexts.insert(id, {paneId, 0, pane->currentPath(), {}});
    emit removeRequested(id, selection, recursive);
}

void MainWindow::queueTransferRequest(const rfm::core::TransferRequest& request, quint64 paneId)
{
    m_transferPanes.insert(request.id, paneId);
    TransferRefreshContext refreshContext;
    refreshContext.connection = currentConnectionIdentity();
    if (request.direction == rfm::core::TransferDirection::Upload) {
        refreshContext.destination = {
            rfm::core::FileSource::Ssh, activeRemoteMachineId(),
            rfm::core::RemotePath::normalize(rfm::core::RemotePath::parent(request.destination))};
    } else {
        refreshContext.destination = {
            rfm::core::FileSource::Local, QString::fromLatin1(rfm::core::LocalMachineId),
            QDir::cleanPath(QFileInfo(request.destination).absolutePath())};
    }
    m_transferRefreshContexts.insert(request.id, std::move(refreshContext));
    m_pendingTransferRequests.insert(request.id);
    m_nonTerminalTransfers.insert(request.id);
    updateConnectionAction();
    emit transferRequested(request);
}

void MainWindow::updateTrackedOperation(rfm::core::OperationProgress operation)
{
    const auto existing = m_operations.constFind(operation.id);
    const bool localOperation = operation.kind == rfm::core::OperationKind::LocalCopy ||
                                operation.kind == rfm::core::OperationKind::LocalMove;
    if (localOperation) {
        operation.serverHost.clear();
        operation.serverPort = 0;
    }
    operation.serverHost = operation.serverHost.trimmed();
    if (!localOperation && (operation.serverHost.isEmpty() || operation.serverPort == 0) &&
        existing != m_operations.cend() && !existing->serverHost.isEmpty() &&
        existing->serverPort != 0) {
        operation.serverHost = existing->serverHost;
        operation.serverPort = existing->serverPort;
    }
    if (!localOperation && (operation.serverHost.isEmpty() || operation.serverPort == 0)) {
        operation.serverHost = m_activeProfile.host.trimmed();
        operation.serverPort = m_activeProfile.port;
    }
    if (rfm::core::isTerminal(operation.state)) {
        if (!operation.finishedAt.isValid()) {
            operation.finishedAt = existing != m_operations.cend() && existing->finishedAt.isValid()
                                       ? existing->finishedAt
                                       : QDateTime::currentDateTimeUtc();
        }
    } else {
        operation.finishedAt = {};
    }
    m_operations.insert(operation.id, operation);
    m_operationPanel->updateOperation(operation);
    if (rfm::core::isTerminal(operation.state)) {
        scheduleOperationHistorySave();
    }
}

void MainWindow::loadOperationHistory()
{
    const QList<rfm::core::OperationProgress> operations = m_operationHistoryStore->load();
    for (const rfm::core::OperationProgress& operation : operations) {
        m_operations.insert(operation.id, operation);
        if (operation.id >= m_nextOperationId &&
            operation.id != std::numeric_limits<quint64>::max()) {
            m_nextOperationId = operation.id + 1;
        }
    }
    m_operationPanel->restoreOperations(operations);
}

void MainWindow::scheduleOperationHistorySave() { m_historySaveTimer->start(); }

void MainWindow::saveOperationHistory()
{
    QString error;
    if (!m_operationHistoryStore->save(m_operations.values(), &error)) {
        statusBar()->showMessage(error, 8000);
    }
}

void MainWindow::removeTerminalOperation(quint64 id)
{
    const auto operation = m_operations.constFind(id);
    if (operation == m_operations.cend() || !rfm::core::isTerminal(operation->state) ||
        !m_operationPanel->removeTerminalOperation(id)) {
        return;
    }
    m_operations.remove(id);
    scheduleOperationHistorySave();
}

void MainWindow::clearTerminalOperations()
{
    for (auto iterator = m_operations.begin(); iterator != m_operations.end();) {
        if (rfm::core::isTerminal(iterator->state)) {
            iterator = m_operations.erase(iterator);
        } else {
            ++iterator;
        }
    }
    m_operationPanel->clearTerminalOperations();
    scheduleOperationHistorySave();
}

void MainWindow::handleOperationResult(const rfm::core::RemoteOperationResult& result)
{
    const OperationContext context = m_operationContexts.take(result.id);
    const bool trackedRemoteOperation = result.kind == rfm::core::RemoteOperationKind::Copy ||
                                        result.kind == rfm::core::RemoteOperationKind::Move;
    bool operationCancelled = false;
    if (trackedRemoteOperation) {
        const rfm::core::OperationProgress started = m_remoteOperations.take(result.id);
        operationCancelled = started.state == rfm::core::OperationState::Cancelled;
        if (!rfm::core::isTerminal(started.state)) {
            const rfm::core::OperationProgress finished =
                rfm::core::finishRemoteOperation(result, started);
            operationCancelled = finished.state == rfm::core::OperationState::Cancelled;
            updateTrackedOperation(finished);
        }
    }
    const bool suppressDialog = m_silentRemoteOperationResults.remove(result.id) > 0;
    const auto clipboardMove = m_clipboardMoveOperations.find(result.id);
    const std::optional<quint64> clipboardGeneration =
        clipboardMove == m_clipboardMoveOperations.end()
            ? std::nullopt
            : std::optional<quint64>{clipboardMove.value()};
    if (clipboardMove != m_clipboardMoveOperations.end()) {
        m_clipboardMoveOperations.erase(clipboardMove);
    }
    if (clipboardGeneration.has_value() && result.allSucceeded() &&
        m_internalClipboard.hasContent() &&
        m_internalClipboard.generation() == *clipboardGeneration) {
        clearInternalClipboard();
    }
    QStringList failures;
    bool anySuccess = false;
    for (const rfm::core::RemoteItemResult& item : result.items) {
        if (!item.success) {
            const QString label = !item.source.isEmpty() && !item.destination.isEmpty()
                                      ? tr("%1 → %2").arg(item.source, item.destination)
                                      : (item.source.isEmpty() ? item.destination : item.source);
            failures.push_back(tr("%1: %2").arg(label, item.error));
        }
        anySuccess = anySuccess || item.success;
    }
    if (!failures.isEmpty() && !suppressDialog) {
        QMessageBox::warning(this,
                             operationCancelled ? tr("Remote operation cancelled")
                                                : tr("Remote operation incomplete"),
                             failures.join(QChar{'\n'}));
    }
    statusBar()->showMessage(
        operationCancelled ? tr("Remote operation cancelled")
                           : (failures.isEmpty() ? tr("Remote operation completed")
                                                 : tr("Remote operation completed with errors")));
    if (!trackedRemoteOperation) {
        setBusy(false);
    } else {
        updateConnectionAction();
    }
    if (anySuccess) {
        QSet<QString> affectedPaths;
        QHash<QString, QStringList> selectionsByPath;
        if (result.kind == rfm::core::RemoteOperationKind::Copy &&
            !context.destinationDirectory.isEmpty()) {
            affectedPaths.insert(rfm::core::RemotePath::normalize(context.destinationDirectory));
        } else if (result.kind == rfm::core::RemoteOperationKind::Move) {
            if (!context.sourceDirectory.isEmpty()) {
                affectedPaths.insert(rfm::core::RemotePath::normalize(context.sourceDirectory));
            }
            if (!context.destinationDirectory.isEmpty()) {
                affectedPaths.insert(
                    rfm::core::RemotePath::normalize(context.destinationDirectory));
            }
        } else if (!context.sourceDirectory.isEmpty()) {
            affectedPaths.insert(rfm::core::RemotePath::normalize(context.sourceDirectory));
        }
        for (const rfm::core::RemoteItemResult& item : result.items) {
            if (!item.success) {
                continue;
            }
            const QString sourceParent = rfm::core::RemotePath::parent(item.source);
            const QString destinationParent = rfm::core::RemotePath::parent(item.destination);
            if (result.kind == rfm::core::RemoteOperationKind::Move ||
                result.kind == rfm::core::RemoteOperationKind::Rename ||
                result.kind == rfm::core::RemoteOperationKind::Remove) {
                affectedPaths.insert(rfm::core::RemotePath::normalize(sourceParent));
            }
            if (result.kind == rfm::core::RemoteOperationKind::Copy ||
                result.kind == rfm::core::RemoteOperationKind::Move ||
                result.kind == rfm::core::RemoteOperationKind::Rename ||
                result.kind == rfm::core::RemoteOperationKind::CreateDirectory) {
                affectedPaths.insert(rfm::core::RemotePath::normalize(destinationParent));
                if (!item.destination.isEmpty()) {
                    selectionsByPath[rfm::core::RemotePath::normalize(destinationParent)].push_back(
                        rfm::core::RemotePath::fileName(item.destination));
                }
            }
        }
        for (const quint64 paneId : m_workspaceTabs->openPaneIds()) {
            FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
            const QString normalizedPath = rfm::core::RemotePath::normalize(pane->currentPath());
            if (selectionsByPath.contains(normalizedPath)) {
                pane->setPendingSelectionNames(selectionsByPath.value(normalizedPath));
            }
        }
        scheduleVisiblePanesForPaths(affectedPaths, true);
    }
}

void MainWindow::handleLocalFileOperationStarted(quint64 id)
{
    if (!m_localOperationRequests.contains(id)) {
        return;
    }
    auto operation = m_operations.value(id);
    operation.state = rfm::core::OperationState::Running;
    updateTrackedOperation(operation);
}

void MainWindow::handleLocalFileOperationResult(const rfm::core::LocalFileOperationResult& result)
{
    const OperationContext context = m_localOperationContexts.value(result.id);
    if (!m_localOperationContexts.contains(result.id)) {
        return;
    }

    const bool trackedCopyMove = m_localOperationRequests.contains(result.id);
    QList<rfm::core::LocalFileOperationItemResult> collisions;
    if (trackedCopyMove) {
        QList<rfm::core::LocalFileOperationItemResult>& accumulated =
            m_localOperationItems[result.id];
        for (const auto& item : result.items) {
            for (auto iterator = accumulated.begin(); iterator != accumulated.end(); ++iterator) {
                if (iterator->source == item.source &&
                    iterator->outcome == rfm::core::LocalFileOperationOutcome::Collision) {
                    iterator = accumulated.erase(iterator);
                    break;
                }
            }
            accumulated.push_back(item);
        }
        for (const auto& item : accumulated) {
            if (item.outcome == rfm::core::LocalFileOperationOutcome::Collision) {
                collisions.push_back(item);
            }
        }
    }
    if (!collisions.isEmpty()) {
        QMessageBox choice(QMessageBox::Question, tr("Destination already exists"),
                           tr("An item already exists at:\n%1\n\nChoose how to continue.")
                               .arg(collisions.constFirst().destination),
                           QMessageBox::NoButton, this);
        QPushButton* const replace = choice.addButton(tr("Replace"), QMessageBox::DestructiveRole);
        QPushButton* const skip = choice.addButton(tr("Skip"), QMessageBox::AcceptRole);
        QPushButton* const cancel = choice.addButton(QMessageBox::Cancel);
        choice.setDefaultButton(cancel);
        choice.exec();
        if (choice.clickedButton() != cancel) {
            auto request = m_localOperationRequests.value(result.id);
            request.sourcePaths.clear();
            for (const auto& item : collisions) {
                request.sourcePaths.push_back(item.source);
            }
            request.collisionPolicy =
                choice.clickedButton() == replace
                    ? rfm::core::LocalCollisionPolicy::Overwrite
                    : (choice.clickedButton() == skip ? rfm::core::LocalCollisionPolicy::Skip
                                                      : rfm::core::LocalCollisionPolicy::Cancel);
            m_localOperationRequests.insert(result.id, request);
            auto progress = m_operations.value(result.id);
            progress.state = rfm::core::OperationState::Running;
            progress.error.clear();
            updateTrackedOperation(progress);
            emit localFileOperationRequested(request);
            return;
        }
        auto operation = m_operations.value(result.id);
        operation.state = rfm::core::OperationState::Cancelled;
        operation.cancellationSupported = false;
        operation.error = tr("Collision resolution cancelled.");
        updateTrackedOperation(operation);
    }

    m_localOperationContexts.remove(result.id);
    m_localOperationRequests.remove(result.id);
    const QList<rfm::core::LocalFileOperationItemResult> allItems =
        trackedCopyMove ? m_localOperationItems.take(result.id) : result.items;
    setPaneBusy(context.sourcePaneId, false);

    QStringList failures;
    QStringList warnings;
    bool anySuccess = false;
    QStringList selectionNames;
    for (const rfm::core::LocalFileOperationItemResult& item : allItems) {
        if (item.success) {
            anySuccess = true;
            if (!item.error.isEmpty()) {
                warnings.push_back(item.error);
            }
            if (!item.destination.isEmpty() &&
                (result.kind == rfm::core::LocalFileOperationKind::CreateDirectory ||
                 result.kind == rfm::core::LocalFileOperationKind::Rename)) {
                selectionNames.push_back(QFileInfo(item.destination).fileName());
            }
            continue;
        }
        if (item.outcome == rfm::core::LocalFileOperationOutcome::Skipped) {
            continue;
        }
        const QString label = !item.source.isEmpty() && !item.destination.isEmpty()
                                  ? tr("%1 → %2").arg(item.source, item.destination)
                                  : (item.source.isEmpty() ? item.destination : item.source);
        failures.push_back(label.isEmpty() ? item.error : tr("%1: %2").arg(label, item.error));
    }
    if (trackedCopyMove) {
        rfm::core::OperationProgress operation = m_operations.value(result.id);
        const bool collisionCancelled = operation.state == rfm::core::OperationState::Cancelled;
        operation.state = (result.cancelled || collisionCancelled)
                              ? rfm::core::OperationState::Cancelled
                              : (failures.isEmpty() ? rfm::core::OperationState::Completed
                                                    : rfm::core::OperationState::Failed);
        operation.completedItems = static_cast<quint64>(
            std::ranges::count(allItems, true, &rfm::core::LocalFileOperationItemResult::success));
        operation.totalItems = static_cast<quint64>(allItems.size());
        operation.error = (warnings + failures).join(QChar{'\n'});
        operation.cancellationSupported = false;
        updateTrackedOperation(operation);
        const auto clipboardMove = m_localClipboardMoveOperations.find(result.id);
        const bool consumedClipboard =
            clipboardMove != m_localClipboardMoveOperations.end() &&
            operation.state == rfm::core::OperationState::Completed &&
            m_internalClipboard.matchesCutGeneration(clipboardMove.value());
        m_localClipboardMoveOperations.remove(result.id);
        if (consumedClipboard) {
            clearInternalClipboard();
        }
    }
    statusBar()->showMessage(
        !failures.isEmpty() ? tr("Local operation completed with errors")
                            : (!warnings.isEmpty() ? tr("Local operation completed with warnings")
                                                   : tr("Local operation completed")),
        5000);
    if (!anySuccess || context.sourceDirectory.isEmpty()) {
        return;
    }

    requestLocalTreeDirectoryRefresh(context.sourceDirectory);
    if (context.destinationDirectory != context.sourceDirectory) {
        requestLocalTreeDirectoryRefresh(context.destinationDirectory);
    }
    QSet<QString> directories{context.sourceDirectory, context.destinationDirectory};
    for (const quint64 paneId : m_workspaceTabs->openPaneIds()) {
        FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
        bool affected = false;
        for (const QString& directory : directories) {
            affected =
                affected || (rfm::core::localPathIsAtOrBelow(pane->currentPath(), directory) &&
                             rfm::core::localPathIsAtOrBelow(directory, pane->currentPath()));
        }
        if (pane->source() != rfm::core::FileSource::Local || !affected) {
            continue;
        }
        if (!selectionNames.isEmpty()) {
            pane->setPendingSelectionNames(selectionNames);
        }
        schedulePaneRefresh(paneId, true);
    }
}

void MainWindow::handleRemoteOperationProgress(rfm::core::OperationProgress progress)
{
    if (progress.id == 0 || (progress.kind != rfm::core::OperationKind::RemoteCopy &&
                             progress.kind != rfm::core::OperationKind::RemoteMove)) {
        return;
    }
    m_remoteOperations.insert(progress.id, progress);
    updateTrackedOperation(std::move(progress));
    updateConnectionAction();
}

void MainWindow::handleTransferProgress(const rfm::core::TransferProgress& progress)
{
    updateTrackedOperation(rfm::core::operationProgress(progress));
    m_pendingTransferRequests.remove(progress.id);
    const bool terminal = progress.state == rfm::core::TransferState::Completed ||
                          progress.state == rfm::core::TransferState::Cancelled ||
                          progress.state == rfm::core::TransferState::Failed;
    if (terminal) {
        m_nonTerminalTransfers.remove(progress.id);
    } else {
        m_nonTerminalTransfers.insert(progress.id);
    }
    updateConnectionAction();

    if (progress.state == rfm::core::TransferState::Completed) {
        scheduleTransferDestinationRefresh(progress.id);
    }
    if (terminal) {
        m_transferPanes.remove(progress.id);
        m_transferRefreshContexts.remove(progress.id);
    }
}

void MainWindow::scheduleTransferDestinationRefresh(quint64 transferId)
{
    const auto context = m_transferRefreshContexts.constFind(transferId);
    if (context == m_transferRefreshContexts.cend()) {
        return;
    }
    if (context->connection != currentConnectionIdentity()) {
        return;
    }
    for (const quint64 paneId : m_workspaceTabs->openPaneIds()) {
        FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
        if (pane == nullptr || pane->source() != context->destination.source) {
            continue;
        }
        const bool sameDestination =
            context->destination.source == rfm::core::FileSource::Ssh
                ? pane->currentLocation().machineId == context->destination.machineId &&
                      rfm::core::RemotePath::normalize(pane->currentPath()) ==
                          context->destination.path
                : QDir::cleanPath(pane->currentPath()) == context->destination.path;
        if (sameDestination) {
            schedulePaneRefresh(paneId, false);
            m_scheduledTransferRefreshLocations.insert(paneId, context->destination);
        }
    }
}

void MainWindow::updateConnectionAction()
{
    const bool enabled = !m_busy && !m_connecting && m_busyPanes.isEmpty() &&
                         m_nonTerminalTransfers.isEmpty() && m_remoteOperations.isEmpty();
    m_newConnectionAction->setEnabled(enabled && !m_connected);
    m_disconnectAction->setEnabled(enabled && m_connected);
}

rfm::core::RemoteConnectionIdentity MainWindow::currentConnectionIdentity() const
{
    if (!m_connected || m_connectionGeneration == 0) {
        return {};
    }
    const QString host = m_activeProfile.host.trimmed().isEmpty()
                             ? QStringLiteral("<active-session>")
                             : m_activeProfile.host.trimmed();
    return {host, m_activeProfile.port, m_connectionGeneration};
}

rfm::core::InternalTransferPayload
MainWindow::transferPayload(quint64 paneId, const QList<rfm::core::RemoteSelection>& sources) const
{
    const FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
    if (pane == nullptr) {
        return {};
    }
    const rfm::core::BrowserLocation location = pane->currentLocation();
    return {location.source,
            location.machineId,
            m_applicationInstanceId,
            location.source == rfm::core::FileSource::Ssh ? currentConnectionIdentity()
                                                          : rfm::core::RemoteConnectionIdentity{},
            paneId,
            sources};
}

void MainWindow::startRemoteFilesystemPreflight(rfm::core::InternalTransferPayload payload,
                                                quint64 destinationPaneId,
                                                QString destinationDirectory)
{
    if (payload.sources.isEmpty()) {
        return;
    }
    const QString sourceDirectory =
        rfm::core::RemotePath::parent(payload.sources.constFirst().path);
    if (sourceDirectory.isEmpty()) {
        startRemoteTransfer(rfm::core::InternalTransferAction::Copy, payload, destinationPaneId,
                            destinationDirectory);
        return;
    }
    // A browser-pane drag normally contains siblings from one displayed directory. Do not rely
    // on that UI invariant for a decoded payload: mixed source directories fall back to Copy.
    for (const rfm::core::RemoteSelection& source : payload.sources) {
        if (rfm::core::RemotePath::parent(source.path) != sourceDirectory) {
            startRemoteTransfer(rfm::core::InternalTransferAction::Copy, payload, destinationPaneId,
                                destinationDirectory);
            return;
        }
    }
    const rfm::core::RemoteConnectionIdentity connection = currentConnectionIdentity();
    if (!connection.isValid()) {
        return;
    }
    const quint64 requestId = nextOperationId();
    m_pendingRemoteFilesystemPreflights.insert(
        requestId,
        {std::move(payload), destinationPaneId, std::move(destinationDirectory), connection});
    const auto request = m_pendingRemoteFilesystemPreflights.constFind(requestId);
    emit remoteFilesystemRelationRequested(requestId, sourceDirectory,
                                           request->destinationDirectory);
}

bool MainWindow::startRemoteTransfer(rfm::core::InternalTransferAction action,
                                     const rfm::core::InternalTransferPayload& payload,
                                     quint64 destinationPaneId, const QString& destinationDirectory,
                                     bool clipboardMove)
{
    const rfm::core::BrowserLocation destinationLocation{
        rfm::core::FileSource::Ssh, payload.sourceMachineId, destinationDirectory};
    const rfm::core::InternalTransferValidation validation = rfm::core::validateInternalTransfer(
        payload, m_applicationInstanceId, destinationLocation, currentConnectionIdentity());
    FileBrowserPane* const sourcePane = m_workspaceTabs->pane(payload.sourcePaneId);
    FileBrowserPane* const destinationPane =
        destinationPaneId == 0 ? nullptr : m_workspaceTabs->pane(destinationPaneId);
    if (!validation.accepted() || !m_connected || m_busy || sourcePane == nullptr ||
        sourcePane->source() != rfm::core::FileSource::Ssh ||
        sourcePane->currentLocation().machineId != activeRemoteMachineId() ||
        (destinationPaneId != 0 &&
         (destinationPane == nullptr || destinationPane->isHidden() ||
          destinationPane->source() != rfm::core::FileSource::Ssh ||
          destinationPane->currentLocation().machineId != activeRemoteMachineId()))) {
        statusBar()->showMessage(
            transferValidationMessage(
                validation.accepted()
                    ? rfm::core::InternalTransferValidationError::InvalidDestination
                    : validation.error),
            8000);
        return false;
    }

    const bool move = action == rfm::core::InternalTransferAction::Move;
    const quint64 id = nextOperationId();
    const QString sourceDirectory =
        payload.sources.isEmpty()
            ? QString{}
            : rfm::core::RemotePath::parent(payload.sources.constFirst().path);
    m_operationContexts.insert(
        id, {payload.sourcePaneId, destinationPaneId, sourceDirectory, destinationDirectory});
    if (clipboardMove) {
        m_clipboardMoveOperations.insert(id, m_internalClipboard.generation());
    }
    emit remoteOperationRequested(
        {id, move ? rfm::core::RemoteOperationKind::Move : rfm::core::RemoteOperationKind::Copy,
         payload.sources, destinationDirectory});
    if (move) {
        emit moveRequested(id, payload.sources, destinationDirectory);
    } else {
        emit copyRequested(id, payload.sources, destinationDirectory);
    }
    return true;
}

bool MainWindow::startCrossSourceTransfer(rfm::core::InternalTransferAction action,
                                          const rfm::core::InternalTransferPayload& payload,
                                          quint64 destinationPaneId,
                                          const QString& destinationDirectory)
{
    if (action == rfm::core::InternalTransferAction::Move) {
        statusBar()->showMessage(tr("Moving between local and SSH locations is not supported yet."),
                                 8000);
        return false;
    }

    FileBrowserPane* const sourcePane = m_workspaceTabs->pane(payload.sourcePaneId);
    FileBrowserPane* const destinationPane = m_workspaceTabs->pane(destinationPaneId);
    const bool sourceIsCurrent = sourcePane != nullptr && sourcePane->source() == payload.source &&
                                 sourcePane->currentLocation().machineId == payload.sourceMachineId;
    const bool destinationIsCurrent =
        destinationPane != nullptr && !destinationPane->isHidden() &&
        ((payload.source == rfm::core::FileSource::Local &&
          destinationPane->source() == rfm::core::FileSource::Ssh &&
          destinationPane->currentLocation().machineId == activeRemoteMachineId()) ||
         (payload.source == rfm::core::FileSource::Ssh &&
          destinationPane->source() == rfm::core::FileSource::Local &&
          destinationPane->currentLocation().machineId ==
              QString::fromLatin1(rfm::core::LocalMachineId)));
    if (!m_connected || !sourceIsCurrent || !destinationIsCurrent ||
        (payload.source == rfm::core::FileSource::Ssh &&
         payload.sourceMachineId != activeRemoteMachineId())) {
        statusBar()->showMessage(tr("The source or destination session is no longer available."),
                                 8000);
        return false;
    }

    if (payload.source == rfm::core::FileSource::Local) {
        for (const rfm::core::RemoteSelection& source : payload.sources) {
            const auto request = TransferRequestFactory::upload(nextOperationId(), source.path,
                                                                destinationDirectory);
            if (request.has_value()) {
                queueTransferRequest(*request, destinationPaneId);
            }
        }
        return true;
    }

    if (payload.source == rfm::core::FileSource::Ssh) {
        for (const rfm::core::RemoteSelection& source : payload.sources) {
            QString error;
            const auto request = TransferRequestFactory::download(nextOperationId(), source,
                                                                  destinationDirectory, &error);
            if (request.has_value()) {
                queueTransferRequest(*request, payload.sourcePaneId);
            } else if (!error.isEmpty()) {
                statusBar()->showMessage(error, 8000);
            }
        }
        return true;
    }
    return false;
}

QString
MainWindow::transferValidationMessage(rfm::core::InternalTransferValidationError error) const
{
    using Error = rfm::core::InternalTransferValidationError;
    switch (error) {
    case Error::ForeignApplication:
    case Error::InvalidPayload:
        return tr("Only internal RemoteFileManager drags are accepted.");
    case Error::InvalidSource:
        return tr("The source selection is no longer valid.");
    case Error::IncompatibleSource:
        return tr("The source and destination locations are incompatible.");
    case Error::IncompatibleConnection:
        return tr("The source belongs to a different or expired server session.");
    case Error::InvalidDestination:
        return tr("The destination folder is invalid.");
    case Error::IncompatiblePathConvention:
        return tr("Source and destination use incompatible remote path conventions.");
    case Error::IdenticalSourceAndDestination:
        return tr("Source and destination are identical.");
    case Error::DestinationInsideSource:
        return tr("A folder cannot be copied or moved inside itself.");
    case Error::None:
        return {};
    }
    return tr("The destination folder is invalid.");
}

void MainWindow::updatePaneTransferContexts()
{
    const rfm::core::RemoteConnectionIdentity identity = currentConnectionIdentity();
    for (const quint64 paneId : m_workspaceTabs->paneIds()) {
        FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
        if (pane->source() == rfm::core::FileSource::Local &&
            pane->currentLocation().machineId == QString::fromLatin1(rfm::core::LocalMachineId)) {
            pane->setTransferContext(m_applicationInstanceId, identity, paneId);
        } else if (identity.isValid() && pane->source() == rfm::core::FileSource::Ssh &&
                   pane->currentLocation().machineId == activeRemoteMachineId()) {
            pane->setTransferContext(m_applicationInstanceId, identity, paneId);
        } else {
            pane->clearTransferContext();
        }
    }
}

void MainWindow::updateCutAppearance()
{
    for (const quint64 paneId : m_workspaceTabs->paneIds()) {
        m_workspaceTabs->pane(paneId)->setCutPaths({});
    }
    if (!m_internalClipboard.isCut() || !m_internalClipboard.content().has_value()) {
        return;
    }
    const rfm::core::InternalTransferPayload& payload = m_internalClipboard.content()->payload;
    FileBrowserPane* const sourcePane = m_workspaceTabs->pane(payload.sourcePaneId);
    if (sourcePane == nullptr || sourcePane->source() != payload.source ||
        sourcePane->currentLocation().machineId != payload.sourceMachineId ||
        (sourcePane->source() == rfm::core::FileSource::Ssh &&
         payload.connection != currentConnectionIdentity()) ||
        sourcePane->source() == rfm::core::FileSource::None) {
        return;
    }
    QSet<QString> paths;
    for (const rfm::core::RemoteSelection& source : payload.sources) {
        paths.insert(source.path);
    }
    sourcePane->setCutPaths(std::move(paths));
}

void MainWindow::clearInternalClipboard()
{
    m_internalClipboard.clear();
    m_clipboardMoveOperations.clear();
    updateCutAppearance();
    updateOperationActions();
}

void MainWindow::requestDirectoryListing(quint64 paneId, const QString& path, bool showBusy,
                                         bool coalesceIfPending, PaneNavigation navigation,
                                         bool showStatusMessage)
{
    FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
    if (!m_connected || pane == nullptr || pane->isHidden() || path.isEmpty()) {
        return;
    }
    if (m_busy && !showBusy) {
        return;
    }

    bool paneHasPendingRequest = false;
    for (const DirectoryRequest& request : std::as_const(m_directoryRequests)) {
        if (request.paneId == paneId) {
            paneHasPendingRequest = true;
            break;
        }
    }
    if (!coalesceIfPending && paneHasPendingRequest) {
        return;
    }

    if (coalesceIfPending) {
        QQueue<quint64> retained;
        while (!m_directoryQueue.isEmpty()) {
            const quint64 queuedId = m_directoryQueue.dequeue();
            if (m_directoryRequests.value(queuedId).paneId == paneId) {
                m_directoryRequests.remove(queuedId);
            } else {
                retained.enqueue(queuedId);
            }
        }
        m_directoryQueue = std::move(retained);
    }

    const quint64 navigationGeneration = beginPaneNavigation(paneId, rfm::core::FileSource::Ssh);
    const quint64 requestId = nextOperationId();
    DirectoryRequest request;
    request.id = requestId;
    request.paneId = paneId;
    request.path = path;
    request.navigation = navigation;
    request.connectionGeneration = m_connectionGeneration;
    request.navigationGeneration = navigationGeneration;
    m_directoryRequests.insert(requestId, std::move(request));
    m_expectedDirectoryRequests.insert(paneId, requestId);
    m_directoryQueue.enqueue(requestId);
    if (showBusy) {
        setPaneBusy(paneId, true,
                    showStatusMessage ? listingStatusMessage(path, navigation) : QString{},
                    showStatusMessage ? ListingStatusMessageTimeoutMs : 0);
    }
    startNextDirectoryListing();
}

void MainWindow::requestLocalDirectoryListing(quint64 paneId, const QString& path, bool showBusy,
                                              PaneNavigation navigation, bool treeRequest,
                                              bool showStatusMessage)
{
    FileBrowserPane* const pane = paneId == 0 ? nullptr : m_workspaceTabs->pane(paneId);
    if (path.isEmpty() || (!treeRequest && (pane == nullptr || pane->isHidden()))) {
        return;
    }
    if (paneId != 0) {
        const quint64 previousRequest = m_expectedLocalDirectoryRequests.value(paneId);
        if (previousRequest != 0) {
            m_localDirectoryRequests.remove(previousRequest);
        }
    }
    const quint64 navigationGeneration =
        treeRequest ? 0 : beginPaneNavigation(paneId, rfm::core::FileSource::Local);
    if (treeRequest) {
        for (auto iterator = m_localDirectoryRequests.begin();
             iterator != m_localDirectoryRequests.end();) {
            if (iterator->treeRequest && rfm::core::localPathIsAtOrBelow(iterator->path, path) &&
                rfm::core::localPathIsAtOrBelow(path, iterator->path)) {
                iterator = m_localDirectoryRequests.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    const quint64 requestId = nextOperationId();
    m_localDirectoryRequests.insert(requestId,
                                    {paneId, navigation, treeRequest, navigationGeneration, path});
    if (paneId != 0) {
        m_expectedLocalDirectoryRequests.insert(paneId, requestId);
        if (showBusy) {
            setPaneBusy(paneId, true,
                        showStatusMessage ? listingStatusMessage(path, navigation) : QString{},
                        showStatusMessage ? ListingStatusMessageTimeoutMs : 0);
        }
    }
    emit localDirectoryRequested(requestId, path);
}

void MainWindow::requestLocalTreeDirectoryRefresh(const QString& path)
{
    if (m_navigationTree->hasLoadedLocalDirectory(path)) {
        requestLocalDirectoryListing(0, path, false, PaneNavigation::Refresh, true);
    }
}

void MainWindow::requestRemoteTreeDirectory(const QString& profileId, const QString& path)
{
    if (!m_connected || profileId != activeRemoteMachineId() || path.isEmpty()) {
        return;
    }
    const quint64 requestId = nextOperationId();
    DirectoryRequest request;
    request.id = requestId;
    request.path = path;
    request.treeRequest = true;
    request.profileId = profileId;
    request.connectionGeneration = m_connectionGeneration;
    m_directoryRequests.insert(requestId, request);
    m_directoryQueue.enqueue(requestId);
    startNextDirectoryListing();
}

void MainWindow::requestLocationListing(quint64 paneId, const rfm::core::BrowserLocation& location,
                                        bool showBusy, PaneNavigation navigation,
                                        bool showStatusMessage)
{
    if (location.source == rfm::core::FileSource::Local &&
        location.machineId == QString::fromLatin1(rfm::core::LocalMachineId)) {
        requestLocalDirectoryListing(paneId, location.path, showBusy, navigation, false,
                                     showStatusMessage);
    } else if (location.source == rfm::core::FileSource::Ssh && m_connected &&
               location.machineId == activeRemoteMachineId()) {
        requestDirectoryListing(paneId, location.path, showBusy, true, navigation,
                                showStatusMessage);
    }
}

void MainWindow::handleLocalDirectoryListed(quint64 requestId, const QString& path,
                                            const QList<rfm::core::RemoteEntry>& entries)
{
    const auto requestIterator = m_localDirectoryRequests.find(requestId);
    if (requestIterator == m_localDirectoryRequests.end()) {
        return;
    }
    const LocalDirectoryRequest request = requestIterator.value();
    m_localDirectoryRequests.erase(requestIterator);
    if (request.treeRequest) {
        m_navigationTree->setLocalDirectory(path, entries);
        return;
    }
    if (m_expectedLocalDirectoryRequests.value(request.paneId) != requestId ||
        !isExpectedPaneNavigation(request.paneId, request.navigationGeneration,
                                  rfm::core::FileSource::Local)) {
        return;
    }
    m_expectedLocalDirectoryRequests.remove(request.paneId);
    FileBrowserPane* const pane = m_workspaceTabs->pane(request.paneId);
    if (pane == nullptr || pane->isHidden()) {
        return;
    }
    if (m_navigationTree->hasLoadedLocalDirectory(path)) {
        m_navigationTree->setLocalDirectory(path, entries);
    }
    const rfm::core::BrowserLocation location{rfm::core::FileSource::Local,
                                              QString::fromLatin1(rfm::core::LocalMachineId), path};
    pane->showDirectory(location, QUrl::fromLocalFile(path).toDisplayString(), entries,
                        request.navigation);
    pane->setTransferContext(m_applicationInstanceId, {}, request.paneId);
    setPaneBusy(request.paneId, false);
    m_centralStack->setCurrentWidget(m_workspaceTabs);
    updateNavigationActions();
    updateOperationActions();
}

void MainWindow::handleLocalDirectoryListingError(quint64 requestId, const QString& path,
                                                  const QString& error)
{
    const auto requestIterator = m_localDirectoryRequests.find(requestId);
    if (requestIterator == m_localDirectoryRequests.end()) {
        return;
    }
    const LocalDirectoryRequest request = requestIterator.value();
    m_localDirectoryRequests.erase(requestIterator);
    if (request.treeRequest) {
        m_navigationTree->setDirectoryError(true, {}, path, error);
        return;
    }
    if (m_expectedLocalDirectoryRequests.value(request.paneId) == requestId &&
        isExpectedPaneNavigation(request.paneId, request.navigationGeneration,
                                 rfm::core::FileSource::Local)) {
        m_expectedLocalDirectoryRequests.remove(request.paneId);
        setPaneBusy(request.paneId, false);
        statusBar()->showMessage(tr("Unable to list local folder %1: %2").arg(path, error), 8000);
    }
}

void MainWindow::openLocalLocation(const QString& path)
{
    const quint64 paneId = m_workspaceTabs->paneId(m_workspaceTabs->activePane());
    requestLocalDirectoryListing(paneId, path, true, PaneNavigation::Normal);
}

void MainWindow::beginVolumeOperation(const rfm::core::StorageVolume& volume,
                                      rfm::core::VolumeOperation operation)
{
    const QString device = QDir::cleanPath(volume.device.trimmed());
    if (!device.startsWith(QStringLiteral("/dev/")) ||
        (operation == rfm::core::VolumeOperation::Mount && volume.mounted) ||
        (operation == rfm::core::VolumeOperation::Unmount &&
         (!volume.mounted || QDir::cleanPath(volume.rootPath) == QStringLiteral("/") ||
          volume.kind == rfm::core::StorageKind::System))) {
        return;
    }
    for (const rfm::core::VolumeOperationRequest& active : std::as_const(m_volumeOperations)) {
        if (QDir::cleanPath(active.target.device.trimmed()) == device) {
            return;
        }
    }

    const quint64 id = nextOperationId();
    const rfm::core::VolumeOperationRequest request{
        id,
        operation,
        {device, volume.rootPath, volume.kind,
         knownMountPointsForDevice(m_localStorageVolumes, device)}};
    m_volumeOperations.insert(id, request);
    m_navigationTree->setLocalVolumeOperation(device, operation);
    emit volumeOperationRequested(request);
}

void MainWindow::handleVolumeOperationResult(const rfm::core::VolumeOperationResult& result)
{
    const auto request = m_volumeOperations.constFind(result.id);
    if (request == m_volumeOperations.cend() || request->operation != result.operation ||
        QDir::cleanPath(request->target.device.trimmed()) !=
            QDir::cleanPath(result.device.trimmed())) {
        return;
    }

    if (!result.succeeded()) {
        const QString device = request->target.device;
        m_volumeOperations.remove(result.id);
        m_navigationTree->setLocalVolumeOperation(device, std::nullopt);
        statusBar()->showMessage(volumeOperationErrorMessage(result), 8000);
        if (result.error == rfm::core::VolumeOperationError::DeviceNotFound) {
            if (m_localStorageRefreshPending) {
                m_localStorageRefreshAfterCurrent = true;
            } else {
                refreshStorage();
            }
        }
        return;
    }

    if (result.operation == rfm::core::VolumeOperation::Unmount) {
        evacuateLocalPanesFromMountPoint(request->target.mountPoint);
    }
    m_volumeOperationsAwaitingRefresh.insert(result.id);
    if (m_localStorageRefreshPending) {
        // The in-flight snapshot may predate the command. Discard it and force
        // an enumeration whose start is known to follow the successful result.
        m_localStorageRefreshAfterCurrent = true;
    } else {
        refreshStorage();
    }
}

void MainWindow::beginRemoteVolumeOperation(const QString& machineId,
                                            const rfm::core::StorageVolume& volume,
                                            rfm::core::VolumeOperation operation)
{
    if (!m_connected || machineId != activeRemoteMachineId() ||
        !rfm::core::isSafeLinuxDevicePath(volume.device) ||
        (operation == rfm::core::VolumeOperation::Mount && volume.mounted) ||
        (operation == rfm::core::VolumeOperation::Unmount &&
         (!volume.mounted ||
          rfm::core::RemotePath::normalize(volume.rootPath) == QStringLiteral("/") ||
          volume.kind == rfm::core::StorageKind::System))) {
        return;
    }
    for (const RemoteVolumeOperationContext& active : std::as_const(m_remoteVolumeOperations)) {
        if (active.machineId == machineId && active.request.target.device == volume.device) {
            return;
        }
    }

    const quint64 id = nextOperationId();
    const rfm::core::VolumeOperationRequest request{
        id,
        operation,
        {volume.device, volume.rootPath, volume.kind,
         knownMountPointsForDevice(m_remoteStorageVolumes, volume.device)}};
    m_remoteVolumeOperations.insert(id, {request, machineId, m_connectionGeneration, 0, nullptr});
    m_navigationTree->setVolumeOperation(machineId, volume.device, operation);
    emit remoteVolumeOperationRequested(request);
}

void MainWindow::handleRemoteVolumeOperationResult(const rfm::core::VolumeOperationResult& result)
{
    const auto context = m_remoteVolumeOperations.constFind(result.id);
    if (context == m_remoteVolumeOperations.cend() ||
        context->request.operation != result.operation ||
        context->request.target.device != result.device) {
        return;
    }
    const RemoteVolumeOperationContext completed = *context;
    if (completed.connectionGeneration != m_connectionGeneration ||
        completed.machineId != activeRemoteMachineId()) {
        m_remoteVolumeOperations.remove(result.id);
        return;
    }

    if (result.error == rfm::core::VolumeOperationError::AuthenticationRequired &&
        result.authenticationToken != 0) {
        showRemoteVolumeAuthentication(result);
        return;
    }

    if (!result.succeeded()) {
        m_remoteVolumeOperations.remove(result.id);
        m_navigationTree->setVolumeOperation(completed.machineId, completed.request.target.device,
                                             std::nullopt);
        statusBar()->showMessage(volumeOperationErrorMessage(result), 8000);
        if (result.error == rfm::core::VolumeOperationError::DeviceNotFound && m_connected) {
            if (m_remoteStorageRefreshPending) {
                m_remoteStorageRefreshAfterCurrent = true;
            } else {
                refreshStorage();
            }
        }
        return;
    }

    if (result.operation == rfm::core::VolumeOperation::Unmount) {
        evacuateRemotePanesFromMountPoint(completed.machineId, completed.request.target.mountPoint);
    }
    m_remoteVolumeOperationsAwaitingRefresh.insert(result.id);
    if (m_remoteStorageRefreshPending) {
        m_remoteStorageRefreshAfterCurrent = true;
    } else {
        refreshStorage();
    }
}

void MainWindow::showRemoteVolumeAuthentication(const rfm::core::VolumeOperationResult& result)
{
    auto context = m_remoteVolumeOperations.find(result.id);
    if (context == m_remoteVolumeOperations.end() || result.authenticationToken == 0 ||
        context->authenticationDialog != nullptr || !m_connected ||
        context->connectionGeneration != m_connectionGeneration ||
        context->machineId != activeRemoteMachineId()) {
        return;
    }

    auto* const dialog = new VolumeAuthenticationDialog(m_activeProfile.host.trimmed(),
                                                        context->request.target.device,
                                                        context->request.operation, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    context->authenticationToken = result.authenticationToken;
    context->authenticationDialog = dialog;
    const quint64 operationId = result.id;
    const quint64 authenticationToken = result.authenticationToken;
    connect(dialog, &QDialog::accepted, this, [this, dialog, operationId, authenticationToken] {
        rfm::core::SecurePassword password = dialog->takePassword();
        auto current = m_remoteVolumeOperations.find(operationId);
        const bool valid = current != m_remoteVolumeOperations.end() && m_connected &&
                           current->connectionGeneration == m_connectionGeneration &&
                           current->machineId == activeRemoteMachineId() &&
                           current->authenticationToken == authenticationToken &&
                           current->authenticationDialog == dialog;
        if (current != m_remoteVolumeOperations.end()) {
            current->authenticationDialog = nullptr;
        }
        if (valid && !password.isEmpty()) {
            m_sshSession->postVolumeAuthentication(operationId, authenticationToken,
                                                   std::move(password));
        }
    });
    connect(dialog, &QDialog::rejected, this, [this, dialog, operationId, authenticationToken] {
        auto current = m_remoteVolumeOperations.find(operationId);
        if (current == m_remoteVolumeOperations.end() || current->authenticationDialog != dialog) {
            return;
        }
        if (!m_connected || current->connectionGeneration != m_connectionGeneration ||
            current->machineId != activeRemoteMachineId()) {
            current->authenticationDialog = nullptr;
            return;
        }
        const RemoteVolumeOperationContext cancelled = *current;
        m_remoteVolumeOperations.erase(current);
        m_navigationTree->setVolumeOperation(cancelled.machineId, cancelled.request.target.device,
                                             std::nullopt);
        if (cancelled.authenticationToken == authenticationToken) {
            emit remoteVolumeAuthenticationCancelled(operationId, authenticationToken);
            statusBar()->showMessage(tr("Volume authentication was cancelled."), 5000);
        }
    });
    dialog->open();
}

void MainWindow::evacuateLocalPanesFromMountPoint(const QString& mountPoint)
{
    if (mountPoint.trimmed().isEmpty()) {
        return;
    }
    QString fallbackPath = QDir::homePath();
    if (rfm::core::localPathIsAtOrBelow(fallbackPath, mountPoint)) {
        fallbackPath = QDir::rootPath();
    }
    for (const quint64 paneId : m_workspaceTabs->openPaneIds()) {
        FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
        if (pane == nullptr) {
            continue;
        }
        const rfm::core::BrowserLocation location = pane->currentLocation();
        if (location.source != rfm::core::FileSource::Local ||
            location.machineId != QString::fromLatin1(rfm::core::LocalMachineId) ||
            !rfm::core::localPathIsAtOrBelow(location.path, mountPoint)) {
            continue;
        }
        pane->removeLocalHistoryUnderPath(mountPoint);
        requestLocalDirectoryListing(paneId, fallbackPath, true, PaneNavigation::SafetyFallback);
    }
}

void MainWindow::evacuateRemotePanesFromMountPoint(const QString& machineId,
                                                   const QString& mountPoint)
{
    if (!m_connected || machineId != activeRemoteMachineId() ||
        !rfm::core::RemotePath::normalize(mountPoint).startsWith(QChar{'/'})) {
        return;
    }
    QString fallbackPath = rfm::core::RemotePath::normalize(m_remoteInitialPath);
    if (!fallbackPath.startsWith(QChar{'/'}) ||
        rfm::core::RemotePath::isAtOrBelow(fallbackPath, mountPoint)) {
        fallbackPath = QStringLiteral("/");
    }
    for (const quint64 paneId : m_workspaceTabs->openPaneIds()) {
        FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
        if (pane == nullptr) {
            continue;
        }
        const rfm::core::BrowserLocation location = pane->currentLocation();
        if (location.source != rfm::core::FileSource::Ssh || location.machineId != machineId ||
            !rfm::core::RemotePath::isAtOrBelow(location.path, mountPoint)) {
            continue;
        }
        pane->removeHistoryUnderPath(rfm::core::FileSource::Ssh, machineId, mountPoint);
        requestDirectoryListing(paneId, fallbackPath, true, false, PaneNavigation::SafetyFallback);
    }
}

QString
MainWindow::volumeOperationErrorMessage(const rfm::core::VolumeOperationResult& result) const
{
    const QString operation =
        result.operation == rfm::core::VolumeOperation::Mount ? tr("mount") : tr("unmount");
    switch (result.error) {
    case rfm::core::VolumeOperationError::NotSupported:
        return tr("This volume cannot be %1ed by RemoteFileManager.").arg(operation);
    case rfm::core::VolumeOperationError::AuthenticationRequired:
        return tr("Authentication is required to %1 %2.").arg(operation, result.device);
    case rfm::core::VolumeOperationError::AuthenticationFailed:
        return tr("Authentication failed while trying to %1 %2.").arg(operation, result.device);
    case rfm::core::VolumeOperationError::Cancelled:
        return tr("The request to %1 %2 was cancelled.").arg(operation, result.device);
    case rfm::core::VolumeOperationError::PermissionDenied:
        return tr("Permission was denied while trying to %1 %2.").arg(operation, result.device);
    case rfm::core::VolumeOperationError::DeviceNotFound:
        return tr("The volume %1 is no longer available.").arg(result.device);
    case rfm::core::VolumeOperationError::VolumeBusy:
        return tr("The volume %1 is busy. Close files using it and try again.").arg(result.device);
    case rfm::core::VolumeOperationError::ToolUnavailable:
        return tr("No supported system tool is available to %1 this volume.").arg(operation);
    case rfm::core::VolumeOperationError::ConnectionLost:
        return tr("The SSH connection was lost while trying to %1 %2.")
            .arg(operation, result.device);
    case rfm::core::VolumeOperationError::SystemError:
        return tr("The system could not %1 %2.").arg(operation, result.device);
    case rfm::core::VolumeOperationError::None:
        return {};
    }
    return tr("The volume operation failed.");
}

void MainWindow::openRemoteTreeLocation(const QString& profileId, const QString& path)
{
    if (!m_connected || profileId != activeRemoteMachineId()) {
        statusBar()->showMessage(tr("Connect this server before browsing its files."), 5000);
        return;
    }
    const quint64 paneId = m_workspaceTabs->paneId(m_workspaceTabs->activePane());
    requestDirectoryListing(paneId, path, true, true, PaneNavigation::Normal);
}

void MainWindow::startNextDirectoryListing()
{
    if (m_activeDirectoryRequestId != 0) {
        return;
    }
    while (!m_directoryQueue.isEmpty()) {
        const quint64 requestId = m_directoryQueue.dequeue();
        const auto request = m_directoryRequests.constFind(requestId);
        if (request == m_directoryRequests.cend()) {
            continue;
        }
        m_activeDirectoryRequestId = requestId;
        emit directoryRequested(requestId, request->path);
        return;
    }
}

quint64 MainWindow::beginPaneNavigation(quint64 paneId, rfm::core::FileSource source)
{
    const quint64 generation = m_paneNavigationGenerations.value(paneId) + 1;
    m_paneNavigationGenerations.insert(paneId, generation);
    m_expectedPaneSources.insert(paneId, source);
    return generation;
}

bool MainWindow::isExpectedPaneNavigation(quint64 paneId, quint64 navigationGeneration,
                                          rfm::core::FileSource source,
                                          quint64 connectionGeneration) const
{
    return navigationGeneration != 0 &&
           m_paneNavigationGenerations.value(paneId) == navigationGeneration &&
           m_expectedPaneSources.value(paneId, rfm::core::FileSource::None) == source &&
           (source != rfm::core::FileSource::Ssh ||
            (m_connected && connectionGeneration == m_connectionGeneration));
}

void MainWindow::removePaneContexts(quint64 paneId)
{
    cancelDirectoryRequests(paneId);
    // Retain the active request ID until its reply releases the serialized SSH queue.
    m_directoryRequests.removeIf(
        [paneId](const auto& entry) { return entry.value().paneId == paneId; });
    m_localDirectoryRequests.removeIf(
        [paneId](const auto& entry) { return entry.value().paneId == paneId; });
    m_directoryCountRequests.removeIf(
        [paneId](const auto& entry) { return entry.value().paneId == paneId; });
    m_expectedLocalDirectoryRequests.remove(paneId);
    m_paneNavigationGenerations.remove(paneId);
    m_expectedPaneSources.remove(paneId);
    m_scheduledPaneRefreshes.remove(paneId);
    m_scheduledTransferRefreshLocations.remove(paneId);
    m_busyPanes.remove(paneId);
    m_transferPanes.removeIf([paneId](const auto& entry) { return entry.value() == paneId; });
    m_pendingRemoteFilesystemPreflights.removeIf([paneId](const auto& entry) {
        return entry.value().destinationPaneId == paneId ||
               entry.value().payload.sourcePaneId == paneId;
    });
    // Keep operation paths and tracking alive; only detach their UI endpoints.
    for (auto* const contexts : {&m_operationContexts, &m_localOperationContexts}) {
        for (auto& context : *contexts) {
            if (context.sourcePaneId == paneId) {
                context.sourcePaneId = 0;
            }
            if (context.destinationPaneId == paneId) {
                context.destinationPaneId = 0;
            }
        }
    }
    if (m_internalClipboard.content().has_value() &&
        m_internalClipboard.content()->payload.sourcePaneId == paneId) {
        clearInternalClipboard();
    }
    updateNavigationActions();
    updateOperationActions();
}

void MainWindow::cancelDirectoryRequests(quint64 paneId)
{
    m_expectedDirectoryRequests.remove(paneId);
    QQueue<quint64> retained;
    while (!m_directoryQueue.isEmpty()) {
        const quint64 requestId = m_directoryQueue.dequeue();
        if (m_directoryRequests.value(requestId).paneId == paneId) {
            m_directoryRequests.remove(requestId);
        } else {
            retained.enqueue(requestId);
        }
    }
    m_directoryQueue = std::move(retained);
    const quint64 localRequestId = m_expectedLocalDirectoryRequests.take(paneId);
    if (localRequestId != 0) {
        m_localDirectoryRequests.remove(localRequestId);
    }
    setPaneBusy(paneId, false);
}

QString MainWindow::listingStatusMessage(const QString& path, PaneNavigation navigation) const
{
    if (navigation == PaneNavigation::Refresh) {
        return tr("Refreshing %1…").arg(path);
    }
    return tr("Opening folder %1…").arg(path);
}

void MainWindow::setPaneBusy(quint64 paneId, bool busy, const QString& message, int messageTimeout)
{
    FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
    if (pane == nullptr) {
        return;
    }
    if (busy) {
        m_busyPanes.insert(paneId);
    } else {
        m_busyPanes.remove(paneId);
    }
    pane->setInteractionEnabled(!busy && !m_busy);
    updateNavigationActions();
    if (busy && !message.isEmpty()) {
        statusBar()->showMessage(message, messageTimeout);
    }
    updateConnectionAction();
    updateOperationActions();
}

void MainWindow::schedulePaneRefresh(quint64 paneId, bool showBusy)
{
    FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
    if (pane == nullptr || pane->isHidden() || pane->currentPath().isEmpty()) {
        return;
    }
    m_scheduledTransferRefreshLocations.remove(paneId);
    m_scheduledPaneRefreshes[paneId] = m_scheduledPaneRefreshes.value(paneId) || showBusy;
    m_refreshDebounceTimer->start();
}

void MainWindow::scheduleVisiblePanesForPaths(const QSet<QString>& paths, bool showBusy)
{
    for (const quint64 paneId : m_workspaceTabs->openPaneIds()) {
        FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
        if (pane->source() == rfm::core::FileSource::Ssh &&
            paths.contains(rfm::core::RemotePath::normalize(pane->currentPath()))) {
            schedulePaneRefresh(paneId, showBusy);
        }
    }
}

bool MainWindow::confirmOtherPaneOperation(const QString& operation,
                                           const QList<rfm::core::RemoteSelection>& sources,
                                           const QString& destination)
{
    QStringList sourcePaths;
    sourcePaths.reserve(sources.size());
    for (const rfm::core::RemoteSelection& source : sources) {
        sourcePaths.push_back(source.path);
    }
    return QMessageBox::question(
               this, tr("%1 to other pane").arg(operation),
               tr("%1 the following remote item(s):\n\n%2\n\nDestination folder:\n%3")
                   .arg(operation, sourcePaths.join(QChar{'\n'}), destination),
               QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

void MainWindow::stopAutomaticRefresh()
{
    if (m_refreshDebounceTimer != nullptr) {
        m_refreshDebounceTimer->stop();
    }
    m_directoryRequests.clear();
    m_directoryQueue.clear();
    m_expectedDirectoryRequests.clear();
    for (const DirectoryCountRequest& request : std::as_const(m_directoryCountRequests)) {
        if (FileBrowserPane* const pane = m_workspaceTabs->pane(request.paneId); pane != nullptr) {
            pane->setDirectoryItemCount(request.location, request.generation, request.name,
                                        std::nullopt);
        }
    }
    m_directoryCountRequests.clear();
    m_expectedLocalDirectoryRequests.clear();
    m_paneNavigationGenerations.clear();
    m_expectedPaneSources.clear();
    m_activeDirectoryRequestId = 0;
    m_busyPanes.clear();
    m_scheduledPaneRefreshes.clear();
    m_scheduledTransferRefreshLocations.clear();
}

void MainWindow::updateOperationActions()
{
    const qsizetype count = selectedEntries().size();
    const quint64 paneId = m_workspaceTabs->paneId(m_workspaceTabs->activePane());
    const FileBrowserPane* const activePane = m_workspaceTabs->activePane();
    const bool locationAvailable =
        activePane->hasLocation() && !m_busy && !m_busyPanes.contains(paneId);
    const QFileInfo localDirectory(activePane->currentPath());
    const bool localSourceAvailable = locationAvailable &&
                                      activePane->source() == rfm::core::FileSource::Local &&
                                      localDirectory.isDir() && localDirectory.isReadable();
    const bool localMutationAvailable = localSourceAvailable && localDirectory.isWritable();
    const bool remoteOperationAvailable =
        locationAvailable && m_connected && activePane->source() == rfm::core::FileSource::Ssh &&
        activePane->currentLocation().machineId == activeRemoteMachineId();
    const bool mutationAvailable = localMutationAvailable || remoteOperationAvailable;
    m_createDirectoryAction->setEnabled(mutationAvailable);
    m_renameAction->setEnabled(mutationAvailable && count == 1);
    m_moveAction->setEnabled((remoteOperationAvailable || localMutationAvailable) && count > 0);
    m_copyAction->setEnabled((remoteOperationAvailable || localSourceAvailable) && count > 0);
    FileBrowserPane* const otherPane = m_workspaceTabs->otherVisiblePane(paneId);
    const quint64 otherPaneId = m_workspaceTabs->paneId(otherPane);
    const bool distinctDirectories =
        otherPane != nullptr &&
        rfm::core::RemotePath::normalize(m_workspaceTabs->activePane()->currentPath()) !=
            rfm::core::RemotePath::normalize(otherPane->currentPath());
    const bool compatiblePathConventions =
        otherPane != nullptr && pathsUseSameConvention(m_workspaceTabs->activePane()->currentPath(),
                                                       otherPane->currentPath());
    const bool otherPaneAvailable =
        (remoteOperationAvailable || localSourceAvailable) && count > 0 && otherPane != nullptr &&
        !otherPane->currentPath().isEmpty() &&
        ((remoteOperationAvailable &&
          otherPane->currentLocation().machineId == activeRemoteMachineId()) ||
         (localSourceAvailable && otherPane->source() == rfm::core::FileSource::Local &&
          QFileInfo(otherPane->currentPath()).isWritable())) &&
        !m_busyPanes.contains(otherPaneId) && distinctDirectories && compatiblePathConventions;
    m_moveToOtherPaneAction->setEnabled(otherPaneAvailable &&
                                        (remoteOperationAvailable || localMutationAvailable));
    const bool destinationIsLocal = otherPane != nullptr &&
                                    otherPane->source() == rfm::core::FileSource::Local &&
                                    QFileInfo(otherPane->currentPath()).isDir() &&
                                    QFileInfo(otherPane->currentPath()).isWritable();
    const bool destinationIsRemote =
        otherPane != nullptr && otherPane->source() == rfm::core::FileSource::Ssh && m_connected &&
        otherPane->currentLocation().machineId == activeRemoteMachineId();
    const bool crossSourceCopyAvailable = (localSourceAvailable && destinationIsRemote) ||
                                          (remoteOperationAvailable && destinationIsLocal);
    m_copyToOtherPaneAction->setEnabled(
        otherPaneAvailable || (count > 0 && otherPane != nullptr && !otherPane->isHidden() &&
                               !otherPane->currentPath().isEmpty() &&
                               !m_busyPanes.contains(otherPaneId) && crossSourceCopyAvailable));
    m_removeAction->setEnabled(mutationAvailable && count > 0);
    m_openAction->setEnabled(locationAvailable && count == 1 &&
                             activePane->contextLocalFile().has_value());
    m_clipboardCopyAction->setEnabled((remoteOperationAvailable || localSourceAvailable) &&
                                      count > 0);
    m_clipboardCutAction->setEnabled((remoteOperationAvailable || localMutationAvailable) &&
                                     count > 0);
    bool pasteAvailable = false;
    if (m_internalClipboard.content().has_value()) {
        const auto& clipboard = *m_internalClipboard.content();
        const FileBrowserPane* const sourcePane =
            m_workspaceTabs->pane(clipboard.payload.sourcePaneId);
        const bool panesMatchPayload =
            sourcePane != nullptr && sourcePane->source() == clipboard.payload.source &&
            sourcePane->currentLocation().machineId == clipboard.payload.sourceMachineId;
        const rfm::core::RemoteConnectionIdentity destinationConnection =
            activePane->source() == rfm::core::FileSource::Ssh
                ? currentConnectionIdentity()
                : rfm::core::RemoteConnectionIdentity{};
        pasteAvailable =
            (remoteOperationAvailable || localMutationAvailable) && panesMatchPayload &&
            rfm::core::validateInternalTransfer(clipboard.payload, m_applicationInstanceId,
                                                activePane->currentLocation(),
                                                destinationConnection)
                .accepted();
    }
    m_clipboardPasteAction->setEnabled(pasteAvailable);
    m_selectAllAction->setEnabled(locationAvailable);
    m_focusLocationAction->setEnabled(locationAvailable);
    m_switchPaneAction->setEnabled(!m_busy && m_workspaceTabs->activeWorkspace()->isSplit());
    m_cancelCutAction->setEnabled(m_internalClipboard.isCut());
}

QList<rfm::core::RemoteSelection> MainWindow::selectedEntries() const
{
    return m_workspaceTabs->activePane()->selectedEntries();
}

QString MainWindow::askDestination(const QString& title)
{
    bool accepted = false;
    const QString value =
        QInputDialog::getText(this, title, tr("Remote destination folder:"), QLineEdit::Normal,
                              m_workspaceTabs->activePane()->currentPath(), &accepted);
    if (!accepted) {
        return {};
    }
    const QString normalized = rfm::core::RemotePath::normalize(value);
    if (normalized.isEmpty() || normalized == QStringLiteral("..") ||
        normalized.startsWith(QStringLiteral("../"))) {
        QMessageBox::warning(this, tr("Invalid destination"), tr("Enter a valid remote folder."));
        return {};
    }
    return normalized;
}

quint64 MainWindow::nextOperationId()
{
    while (m_nextOperationId == 0 || m_operations.contains(m_nextOperationId)) {
        ++m_nextOperationId;
    }
    const quint64 id = m_nextOperationId++;
    if (m_nextOperationId == 0) {
        m_nextOperationId = 1;
    }
    return id;
}

void MainWindow::requestParentDirectory()
{
    m_workspaceTabs->activePane()->requestParentDirectory();
}

void MainWindow::updateNavigationActions()
{
    const FileBrowserPane* const pane = m_workspaceTabs->activePane();
    const quint64 paneId = m_workspaceTabs->paneId(pane);
    const bool available = pane->hasLocation() && !m_busy && !m_busyPanes.contains(paneId);
    bool hasParent = false;
    if (available && pane->source() == rfm::core::FileSource::Local) {
        hasParent = !QDir(pane->currentPath()).isRoot();
    } else if (available && pane->source() == rfm::core::FileSource::Ssh) {
        hasParent = pane->currentPath() != QStringLiteral(".") &&
                    pane->currentPath() != QStringLiteral("/");
    }
    m_upAction->setEnabled(available && hasParent);
    m_refreshAction->setEnabled(available);
    m_backAction->setEnabled(available && pane->canGoBack());
    m_forwardAction->setEnabled(available && pane->canGoForward());
}

QString MainWindow::activeRemoteMachineId() const
{
    return m_connected ? m_activeRemoteMachineId : QString{};
}

RemoteMachineDescriptor MainWindow::activeRemoteMachine() const
{
    if (!m_connected) {
        return {};
    }
    return {activeRemoteMachineId(), m_activeProfile.effectiveDisplayName(),
            m_activeProfile.host,    m_activeProfile.username,
            m_activeProfile.port,    m_activeSavedProfileId};
}

void MainWindow::setBusy(bool busy, const QString& message)
{
    if (busy != m_busy) {
        if (busy) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
        } else {
            QApplication::restoreOverrideCursor();
        }
        m_busy = busy;
    }
    updateConnectionAction();
    updateNavigationActions();
    for (const quint64 paneId : m_workspaceTabs->openPaneIds()) {
        FileBrowserPane* const pane = m_workspaceTabs->pane(paneId);
        pane->setInteractionEnabled(pane->hasLocation() && !busy && !m_busyPanes.contains(paneId));
    }
    if (busy && !message.isEmpty()) {
        statusBar()->showMessage(message);
    }
    updateOperationActions();
}

void MainWindow::showAboutDialog()
{
    QMessageBox::about(
        this, tr("About RemoteFileManager"),
        tr("RemoteFileManager %1\n\n"
           "A native file manager for browsing, managing and transferring files "
           "locally and on remote SSH/SFTP servers.\n\n"
           "Copyright © 2026 Gabriel Albaladejo\n"
           "Licensed under GNU GPL v3.0 or later.")
            .arg(QApplication::applicationVersion()));
}

} // namespace rfm::app
