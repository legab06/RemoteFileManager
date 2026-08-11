#include "remotefilemanager/app/MainWindow.hpp"

#include "remotefilemanager/app/ConnectionDialog.hpp"
#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/app/HomePage.hpp"
#include "remotefilemanager/app/NavigationTree.hpp"
#include "remotefilemanager/app/OperationPanel.hpp"
#include "remotefilemanager/app/PaneWorkspace.hpp"
#include "remotefilemanager/app/ServerProfileDialog.hpp"
#include "remotefilemanager/app/TransferRequestFactory.hpp"
#include "remotefilemanager/core/OperationHistoryStore.hpp"
#include "remotefilemanager/core/LocalFileSystem.hpp"
#include "remotefilemanager/core/RemotePath.hpp"
#include "remotefilemanager/core/ServerProfileStore.hpp"
#include "remotefilemanager/ssh/LibsshRuntime.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDateTime>
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
#include <QStatusBar>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <limits>
#include <utility>

namespace rfm::app
{
namespace
{

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
        url.setPath(normalizedPath == QStringLiteral(".")
                        ? QStringLiteral("/~/")
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
           first.username.trimmed() == second.username.trimmed() && first.port == second.port;
}

class UploadSelectionDialog final : public QFileDialog
{
  public:
    explicit UploadSelectionDialog(QWidget* parent)
        : QFileDialog(parent, tr("Select files or folders to upload"), QDir::homePath())
    {
        setOption(QFileDialog::DontUseNativeDialog);
        setAcceptMode(QFileDialog::AcceptOpen);
        setFileMode(QFileDialog::ExistingFiles);
        setLabelText(QFileDialog::Accept, tr("Select"));
    }

    [[nodiscard]] QStringList selectedPaths() const { return m_selectedPaths; }

  protected:
    void accept() override
    {
        QStringList paths;
        for (const QString& path : selectedFiles()) {
            const QFileInfo info(path);
            if ((info.isFile() || info.isDir()) && !paths.contains(info.absoluteFilePath())) {
                paths.push_back(info.absoluteFilePath());
            }
        }
        if (paths.isEmpty()) {
            return;
        }
        m_selectedPaths = std::move(paths);
        QDialog::accept();
    }

  private:
    QStringList m_selectedPaths;
};

} // namespace

MainWindow::MainWindow(QWidget* parent, QString operationHistoryDirectory,
                       QString serverProfileDirectory)
    : QMainWindow(parent),
      m_operationHistoryStore(std::make_unique<rfm::core::OperationHistoryStore>(
          std::move(operationHistoryDirectory))),
      m_serverProfileStore(std::make_unique<rfm::core::ServerProfileStore>(
          std::move(serverProfileDirectory)))
{
    setObjectName(QStringLiteral("mainWindow"));
    m_applicationInstanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    setWindowTitle(tr("RemoteFileManager"));
    resize(1100, 700);
    setMinimumSize(760, 480);
    setUnifiedTitleAndToolBarOnMac(true);

    createPaneWorkspace();
    createActions();
    createMenus();
    createNavigationBar();
    createCentralPages();
    createPlacesDock();
    createOperationDock();

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
        for (const quint64 paneId : m_paneWorkspace->visiblePaneIds()) {
            FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
            if (pane->source() == rfm::core::FileSource::Ssh) {
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
        for (auto iterator = refreshes.cbegin(); iterator != refreshes.cend(); ++iterator) {
            if (FileBrowserPane* const pane = m_paneWorkspace->pane(iterator.key());
                pane != nullptr && !pane->isHidden()) {
                requestLocationListing(iterator.key(), pane->currentLocation(), iterator.value(),
                                       PaneNavigation::Refresh);
            }
        }
    });

    qRegisterMetaType<rfm::core::ConnectionProfile>();
    qRegisterMetaType<QList<rfm::core::RemoteEntry>>();
    qRegisterMetaType<QList<rfm::core::RemoteSelection>>();
    qRegisterMetaType<rfm::core::InternalTransferPayload>();
    qRegisterMetaType<rfm::core::RemoteOperationResult>();
    qRegisterMetaType<rfm::core::OperationProgress>();
    qRegisterMetaType<rfm::core::TransferRequest>();
    qRegisterMetaType<rfm::core::TransferProgress>();
    qRegisterMetaType<rfm::core::BrowserLocation>();
    qRegisterMetaType<QList<rfm::core::StorageVolume>>();

    m_localThread = new QThread(this);
    m_localFileSystem = new rfm::core::LocalFileSystemWorker;
    m_localFileSystem->moveToThread(m_localThread);
    connect(m_localThread, &QThread::finished, m_localFileSystem, &QObject::deleteLater);
    connect(this, &MainWindow::localDirectoryRequested, m_localFileSystem,
            &rfm::core::LocalFileSystemWorker::listDirectory);
    connect(this, &MainWindow::localVolumesRequested, m_localFileSystem,
            &rfm::core::LocalFileSystemWorker::listVolumes);
    connect(m_localFileSystem, &rfm::core::LocalFileSystemWorker::directoryListed, this,
            &MainWindow::handleLocalDirectoryListed);
    connect(m_localFileSystem, &rfm::core::LocalFileSystemWorker::directoryListingFailed, this,
            &MainWindow::handleLocalDirectoryListingError);
    connect(m_localFileSystem, &rfm::core::LocalFileSystemWorker::volumesListed, this,
            &MainWindow::handleLocalStorageVolumes);
    m_localThread->start();

    m_sshThread = new QThread(this);
    m_sshSession = new rfm::ssh::SshSession;
    m_sshSession->moveToThread(m_sshThread);
    connect(m_sshThread, &QThread::finished, m_sshSession, &QObject::deleteLater);
    connect(this, &MainWindow::connectionRequested, m_sshSession,
            &rfm::ssh::SshSession::connectToHost);
    connect(this, &MainWindow::hostKeyDecision, m_sshSession,
            &rfm::ssh::SshSession::confirmUnknownHost);
    connect(this, &MainWindow::directoryRequested, m_sshSession,
            &rfm::ssh::SshSession::listDirectory);
    connect(this, &MainWindow::remoteStorageRequested, m_sshSession,
            &rfm::ssh::SshSession::listStorageVolumes);
    connect(this, &MainWindow::createDirectoryRequested, m_sshSession,
            &rfm::ssh::SshSession::createDirectory);
    connect(this, &MainWindow::renameRequested, m_sshSession, &rfm::ssh::SshSession::renameEntry);
    connect(this, &MainWindow::moveRequested, m_sshSession, &rfm::ssh::SshSession::moveEntries);
    connect(this, &MainWindow::copyRequested, m_sshSession, &rfm::ssh::SshSession::copyEntries);
    connect(this, &MainWindow::removeRequested, m_sshSession, &rfm::ssh::SshSession::removeEntries);
    connect(this, &MainWindow::transferRequested, m_sshSession,
            &rfm::ssh::SshSession::enqueueTransfer);
    connect(this, &MainWindow::pauseTransferRequested, m_sshSession,
            &rfm::ssh::SshSession::pauseTransfer);
    connect(this, &MainWindow::resumeTransferRequested, m_sshSession,
            &rfm::ssh::SshSession::resumeTransfer);
    connect(this, &MainWindow::cancelTransferRequested, m_sshSession,
            &rfm::ssh::SshSession::cancelTransfer);
    connect(this, &MainWindow::cancelRemoteOperationRequested, m_sshSession,
            &rfm::ssh::SshSession::cancelRemoteOperation);
    connect(this, &MainWindow::shutdownRequested, m_sshSession,
            &rfm::ssh::SshSession::shutdownTransfers);
    connect(this, &MainWindow::disconnectionRequested, m_sshSession,
            &rfm::ssh::SshSession::disconnectFromHost);
    connect(m_sshSession, &rfm::ssh::SshSession::hostKeyConfirmationRequired, this,
            &MainWindow::showHostKeyConfirmation);
    connect(m_sshSession, &rfm::ssh::SshSession::connected, this, &MainWindow::handleConnected);
    connect(m_sshSession, &rfm::ssh::SshSession::directoryListed, this,
            &MainWindow::handleDirectoryListed);
    connect(m_sshSession, &rfm::ssh::SshSession::directoryListingFailed, this,
            &MainWindow::handleDirectoryListingError);
    connect(m_sshSession, &rfm::ssh::SshSession::storageVolumesListed, this,
            &MainWindow::handleRemoteStorageVolumes);
    connect(m_sshSession, &rfm::ssh::SshSession::storageVolumeListingFailed, this,
            &MainWindow::handleRemoteStorageError);
    connect(m_sshSession, &rfm::ssh::SshSession::failed, this, &MainWindow::showConnectionError);
    connect(m_sshSession, &rfm::ssh::SshSession::operationFinished, this,
            &MainWindow::handleOperationResult);
    connect(m_sshSession, &rfm::ssh::SshSession::operationUpdated, this,
            &MainWindow::handleRemoteOperationProgress);
    connect(m_sshSession, &rfm::ssh::SshSession::transferUpdated, this,
            &MainWindow::handleTransferProgress);
    connect(m_sshSession, &rfm::ssh::SshSession::transferRejected, this,
            [this](quint64 id, const QString& error) {
                if (m_pendingTransferRequests.remove(id) > 0) {
                    m_nonTerminalTransfers.remove(id);
                }
                m_transferPanes.remove(id);
                updateConnectionAction();
                statusBar()->showMessage(error, 8000);
            });
    connect(m_sshSession, &rfm::ssh::SshSession::disconnected, this,
            &MainWindow::handleDisconnected);
    m_sshThread->start();

    refreshStorage();

    statusBar()->showMessage(
        tr("Disconnected · libssh %1").arg(rfm::ssh::LibsshRuntime::version()));
}

MainWindow::~MainWindow()
{
    stopAutomaticRefresh();
    if (m_sshThread != nullptr && m_sshThread->isRunning()) {
        QEventLoop shutdownLoop;
        connect(m_sshSession, &rfm::ssh::SshSession::transfersShutdown, &shutdownLoop,
                &QEventLoop::quit, Qt::QueuedConnection);
        emit shutdownRequested();
        shutdownLoop.exec(QEventLoop::ExcludeUserInputEvents);
        m_sshThread->quit();
        m_sshThread->wait();
    }
    if (m_historySaveTimer->isActive()) {
        m_historySaveTimer->stop();
        saveOperationHistory();
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
    connect(m_createDirectoryAction, &QAction::triggered, this, &MainWindow::createRemoteDirectory);
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
    m_removeAction = new QAction(tr("Delete…"), this);
    m_removeAction->setObjectName(QStringLiteral("removeAction"));
    m_removeAction->setShortcut(QKeySequence::Delete);
    connect(m_removeAction, &QAction::triggered, this, &MainWindow::removeSelectedEntries);

    m_clipboardCopyAction = new QAction(tr("Copy"), this);
    m_clipboardCopyAction->setObjectName(QStringLiteral("clipboardCopyAction"));
    m_clipboardCopyAction->setShortcut(QKeySequence::Copy);
    connect(m_clipboardCopyAction, &QAction::triggered, this,
            &MainWindow::copySelectionToClipboard);
    m_clipboardCutAction = new QAction(tr("Cut"), this);
    m_clipboardCutAction->setObjectName(QStringLiteral("clipboardCutAction"));
    m_clipboardCutAction->setShortcut(QKeySequence::Cut);
    connect(m_clipboardCutAction, &QAction::triggered, this,
            &MainWindow::cutSelectionToClipboard);
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
    connect(m_switchPaneAction, &QAction::triggered, m_paneWorkspace,
            &PaneWorkspace::activateOtherPane);
    m_cancelCutAction = new QAction(this);
    m_cancelCutAction->setObjectName(QStringLiteral("cancelCutAction"));
    m_cancelCutAction->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(m_cancelCutAction, &QAction::triggered, this, &MainWindow::cancelPendingCut);
    addAction(m_cancelCutAction);

    const QIcon uploadIcon =
        QIcon::fromTheme(QStringLiteral("go-up"), style()->standardIcon(QStyle::SP_ArrowUp));
    m_uploadAction = new QAction(uploadIcon, tr("Upload"), this);
    m_uploadAction->setObjectName(QStringLiteral("uploadAction"));
    m_uploadAction->setToolTip(tr("Upload files or folders to the server"));
    connect(m_uploadAction, &QAction::triggered, this, &MainWindow::chooseUploads);

    const QIcon downloadIcon =
        QIcon::fromTheme(QStringLiteral("go-down"), style()->standardIcon(QStyle::SP_ArrowDown));
    m_downloadAction = new QAction(downloadIcon, tr("Download"), this);
    m_downloadAction->setObjectName(QStringLiteral("downloadAction"));
    m_downloadAction->setToolTip(tr("Download the selection to this computer"));
    connect(m_downloadAction, &QAction::triggered, this, &MainWindow::chooseDownloadDirectory);

    m_splitViewAction = new QAction(tr("Split view"), this);
    m_splitViewAction->setObjectName(QStringLiteral("splitViewAction"));
    m_splitViewAction->setCheckable(true);
    connect(m_splitViewAction, &QAction::toggled, m_paneWorkspace, &PaneWorkspace::setSplit);
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
    fileMenu->addAction(m_uploadAction);
    fileMenu->addAction(m_downloadAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_quitAction);

    QMenu* const editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(m_clipboardCopyAction);
    editMenu->addAction(m_clipboardCutAction);
    editMenu->addAction(m_clipboardPasteAction);
    editMenu->addSeparator();
    editMenu->addAction(m_selectAllAction);

    QMenu* const viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_splitViewAction);
    viewMenu->addAction(m_focusLocationAction);
    viewMenu->addAction(m_switchPaneAction);

    QMenu* const helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(m_aboutAction);
}

void MainWindow::createPaneWorkspace()
{
    m_paneWorkspace = new PaneWorkspace(this);
    connectBrowserPane(m_paneWorkspace->paneId(m_paneWorkspace->primaryPane()));
    connect(m_paneWorkspace, &PaneWorkspace::activePaneChanged, this, [this](quint64) {
        updateOperationActions();
        updateNavigationActions();
    });
    connect(m_paneWorkspace, &PaneWorkspace::paneVisibilityChanged, this,
            [this](quint64 paneId, bool visible) {
                if (!visible) {
                    cancelDirectoryRequests(paneId);
                    updateOperationActions();
                    return;
                }
                connectBrowserPane(paneId);
                FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
                if (m_connected && pane->source() == rfm::core::FileSource::Ssh) {
                    pane->setTransferContext(m_applicationInstanceId, currentConnectionIdentity(),
                                             paneId);
                }
                const FileBrowserPane* const activePane = m_paneWorkspace->activePane();
                if (!pane->hasLocation() && activePane->hasLocation()) {
                    requestLocationListing(paneId, activePane->currentLocation(), true,
                                           PaneNavigation::Initial);
                }
                updateOperationActions();
            });
}

void MainWindow::connectBrowserPane(quint64 paneId)
{
    FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
    if (pane == nullptr || pane->property("mainWindowConnected").toBool()) {
        return;
    }
    pane->setProperty("mainWindowConnected", true);
    connect(pane, &FileBrowserPane::locationNavigationRequested, this,
            [this, paneId](const rfm::core::BrowserLocation& location,
                           PaneNavigation navigation) {
                requestLocationListing(paneId, location, true, navigation);
            });
    connect(pane, &FileBrowserPane::historyChanged, this, [this, paneId, pane] {
        if (pane == m_paneWorkspace->activePane()) {
            updateNavigationActions();
        }
    });
    connect(pane, &FileBrowserPane::selectionChanged, this, [this, paneId] {
        if (paneId == m_paneWorkspace->paneId(m_paneWorkspace->activePane())) {
            updateOperationActions();
        }
    });
    connect(pane, &FileBrowserPane::contextMenuRequested, this,
            &MainWindow::showFileContextMenu);
    connect(pane, &FileBrowserPane::internalDropRequested, this,
            [this, paneId](rfm::core::InternalTransferPayload payload,
                           const QString& destination) {
                handleInternalDrop(std::move(payload), paneId, destination);
            });
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

    navigationBar->addSeparator();
    navigationBar->addAction(m_uploadAction);
    navigationBar->addAction(m_downloadAction);
    if (auto* const uploadButton =
            qobject_cast<QToolButton*>(navigationBar->widgetForAction(m_uploadAction));
        uploadButton != nullptr) {
        uploadButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    if (auto* const downloadButton =
            qobject_cast<QToolButton*>(navigationBar->widgetForAction(m_downloadAction));
        downloadButton != nullptr) {
        downloadButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }

    m_backAction->setEnabled(false);
    m_forwardAction->setEnabled(false);
    m_upAction->setEnabled(false);
    m_refreshAction->setEnabled(false);
    connect(m_backAction, &QAction::triggered, this,
            [this] { m_paneWorkspace->activePane()->requestBack(); });
    connect(m_forwardAction, &QAction::triggered, this,
            [this] { m_paneWorkspace->activePane()->requestForward(); });
    connect(m_upAction, &QAction::triggered, this, &MainWindow::requestParentDirectory);
    connect(m_refreshAction, &QAction::triggered, this, [this] {
        m_paneWorkspace->activePane()->requestRefresh();
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

    auto* const storageHeader = new QHBoxLayout;
    auto* const storageLabel = new QLabel(tr("Storage"), container);
    storageLabel->setObjectName(QStringLiteral("storageHeaderLabel"));
    m_storageRefreshAction = new QAction(
        QIcon::fromTheme(QStringLiteral("view-refresh"),
                         style()->standardIcon(QStyle::SP_BrowserReload)),
        tr("Refresh storage"), this);
    m_storageRefreshAction->setObjectName(QStringLiteral("storageRefreshAction"));
    m_storageRefreshAction->setToolTip(
        tr("Refresh local and connected-server volumes and external devices"));
    auto* const storageRefreshButton = new QToolButton(container);
    storageRefreshButton->setObjectName(QStringLiteral("storageRefreshButton"));
    storageRefreshButton->setDefaultAction(m_storageRefreshAction);
    storageRefreshButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    storageHeader->addWidget(storageLabel);
    storageHeader->addStretch();
    storageHeader->addWidget(storageRefreshButton);
    layout->addLayout(storageHeader);
    connect(m_storageRefreshAction, &QAction::triggered, this, &MainWindow::refreshStorage);

    m_navigationTree = new NavigationTree(container);
    layout->addWidget(m_navigationTree);

    auto* const buttonLayout = new QHBoxLayout;
    m_connectServerProfileButton = new QPushButton(tr("Connect"), container);
    m_connectServerProfileButton->setObjectName(QStringLiteral("connectServerProfileButton"));
    auto* const addButton = new QPushButton(tr("Add…"), container);
    addButton->setObjectName(QStringLiteral("addServerProfileButton"));
    auto* const editButton = new QPushButton(tr("Edit…"), container);
    editButton->setObjectName(QStringLiteral("editServerProfileButton"));
    auto* const removeButton = new QPushButton(tr("Remove"), container);
    removeButton->setObjectName(QStringLiteral("removeServerProfileButton"));
    m_connectServerProfileButton->setEnabled(false);
    editButton->setEnabled(false);
    removeButton->setEnabled(false);
    buttonLayout->addWidget(m_connectServerProfileButton);
    buttonLayout->addWidget(addButton);
    buttonLayout->addWidget(editButton);
    buttonLayout->addWidget(removeButton);
    layout->addLayout(buttonLayout);

    connect(m_connectServerProfileButton, &QPushButton::clicked, this,
            &MainWindow::connectToSelectedServerProfile);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addServerProfile);
    connect(editButton, &QPushButton::clicked, this, &MainWindow::editSelectedServerProfile);
    connect(removeButton, &QPushButton::clicked, this, &MainWindow::removeSelectedServerProfile);
    connect(m_navigationTree, &NavigationTree::selectedProfileChanged, this,
            [this, editButton, removeButton] {
                const bool selected = selectedServerProfile().isValidSavedProfile();
                editButton->setEnabled(selected);
                removeButton->setEnabled(selected);
                updateSelectedServerAction();
            });
    connect(m_navigationTree->tree(), &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (item != nullptr && item->data(0, Qt::UserRole).toInt() ==
                                           static_cast<int>(
                                               NavigationTree::NodeKind::ServerProfile) &&
                    selectedServerProfile().isValidSavedProfile()) {
                    connectToSelectedServerProfile();
                }
            });
    connect(m_navigationTree->tree(), &QWidget::customContextMenuRequested, this,
            &MainWindow::showServerProfileContextMenu);
    connect(m_navigationTree, &NavigationTree::localLocationActivated, this,
            &MainWindow::openLocalLocation);
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
    loadServerProfiles();
}

void MainWindow::refreshStorage()
{
    if (!m_localStorageRefreshPending) {
        m_localStorageRefreshPending = true;
        emit localVolumesRequested();
    }
    if (m_connected && !m_remoteStorageRefreshPending) {
        m_remoteStorageRefreshPending = true;
        m_remoteStorageRequestId = m_nextStorageRequestId++;
        emit remoteStorageRequested(m_remoteStorageRequestId);
    }
    updateStorageRefreshAction();
}

void MainWindow::handleLocalStorageVolumes(
    const QList<rfm::core::StorageVolume>& volumes)
{
    m_navigationTree->setStorageVolumes(volumes);
    m_localStorageRefreshPending = false;
    updateStorageRefreshAction();
}

void MainWindow::handleRemoteStorageVolumes(
    quint64 requestId, const QList<rfm::core::StorageVolume>& volumes)
{
    if (requestId == 0 || requestId != m_remoteStorageRequestId) {
        return;
    }
    m_remoteStorageRefreshPending = false;
    m_remoteStorageRequestId = 0;
    if (m_connected) {
        m_navigationTree->setRemoteStorageVolumes(activeRemoteMachineId(), volumes);
    }
    updateStorageRefreshAction();
}

void MainWindow::handleRemoteStorageError(quint64 requestId, const QString& error)
{
    if (requestId == 0 || requestId != m_remoteStorageRequestId) {
        return;
    }
    m_remoteStorageRefreshPending = false;
    m_remoteStorageRequestId = 0;
    updateStorageRefreshAction();
    if (m_connected && !error.isEmpty()) {
        statusBar()->showMessage(tr("Unable to refresh server storage: %1").arg(error), 8000);
    }
}

void MainWindow::updateStorageRefreshAction()
{
    if (m_storageRefreshAction != nullptr) {
        m_storageRefreshAction->setEnabled(!m_localStorageRefreshPending &&
                                           !m_remoteStorageRefreshPending);
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
    updateSelectedServerAction();
}

void MainWindow::updateSelectedServerAction()
{
    if (m_connectServerProfileButton == nullptr) {
        return;
    }
    const rfm::core::ConnectionProfile selected = selectedServerProfile();
    const bool valid = selected.isValidSavedProfile();
    const bool active = valid && m_connected &&
                        profilesHaveSameConnectionSettings(selected, m_activeProfile);
    m_connectServerProfileButton->setText(active ? tr("Disconnect") : tr("Connect"));
    if (!valid) {
        m_connectServerProfileButton->setEnabled(false);
        m_connectServerProfileButton->setToolTip({});
    } else if (active) {
        m_connectServerProfileButton->setEnabled(m_disconnectAction->isEnabled());
        m_connectServerProfileButton->setToolTip(tr("Disconnect from this server"));
    } else if (m_connected) {
        m_connectServerProfileButton->setEnabled(false);
        m_connectServerProfileButton->setToolTip(
            tr("Disconnect the active server before connecting to another one."));
    } else {
        m_connectServerProfileButton->setEnabled(m_newConnectionAction->isEnabled());
        m_connectServerProfileButton->setToolTip(tr("Connect to this server"));
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

void MainWindow::addServerProfile()
{
    rfm::core::ConnectionProfile initial;
    initial.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ServerProfileDialog dialog(this);
    dialog.setProfile(initial);
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

void MainWindow::editSelectedServerProfile()
{
    const rfm::core::ConnectionProfile selected = selectedServerProfile();
    if (!selected.isValidSavedProfile()) {
        return;
    }
    ServerProfileDialog dialog(this);
    dialog.setProfile(selected);
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
    const auto profile = std::ranges::find_if(m_serverProfiles, [&id](const auto& candidate) {
        return candidate.id == id;
    });
    if (profile != m_serverProfiles.cend()) {
        showConnectionDialogForProfile(*profile);
    }
}

void MainWindow::showServerProfileContextMenu(const QPoint& position)
{
    const QString id = m_navigationTree->profileIdAt(position);
    if (id.isEmpty()) {
        return;
    }
    const auto profile = std::ranges::find_if(m_serverProfiles, [&id](const auto& candidate) {
        return candidate.id == id;
    });
    if (profile == m_serverProfiles.cend() || !m_connected ||
        !profilesHaveSameConnectionSettings(*profile, m_activeProfile)) {
        return;
    }
    QMenu menu(m_navigationTree->tree());
    menu.addAction(m_disconnectAction);
    menu.exec(m_navigationTree->tree()->viewport()->mapToGlobal(position));
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
    m_operationPanel = new OperationPanel(operationDock);
    operationDock->setWidget(m_operationPanel);
    addDockWidget(Qt::BottomDockWidgetArea, operationDock);

    connect(m_operationPanel, &OperationPanel::pauseRequested, this,
            &MainWindow::pauseTransferRequested);
    connect(m_operationPanel, &OperationPanel::resumeRequested, this,
            &MainWindow::resumeTransferRequested);
    connect(m_operationPanel, &OperationPanel::cancelRequested, this, [this](quint64 id) {
        if (m_remoteOperations.contains(id)) {
            emit cancelRemoteOperationRequested(id);
        } else {
            emit cancelTransferRequested(id);
        }
    });
    connect(m_operationPanel, &OperationPanel::removeTerminalRequested, this,
            &MainWindow::removeTerminalOperation);
    connect(m_operationPanel, &OperationPanel::clearTerminalRequested, this,
            &MainWindow::clearTerminalOperations);
}

void MainWindow::createCentralPages()
{
    m_centralStack = new QStackedWidget(this);
    m_centralStack->setObjectName(QStringLiteral("centralStack"));
    m_homePage = new HomePage(m_centralStack);
    m_centralStack->addWidget(m_homePage);
    m_centralStack->addWidget(m_paneWorkspace);
    m_centralStack->setCurrentWidget(m_homePage);
    setCentralWidget(m_centralStack);

    connect(m_homePage, &HomePage::connectProfileRequested, this,
            &MainWindow::connectToServerProfile);
    connect(m_homePage, &HomePage::newConnectionRequested, m_newConnectionAction,
            &QAction::trigger);
    m_homePage->setProfiles(m_serverProfiles);
}

void MainWindow::showConnectionDialog()
{
    showConnectionDialogForProfile({});
}

void MainWindow::showConnectionDialogForProfile(
    const rfm::core::ConnectionProfile& profile)
{
    if (m_connectionDialog != nullptr) {
        m_connectionDialog->raise();
        m_connectionDialog->activateWindow();
        return;
    }

    auto* const dialog = new ConnectionDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setProfile(profile);
    m_connectionDialog = dialog;
    connect(dialog, &ConnectionDialog::connectionRequested, this, &MainWindow::beginConnection);
    dialog->open();
}

void MainWindow::beginConnection(const rfm::core::ConnectionProfile& profile,
                                 const QString& password)
{
    clearInternalClipboard();
    updatePaneTransferContexts();
    stopAutomaticRefresh();
    m_activeProfile = profile;
    setBusy(true, tr("Connecting securely to %1…").arg(m_activeProfile.host));
    emit connectionRequested(m_activeProfile, password);
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

void MainWindow::handleConnected(const QString& path,
                                 const QList<rfm::core::RemoteEntry>& entries)
{
    const QString profileSaveError = saveConnectedProfileIfRequested();
    if (m_connectionDialog != nullptr) {
        m_connectionDialog->connectionSucceeded();
    }
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
    if (m_activeRemoteMachineId.isEmpty()) {
        m_activeRemoteMachineId = QStringLiteral("ssh:%1@%2:%3")
                                      .arg(m_activeProfile.username, m_activeProfile.host,
                                           QString::number(m_activeProfile.port));
    }
    ++m_connectionGeneration;
    clearInternalClipboard();
    m_centralStack->setCurrentWidget(m_paneWorkspace);
    FileBrowserPane* const pane = m_paneWorkspace->activePane();
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
    if (m_paneWorkspace->isSplit()) {
        const quint64 activePaneId = m_paneWorkspace->paneId(pane);
        for (const quint64 paneId : m_paneWorkspace->visiblePaneIds()) {
            FileBrowserPane* const otherPane = m_paneWorkspace->pane(paneId);
            if (paneId != activePaneId &&
                (otherPane->source() == rfm::core::FileSource::None ||
                 otherPane->source() == rfm::core::FileSource::Ssh)) {
                requestDirectoryListing(paneId, path, true, true, PaneNavigation::Initial);
            }
        }
    }
}

void MainWindow::handleDirectoryListed(quint64 requestId, const QString& path,
                                       const QList<rfm::core::RemoteEntry>& entries)
{
    if (requestId == 0 || requestId != m_activeDirectoryRequestId ||
        !m_directoryRequests.contains(requestId)) {
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
    const bool expected = m_expectedDirectoryRequests.value(request.paneId) == requestId;
    FileBrowserPane* const pane = m_paneWorkspace->pane(request.paneId);
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
    if (requestId == 0 || requestId != m_activeDirectoryRequestId ||
        !m_directoryRequests.contains(requestId)) {
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
    const bool expected = m_expectedDirectoryRequests.value(request.paneId) == requestId;
    FileBrowserPane* const pane = m_paneWorkspace->pane(request.paneId);
    if (expected && pane != nullptr && !pane->isHidden()) {
        m_expectedDirectoryRequests.remove(request.paneId);
        setPaneBusy(request.paneId, false);
        statusBar()->showMessage(tr("Unable to list %1: %2").arg(path, error), 8000);
    }
    startNextDirectoryListing();
}

void MainWindow::showConnectionError(const QString& message)
{
    const bool initialConnectionAttempt =
        m_connectionDialog != nullptr &&
        m_connectionDialog->state() == ConnectionDialog::State::Connecting;
    for (auto operation : std::as_const(m_remoteOperations)) {
        operation.state = rfm::core::OperationState::Failed;
        operation.error = message;
        updateTrackedOperation(operation);
    }
    resetDisconnectedUi();
    if (initialConnectionAttempt && m_connectionDialog != nullptr) {
        m_connectionDialog->showConnectionError(message);
    } else {
        QMessageBox::critical(this, tr("SSH connection error"), message);
    }
}

void MainWindow::handleDisconnected()
{
    resetDisconnectedUi();
}

void MainWindow::resetDisconnectedUi()
{
    m_connected = false;
    m_remoteStorageRefreshPending = false;
    m_remoteStorageRequestId = 0;
    updateStorageRefreshAction();
    clearInternalClipboard();
    updatePaneTransferContexts();
    stopAutomaticRefresh();
    m_refreshDebounceTimer->stop();
    m_scheduledPaneRefreshes.clear();
    m_directoryRequests.clear();
    m_directoryQueue.clear();
    m_expectedDirectoryRequests.clear();
    m_busyPanes.clear();
    m_activeDirectoryRequestId = 0;
    m_pendingTransferRequests.clear();
    m_nonTerminalTransfers.clear();
    m_operationContexts.clear();
    m_remoteOperations.clear();
    m_transferPanes.clear();
    bool hasLocalPane = false;
    for (const quint64 paneId : m_paneWorkspace->paneIds()) {
        FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
        if (pane->source() == rfm::core::FileSource::Ssh) {
            pane->clear();
        } else {
            pane->removeHistoryForSource(rfm::core::FileSource::Ssh);
        }
        hasLocalPane = hasLocalPane || pane->source() == rfm::core::FileSource::Local;
    }
    m_centralStack->setCurrentWidget(hasLocalPane ? static_cast<QWidget*>(m_paneWorkspace)
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
    if (hasSelection) {
        menu.addAction(m_clipboardCopyAction);
        menu.addAction(m_clipboardCutAction);
    }
    menu.addAction(m_clipboardPasteAction);
    menu.addSeparator();
    if (hasSelection) {
        if (m_paneWorkspace->isSplit()) {
            menu.addAction(m_copyToOtherPaneAction);
            menu.addAction(m_moveToOtherPaneAction);
        }
        menu.addAction(m_copyAction);
        menu.addAction(m_moveAction);
        menu.addSeparator();
        menu.addAction(m_renameAction);
        menu.addAction(m_downloadAction);
        menu.addAction(m_removeAction);
        menu.addSeparator();
    }
    menu.addAction(m_createDirectoryAction);
    menu.exec(globalPosition);
}

void MainWindow::createRemoteDirectory()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("New folder"), tr("Folder name:"),
                                               QLineEdit::Normal, {}, &accepted);
    if (!accepted) {
        return;
    }
    if (!rfm::core::RemotePath::isValidName(name)) {
        QMessageBox::warning(this, tr("Invalid folder name"),
                             tr("The name must not be empty, '.', '..', or contain '/'."));
        return;
    }
    setBusy(true, tr("Creating folder %1…").arg(name));
    const quint64 id = nextOperationId();
    const quint64 paneId = m_paneWorkspace->paneId(m_paneWorkspace->activePane());
    m_operationContexts.insert(
        id, {paneId, paneId, m_paneWorkspace->activePane()->currentPath(),
             m_paneWorkspace->activePane()->currentPath()});
    emit createDirectoryRequested(id, m_paneWorkspace->activePane()->currentPath(), name);
}

void MainWindow::renameSelectedEntry()
{
    const QList<rfm::core::RemoteSelection> selection = selectedEntries();
    if (selection.size() != 1) {
        return;
    }
    const QString oldName = rfm::core::RemotePath::fileName(selection.constFirst().path);
    bool accepted = false;
    const QString newName = QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                                  QLineEdit::Normal, oldName, &accepted);
    if (!accepted) {
        return;
    }
    if (!rfm::core::RemotePath::isValidName(newName)) {
        QMessageBox::warning(this, tr("Invalid name"),
                             tr("The name must not be empty, '.', '..', or contain '/'."));
        return;
    }
    setBusy(true, tr("Renaming %1…").arg(oldName));
    const quint64 id = nextOperationId();
    const quint64 paneId = m_paneWorkspace->paneId(m_paneWorkspace->activePane());
    m_operationContexts.insert(id, {paneId, paneId, m_paneWorkspace->activePane()->currentPath(),
                                    m_paneWorkspace->activePane()->currentPath()});
    emit renameRequested(id, selection.constFirst().path, newName);
}

void MainWindow::moveSelectedEntries()
{
    FileBrowserPane* const sourcePane = m_paneWorkspace->activePane();
    const quint64 sourcePaneId = m_paneWorkspace->paneId(sourcePane);
    const QList<rfm::core::RemoteSelection> selection = selectedEntries();
    if (selection.isEmpty()) {
        return;
    }
    const QString destination = askDestination(tr("Move selected items"));
    if (destination.isEmpty()) {
        return;
    }
    startRemoteTransfer(rfm::core::InternalTransferAction::Move,
                        transferPayload(sourcePaneId, selection), 0, destination);
}

void MainWindow::copySelectedEntries()
{
    FileBrowserPane* const sourcePane = m_paneWorkspace->activePane();
    const quint64 sourcePaneId = m_paneWorkspace->paneId(sourcePane);
    const QList<rfm::core::RemoteSelection> selection = selectedEntries();
    if (selection.isEmpty()) {
        return;
    }
    const QString destination = askDestination(tr("Copy selected items"));
    if (destination.isEmpty()) {
        return;
    }
    startRemoteTransfer(rfm::core::InternalTransferAction::Copy,
                        transferPayload(sourcePaneId, selection), 0, destination);
}

void MainWindow::moveSelectedToOtherPane()
{
    FileBrowserPane* const sourcePane = m_paneWorkspace->activePane();
    const quint64 sourcePaneId = m_paneWorkspace->paneId(sourcePane);
    const QList<rfm::core::RemoteSelection> selection = sourcePane->selectedEntries();
    FileBrowserPane* const destinationPane = m_paneWorkspace->otherVisiblePane(sourcePaneId);
    const quint64 destinationPaneId = m_paneWorkspace->paneId(destinationPane);
    const QString sourceDirectory = sourcePane->currentPath();
    const QString destination =
        destinationPane != nullptr ? destinationPane->currentPath() : QString{};
    if (selection.isEmpty() || sourcePaneId == 0 || destinationPaneId == 0 ||
        sourcePaneId == destinationPaneId || destinationPane == nullptr ||
        destinationPane->isHidden() || destination.isEmpty()) {
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
    FileBrowserPane* const sourcePane = m_paneWorkspace->activePane();
    const quint64 sourcePaneId = m_paneWorkspace->paneId(sourcePane);
    const QList<rfm::core::RemoteSelection> selection = sourcePane->selectedEntries();
    FileBrowserPane* const destinationPane = m_paneWorkspace->otherVisiblePane(sourcePaneId);
    const quint64 destinationPaneId = m_paneWorkspace->paneId(destinationPane);
    const QString sourceDirectory = sourcePane->currentPath();
    const QString destination =
        destinationPane != nullptr ? destinationPane->currentPath() : QString{};
    if (selection.isEmpty() || sourcePaneId == 0 || destinationPaneId == 0 ||
        sourcePaneId == destinationPaneId || destinationPane == nullptr ||
        destinationPane->isHidden() || destination.isEmpty()) {
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
    startRemoteTransfer(rfm::core::InternalTransferAction::Copy,
                        transferPayload(sourcePaneId, selection), destinationPaneId, destination);
}

void MainWindow::copySelectionToClipboard()
{
    FileBrowserPane* const pane = m_paneWorkspace->activePane();
    const QList<rfm::core::RemoteSelection> selection = pane->selectedEntries();
    if (!m_connected || selection.isEmpty()) {
        return;
    }
    m_internalClipboard.set(rfm::core::InternalTransferAction::Copy,
                            transferPayload(m_paneWorkspace->paneId(pane), selection));
    updateCutAppearance();
    updateOperationActions();
    statusBar()->showMessage(tr("%1 remote item(s) copied").arg(selection.size()), 3000);
}

void MainWindow::cutSelectionToClipboard()
{
    FileBrowserPane* const pane = m_paneWorkspace->activePane();
    const QList<rfm::core::RemoteSelection> selection = pane->selectedEntries();
    if (!m_connected || selection.isEmpty()) {
        return;
    }
    m_internalClipboard.set(rfm::core::InternalTransferAction::Move,
                            transferPayload(m_paneWorkspace->paneId(pane), selection));
    updateCutAppearance();
    updateOperationActions();
    statusBar()->showMessage(tr("%1 remote item(s) ready to move").arg(selection.size()), 3000);
}

void MainWindow::pasteClipboard()
{
    if (!m_internalClipboard.content().has_value()) {
        return;
    }
    const rfm::core::ClipboardEntry entry = *m_internalClipboard.content();
    FileBrowserPane* const destinationPane = m_paneWorkspace->activePane();
    const quint64 destinationPaneId = m_paneWorkspace->paneId(destinationPane);
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

void MainWindow::selectAllInActivePane()
{
    if (m_paneWorkspace->activePane()->hasLocation()) {
        m_paneWorkspace->activePane()->fileTable()->selectAll();
    }
}

void MainWindow::focusActiveLocation()
{
    if (m_paneWorkspace->activePane()->hasLocation()) {
        m_paneWorkspace->activePane()->focusLocation();
    }
}

void MainWindow::handleInternalDrop(rfm::core::InternalTransferPayload payload,
                                    quint64 destinationPaneId,
                                    QString destinationDirectory)
{
    const rfm::core::InternalTransferValidation validation =
        rfm::core::validateInternalTransfer(payload, m_applicationInstanceId,
                                            currentConnectionIdentity(), destinationDirectory);
    FileBrowserPane* const destinationPane = m_paneWorkspace->pane(destinationPaneId);
    if (!validation.accepted() || destinationPane == nullptr || destinationPane->isHidden()) {
        const QString message = transferValidationMessage(
            validation.accepted() ? rfm::core::InternalTransferValidationError::InvalidDestination
                                  : validation.error);
        statusBar()->showMessage(message, 8000);
        return;
    }

    QMessageBox choice(QMessageBox::Question, tr("Remote file operation"),
                       tr("%1 remote item(s)\nDestination: %2")
                           .arg(payload.sources.size())
                           .arg(destinationDirectory),
                       QMessageBox::NoButton, this);
    QPushButton* const copyButton =
        choice.addButton(tr("Copy"), QMessageBox::AcceptRole);
    copyButton->setObjectName(QStringLiteral("dropCopyButton"));
    QPushButton* const moveButton =
        choice.addButton(tr("Move"), QMessageBox::DestructiveRole);
    moveButton->setObjectName(QStringLiteral("dropMoveButton"));
    QPushButton* const cancelButton = choice.addButton(QMessageBox::Cancel);
    cancelButton->setObjectName(QStringLiteral("dropCancelButton"));
    choice.setDefaultButton(cancelButton);
    choice.exec();
    if (choice.clickedButton() == copyButton) {
        startRemoteTransfer(rfm::core::InternalTransferAction::Copy, payload,
                            destinationPaneId, destinationDirectory);
    } else if (choice.clickedButton() == moveButton) {
        startRemoteTransfer(rfm::core::InternalTransferAction::Move, payload,
                            destinationPaneId, destinationDirectory);
    }
}

void MainWindow::removeSelectedEntries()
{
    const QList<rfm::core::RemoteSelection> selection = selectedEntries();
    if (selection.isEmpty()) {
        return;
    }
    QStringList names;
    bool recursive = false;
    for (const auto& item : selection) {
        names.push_back(rfm::core::RemotePath::fileName(item.path));
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
    setBusy(true, tr("Deleting %1 item(s)…").arg(selection.size()));
    const quint64 id = nextOperationId();
    const quint64 paneId = m_paneWorkspace->paneId(m_paneWorkspace->activePane());
    m_operationContexts.insert(id, {paneId, 0, m_paneWorkspace->activePane()->currentPath(), {}});
    emit removeRequested(id, selection, recursive);
}

void MainWindow::chooseUploads()
{
    UploadSelectionDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        queueUploads(dialog.selectedPaths());
    }
}

void MainWindow::chooseDownloadDirectory()
{
    if (selectedEntries().isEmpty()) {
        return;
    }
    const QString destination =
        QFileDialog::getExistingDirectory(this, tr("Select the download destination folder"),
                                          QDir::homePath(), QFileDialog::ShowDirsOnly);
    if (!destination.isEmpty()) {
        queueDownloads(destination);
    }
}

void MainWindow::queueUploads(QStringList localPaths)
{
    FileBrowserPane* const pane = m_paneWorkspace->activePane();
    const quint64 paneId = m_paneWorkspace->paneId(pane);
    if (!m_connected || pane->currentPath().isEmpty()) {
        return;
    }
    for (const QString& path : std::as_const(localPaths)) {
        const auto request =
            TransferRequestFactory::upload(nextOperationId(), path, pane->currentPath());
        if (request.has_value()) {
            m_transferPanes.insert(request->id, paneId);
            m_pendingTransferRequests.insert(request->id);
            m_nonTerminalTransfers.insert(request->id);
            updateConnectionAction();
            emit transferRequested(*request);
        }
    }
}

void MainWindow::queueDownloads(QString localDirectory)
{
    if (!m_connected || localDirectory.isEmpty()) {
        return;
    }
    for (const rfm::core::RemoteSelection& entry : selectedEntries()) {
        QString error;
        const auto request = TransferRequestFactory::download(nextOperationId(), entry,
                                                              localDirectory, &error);
        if (request.has_value()) {
            m_transferPanes.insert(request->id,
                                   m_paneWorkspace->paneId(m_paneWorkspace->activePane()));
            m_pendingTransferRequests.insert(request->id);
            m_nonTerminalTransfers.insert(request->id);
            updateConnectionAction();
            emit transferRequested(*request);
        } else if (!error.isEmpty()) {
            statusBar()->showMessage(error, 8000);
        }
    }
}

void MainWindow::beginTrackedRemoteOperation(
    quint64 id, rfm::core::OperationKind kind,
    const QList<rfm::core::RemoteSelection>& sources, const QString& destination)
{
    const rfm::core::OperationProgress operation =
        rfm::core::beginRemoteOperation(id, kind, sources, destination);
    m_remoteOperations.insert(id, operation);
    updateTrackedOperation(operation);
}

void MainWindow::updateTrackedOperation(rfm::core::OperationProgress operation)
{
    const auto existing = m_operations.constFind(operation.id);
    operation.serverHost = operation.serverHost.trimmed();
    if ((operation.serverHost.isEmpty() || operation.serverPort == 0) &&
        existing != m_operations.cend() && !existing->serverHost.isEmpty() &&
        existing->serverPort != 0) {
        operation.serverHost = existing->serverHost;
        operation.serverPort = existing->serverPort;
    }
    if (operation.serverHost.isEmpty() || operation.serverPort == 0) {
        operation.serverHost = m_activeProfile.host.trimmed();
        operation.serverPort = m_activeProfile.port;
    }
    if (rfm::core::isTerminal(operation.state)) {
        if (!operation.finishedAt.isValid()) {
            operation.finishedAt =
                existing != m_operations.cend() && existing->finishedAt.isValid()
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
        m_operationPanel->updateOperation(operation);
        if (operation.id >= m_nextOperationId &&
            operation.id != std::numeric_limits<quint64>::max()) {
            m_nextOperationId = operation.id + 1;
        }
    }
}

void MainWindow::scheduleOperationHistorySave()
{
    m_historySaveTimer->start();
}

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
    bool operationCancelled = false;
    if (result.kind == rfm::core::RemoteOperationKind::Copy ||
        result.kind == rfm::core::RemoteOperationKind::Move) {
        const rfm::core::OperationProgress started = m_remoteOperations.take(result.id);
        const rfm::core::OperationProgress finished =
            rfm::core::finishRemoteOperation(result, started);
        operationCancelled = finished.state == rfm::core::OperationState::Cancelled;
        updateTrackedOperation(finished);
    }
    if (m_clipboardMoveOperations.remove(result.id) > 0 && result.allSucceeded()) {
        clearInternalClipboard();
    }
    QStringList failures;
    bool anySuccess = false;
    for (const rfm::core::RemoteItemResult& item : result.items) {
        if (!item.success) {
            const QString label =
                !item.source.isEmpty() && !item.destination.isEmpty()
                    ? tr("%1 → %2").arg(item.source, item.destination)
                    : (item.source.isEmpty() ? item.destination : item.source);
            failures.push_back(tr("%1: %2").arg(label, item.error));
        }
        anySuccess = anySuccess || item.success;
    }
    if (!failures.isEmpty()) {
        QMessageBox::warning(this, operationCancelled ? tr("Remote operation cancelled")
                                                      : tr("Remote operation incomplete"),
                             failures.join(QChar{'\n'}));
    }
    statusBar()->showMessage(
        operationCancelled
            ? tr("Remote operation cancelled")
            : (failures.isEmpty() ? tr("Remote operation completed")
                                  : tr("Remote operation completed with errors")));
    setBusy(false);
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
                    selectionsByPath[rfm::core::RemotePath::normalize(destinationParent)]
                        .push_back(rfm::core::RemotePath::fileName(item.destination));
                }
            }
        }
        for (const quint64 paneId : m_paneWorkspace->visiblePaneIds()) {
            FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
            const QString normalizedPath = rfm::core::RemotePath::normalize(pane->currentPath());
            if (selectionsByPath.contains(normalizedPath)) {
                pane->setPendingSelectionNames(selectionsByPath.value(normalizedPath));
            }
        }
        scheduleVisiblePanesForPaths(affectedPaths, true);
    }
}

void MainWindow::handleRemoteOperationProgress(rfm::core::OperationProgress progress)
{
    if (progress.id == 0 || progress.kind != rfm::core::OperationKind::RemoteCopy) {
        return;
    }
    m_remoteOperations.insert(progress.id, progress);
    updateTrackedOperation(std::move(progress));
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

    if (progress.state == rfm::core::TransferState::Completed &&
        progress.direction == rfm::core::TransferDirection::Upload &&
        !progress.destination.isEmpty()) {
        scheduleVisiblePanesForPaths(
            {rfm::core::RemotePath::normalize(
                rfm::core::RemotePath::parent(progress.destination))},
            false);
    }
    if (terminal) {
        m_transferPanes.remove(progress.id);
    }
}

void MainWindow::updateConnectionAction()
{
    const bool enabled =
        !m_busy && m_busyPanes.isEmpty() && m_nonTerminalTransfers.isEmpty();
    m_newConnectionAction->setEnabled(enabled && !m_connected);
    m_disconnectAction->setEnabled(enabled && m_connected);
    updateSelectedServerAction();
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

rfm::core::InternalTransferPayload MainWindow::transferPayload(
    quint64 paneId, const QList<rfm::core::RemoteSelection>& sources) const
{
    return {m_applicationInstanceId, currentConnectionIdentity(), paneId, sources};
}

bool MainWindow::startRemoteTransfer(rfm::core::InternalTransferAction action,
                                     const rfm::core::InternalTransferPayload& payload,
                                     quint64 destinationPaneId,
                                     const QString& destinationDirectory, bool clipboardMove)
{
    const rfm::core::InternalTransferValidation validation =
        rfm::core::validateInternalTransfer(payload, m_applicationInstanceId,
                                            currentConnectionIdentity(), destinationDirectory);
    FileBrowserPane* const sourcePane = m_paneWorkspace->pane(payload.sourcePaneId);
    FileBrowserPane* const destinationPane =
        destinationPaneId == 0 ? nullptr : m_paneWorkspace->pane(destinationPaneId);
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
    setBusy(true, move ? tr("Moving %1 item(s)…").arg(payload.sources.size())
                       : tr("Copying %1 item(s) on the server…").arg(payload.sources.size()));
    const quint64 id = nextOperationId();
    const QString sourceDirectory = payload.sources.isEmpty()
                                        ? QString{}
                                        : rfm::core::RemotePath::parent(
                                              payload.sources.constFirst().path);
    m_operationContexts.insert(
        id, {payload.sourcePaneId, destinationPaneId, sourceDirectory, destinationDirectory});
    beginTrackedRemoteOperation(id,
                                move ? rfm::core::OperationKind::RemoteMove
                                     : rfm::core::OperationKind::RemoteCopy,
                                payload.sources, destinationDirectory);
    if (clipboardMove) {
        m_clipboardMoveOperations.insert(id);
    }
    if (move) {
        emit moveRequested(id, payload.sources, destinationDirectory);
    } else {
        emit copyRequested(id, payload.sources, destinationDirectory);
    }
    return true;
}

QString MainWindow::transferValidationMessage(
    rfm::core::InternalTransferValidationError error) const
{
    using Error = rfm::core::InternalTransferValidationError;
    switch (error) {
    case Error::ForeignApplication:
    case Error::InvalidPayload:
        return tr("Only internal RemoteFileManager drags are accepted.");
    case Error::IncompatibleConnection:
        return tr("The source belongs to a different or expired server session.");
    case Error::InvalidDestination:
        return tr("The remote destination is invalid.");
    case Error::IncompatiblePathConvention:
        return tr("Source and destination use incompatible remote path conventions.");
    case Error::IdenticalSourceAndDestination:
        return tr("Source and destination are identical.");
    case Error::DestinationInsideSource:
        return tr("A folder cannot be copied or moved inside itself.");
    case Error::None:
        return {};
    }
    return tr("The remote destination is invalid.");
}

void MainWindow::updatePaneTransferContexts()
{
    const rfm::core::RemoteConnectionIdentity identity = currentConnectionIdentity();
    for (const quint64 paneId : m_paneWorkspace->paneIds()) {
        FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
        if (identity.isValid() && pane->source() == rfm::core::FileSource::Ssh &&
            pane->currentLocation().machineId == activeRemoteMachineId()) {
            pane->setTransferContext(m_applicationInstanceId, identity, paneId);
        } else {
            pane->clearTransferContext();
        }
    }
}

void MainWindow::updateCutAppearance()
{
    for (const quint64 paneId : m_paneWorkspace->paneIds()) {
        m_paneWorkspace->pane(paneId)->setCutPaths({});
    }
    if (!m_internalClipboard.isCut() || !m_internalClipboard.content().has_value()) {
        return;
    }
    const rfm::core::InternalTransferPayload& payload =
        m_internalClipboard.content()->payload;
    if (payload.connection != currentConnectionIdentity()) {
        return;
    }
    QSet<QString> paths;
    for (const rfm::core::RemoteSelection& source : payload.sources) {
        paths.insert(source.path);
    }
    if (FileBrowserPane* const pane = m_paneWorkspace->pane(payload.sourcePaneId)) {
        pane->setCutPaths(std::move(paths));
    }
}

void MainWindow::clearInternalClipboard()
{
    m_internalClipboard.clear();
    m_clipboardMoveOperations.clear();
    updateCutAppearance();
    updateOperationActions();
}

void MainWindow::requestDirectoryListing(quint64 paneId, const QString& path, bool showBusy,
                                         bool coalesceIfPending, PaneNavigation navigation)
{
    FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
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

    const quint64 requestId = nextOperationId();
    DirectoryRequest request;
    request.id = requestId;
    request.paneId = paneId;
    request.path = path;
    request.navigation = navigation;
    request.connectionGeneration = m_connectionGeneration;
    m_directoryRequests.insert(requestId, std::move(request));
    m_expectedDirectoryRequests.insert(paneId, requestId);
    m_directoryQueue.enqueue(requestId);
    if (showBusy) {
        setPaneBusy(paneId, true, tr("Refreshing %1…").arg(path));
    }
    startNextDirectoryListing();
}

void MainWindow::requestLocalDirectoryListing(quint64 paneId, const QString& path, bool showBusy,
                                              PaneNavigation navigation, bool treeRequest)
{
    FileBrowserPane* const pane = paneId == 0 ? nullptr : m_paneWorkspace->pane(paneId);
    if (path.isEmpty() || (!treeRequest &&
                          (pane == nullptr || pane->isHidden()))) {
        return;
    }
    if (paneId != 0) {
        const quint64 previousRequest = m_expectedLocalDirectoryRequests.value(paneId);
        if (previousRequest != 0) {
            m_localDirectoryRequests.remove(previousRequest);
        }
    }
    const quint64 requestId = nextOperationId();
    m_localDirectoryRequests.insert(requestId, {paneId, navigation, treeRequest});
    if (paneId != 0) {
        m_expectedLocalDirectoryRequests.insert(paneId, requestId);
        if (showBusy) {
            setPaneBusy(paneId, true, tr("Opening local folder %1…").arg(path));
        }
    }
    emit localDirectoryRequested(requestId, path);
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

void MainWindow::requestLocationListing(quint64 paneId,
                                        const rfm::core::BrowserLocation& location,
                                        bool showBusy, PaneNavigation navigation)
{
    if (location.source == rfm::core::FileSource::Local &&
        location.machineId == QString::fromLatin1(rfm::core::LocalMachineId)) {
        requestLocalDirectoryListing(paneId, location.path, showBusy, navigation);
    } else if (location.source == rfm::core::FileSource::Ssh && m_connected &&
               location.machineId == activeRemoteMachineId()) {
        requestDirectoryListing(paneId, location.path, showBusy, true, navigation);
    }
}

void MainWindow::handleLocalDirectoryListed(
    quint64 requestId, const QString& path, const QList<rfm::core::RemoteEntry>& entries)
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
    if (m_expectedLocalDirectoryRequests.value(request.paneId) != requestId) {
        return;
    }
    m_expectedLocalDirectoryRequests.remove(request.paneId);
    FileBrowserPane* const pane = m_paneWorkspace->pane(request.paneId);
    if (pane == nullptr || pane->isHidden()) {
        return;
    }
    const rfm::core::BrowserLocation location{rfm::core::FileSource::Local,
                                               QString::fromLatin1(rfm::core::LocalMachineId),
                                               path};
    pane->showDirectory(location, QUrl::fromLocalFile(path).toDisplayString(), entries,
                        request.navigation);
    pane->clearTransferContext();
    setPaneBusy(request.paneId, false);
    m_centralStack->setCurrentWidget(m_paneWorkspace);
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
    if (m_expectedLocalDirectoryRequests.value(request.paneId) == requestId) {
        m_expectedLocalDirectoryRequests.remove(request.paneId);
        setPaneBusy(request.paneId, false);
        statusBar()->showMessage(tr("Unable to list local folder %1: %2").arg(path, error), 8000);
    }
}

void MainWindow::openLocalLocation(const QString& path)
{
    const quint64 paneId = m_paneWorkspace->paneId(m_paneWorkspace->activePane());
    requestLocalDirectoryListing(paneId, path, true, PaneNavigation::Normal);
}

void MainWindow::openRemoteTreeLocation(const QString& profileId, const QString& path)
{
    if (!m_connected || profileId != activeRemoteMachineId()) {
        statusBar()->showMessage(tr("Connect this server before browsing its files."), 5000);
        return;
    }
    const quint64 paneId = m_paneWorkspace->paneId(m_paneWorkspace->activePane());
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

void MainWindow::setPaneBusy(quint64 paneId, bool busy, const QString& message)
{
    FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
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
        statusBar()->showMessage(message);
    }
    updateConnectionAction();
    updateOperationActions();
}

void MainWindow::schedulePaneRefresh(quint64 paneId, bool showBusy)
{
    FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
    if (pane == nullptr || pane->isHidden() || pane->currentPath().isEmpty()) {
        return;
    }
    m_scheduledPaneRefreshes[paneId] = m_scheduledPaneRefreshes.value(paneId) || showBusy;
    m_refreshDebounceTimer->start();
}

void MainWindow::scheduleVisiblePanesForPaths(const QSet<QString>& paths, bool showBusy)
{
    for (const quint64 paneId : m_paneWorkspace->visiblePaneIds()) {
        FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
        if (pane->source() == rfm::core::FileSource::Ssh &&
            paths.contains(rfm::core::RemotePath::normalize(pane->currentPath()))) {
            schedulePaneRefresh(paneId, showBusy);
        }
    }
}

bool MainWindow::confirmOtherPaneOperation(
    const QString& operation, const QList<rfm::core::RemoteSelection>& sources,
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
    if (m_autoRefreshTimer != nullptr) {
        m_autoRefreshTimer->stop();
    }
    if (m_refreshDebounceTimer != nullptr) {
        m_refreshDebounceTimer->stop();
    }
    m_directoryRequests.clear();
    m_directoryQueue.clear();
    m_expectedDirectoryRequests.clear();
    m_activeDirectoryRequestId = 0;
    m_busyPanes.clear();
    m_scheduledPaneRefreshes.clear();
}

void MainWindow::updateOperationActions()
{
    const qsizetype count = selectedEntries().size();
    const quint64 paneId = m_paneWorkspace->paneId(m_paneWorkspace->activePane());
    const FileBrowserPane* const activePane = m_paneWorkspace->activePane();
    const bool locationAvailable = activePane->hasLocation() && !m_busy &&
                                   !m_busyPanes.contains(paneId);
    const bool available = locationAvailable && m_connected &&
                           activePane->source() == rfm::core::FileSource::Ssh &&
                           activePane->currentLocation().machineId == activeRemoteMachineId();
    m_createDirectoryAction->setEnabled(available);
    m_renameAction->setEnabled(available && count == 1);
    m_moveAction->setEnabled(available && count > 0);
    m_copyAction->setEnabled(available && count > 0);
    FileBrowserPane* const otherPane = m_paneWorkspace->otherVisiblePane(paneId);
    const quint64 otherPaneId = m_paneWorkspace->paneId(otherPane);
    const bool distinctDirectories =
        otherPane != nullptr &&
        rfm::core::RemotePath::normalize(m_paneWorkspace->activePane()->currentPath()) !=
            rfm::core::RemotePath::normalize(otherPane->currentPath());
    const bool compatiblePathConventions =
        otherPane != nullptr &&
        pathsUseSameConvention(m_paneWorkspace->activePane()->currentPath(),
                               otherPane->currentPath());
    const bool otherPaneAvailable = available && count > 0 && otherPane != nullptr &&
                                    !otherPane->currentPath().isEmpty() &&
                                    otherPane->source() == rfm::core::FileSource::Ssh &&
                                    otherPane->currentLocation().machineId ==
                                        activeRemoteMachineId() &&
                                    !m_busyPanes.contains(otherPaneId) && distinctDirectories &&
                                    compatiblePathConventions;
    m_moveToOtherPaneAction->setEnabled(otherPaneAvailable);
    m_copyToOtherPaneAction->setEnabled(otherPaneAvailable);
    m_removeAction->setEnabled(available && count > 0);
    m_uploadAction->setEnabled(available);
    m_downloadAction->setEnabled(available && count > 0);
    m_clipboardCopyAction->setEnabled(available && count > 0);
    m_clipboardCutAction->setEnabled(available && count > 0);
    bool pasteAvailable = false;
    if (available && m_internalClipboard.content().has_value()) {
        pasteAvailable = rfm::core::validateInternalTransfer(
                             m_internalClipboard.content()->payload,
                             m_applicationInstanceId, currentConnectionIdentity(),
                             m_paneWorkspace->activePane()->currentPath())
                             .accepted();
    }
    m_clipboardPasteAction->setEnabled(pasteAvailable);
    m_selectAllAction->setEnabled(locationAvailable);
    m_focusLocationAction->setEnabled(locationAvailable);
    m_switchPaneAction->setEnabled(!m_busy && m_paneWorkspace->isSplit());
    m_cancelCutAction->setEnabled(m_internalClipboard.isCut());
}

QList<rfm::core::RemoteSelection> MainWindow::selectedEntries() const
{
    return m_paneWorkspace->activePane()->selectedEntries();
}

QString MainWindow::askDestination(const QString& title)
{
    bool accepted = false;
    const QString value = QInputDialog::getText(this, title, tr("Remote destination folder:"),
                                                QLineEdit::Normal,
                                                m_paneWorkspace->activePane()->currentPath(),
                                                &accepted);
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
    m_paneWorkspace->activePane()->requestParentDirectory();
}

void MainWindow::updateNavigationActions()
{
    const FileBrowserPane* const pane = m_paneWorkspace->activePane();
    const quint64 paneId = m_paneWorkspace->paneId(pane);
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
            m_activeProfile.host, m_activeProfile.username, m_activeProfile.port,
            m_activeSavedProfileId};
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
    for (const quint64 paneId : m_paneWorkspace->visiblePaneIds()) {
        FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
        pane->setInteractionEnabled(pane->hasLocation() && !busy &&
                                    !m_busyPanes.contains(paneId));
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
        tr("RemoteFileManager %1\n\nA native file manager for standard SSH/SFTP servers.\n"
           "Sprint 7: local navigation and machine-scoped navigation tree.")
            .arg(QApplication::applicationVersion()));
}

} // namespace rfm::app
