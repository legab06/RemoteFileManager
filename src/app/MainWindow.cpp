#include "remotefilemanager/app/MainWindow.hpp"

#include "remotefilemanager/app/ConnectionDialog.hpp"
#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/app/OperationPanel.hpp"
#include "remotefilemanager/app/PaneWorkspace.hpp"
#include "remotefilemanager/app/TransferRequestFactory.hpp"
#include "remotefilemanager/core/RemotePath.hpp"
#include "remotefilemanager/ssh/LibsshRuntime.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(tr("RemoteFileManager"));
    resize(1100, 700);
    setMinimumSize(760, 480);
    setUnifiedTitleAndToolBarOnMac(true);

    createPaneWorkspace();
    createActions();
    createMenus();
    createNavigationBar();
    createPlacesDock();
    createOperationDock();
    createEmptyState();

    m_autoRefreshTimer = new QTimer(this);
    m_autoRefreshTimer->setObjectName(QStringLiteral("autoRefreshTimer"));
    m_autoRefreshTimer->setInterval(3000);
    connect(m_autoRefreshTimer, &QTimer::timeout, this, [this] {
        for (const quint64 paneId : m_paneWorkspace->visiblePaneIds()) {
            requestDirectoryListing(paneId, m_paneWorkspace->pane(paneId)->currentPath(), false,
                                    false);
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
                requestDirectoryListing(iterator.key(), pane->currentPath(), iterator.value(),
                                        true);
            }
        }
    });

    qRegisterMetaType<rfm::core::ConnectionProfile>();
    qRegisterMetaType<QList<rfm::core::RemoteEntry>>();
    qRegisterMetaType<QList<rfm::core::RemoteSelection>>();
    qRegisterMetaType<rfm::core::RemoteOperationResult>();
    qRegisterMetaType<rfm::core::OperationProgress>();
    qRegisterMetaType<rfm::core::TransferRequest>();
    qRegisterMetaType<rfm::core::TransferProgress>();
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
    connect(this, &MainWindow::shutdownRequested, m_sshSession,
            &rfm::ssh::SshSession::shutdownTransfers);
    connect(this, &MainWindow::disconnectionRequested, m_sshSession,
            &rfm::ssh::SshSession::disconnectFromHost);
    connect(m_sshSession, &rfm::ssh::SshSession::hostKeyConfirmationRequired, this,
            &MainWindow::showHostKeyConfirmation);
    connect(m_sshSession, &rfm::ssh::SshSession::connected, this,
            [this](const QString& path, const QList<rfm::core::RemoteEntry>& entries) {
                m_connected = true;
                showRemoteDirectory(path, entries);
                statusBar()->showMessage(tr("Connected securely to %1").arg(m_activeProfile.host));
            });
    connect(m_sshSession, &rfm::ssh::SshSession::directoryListed, this,
            &MainWindow::handleDirectoryListed);
    connect(m_sshSession, &rfm::ssh::SshSession::directoryListingFailed, this,
            &MainWindow::handleDirectoryListingError);
    connect(m_sshSession, &rfm::ssh::SshSession::failed, this, &MainWindow::showConnectionError);
    connect(m_sshSession, &rfm::ssh::SshSession::operationFinished, this,
            &MainWindow::handleOperationResult);
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
    connect(m_sshSession, &rfm::ssh::SshSession::disconnected, this, [this] {
        m_connected = false;
        stopAutomaticRefresh();
        setBusy(false);
    });
    m_sshThread->start();

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
}

void MainWindow::createActions()
{
    m_newConnectionAction =
        new QAction(style()->standardIcon(QStyle::SP_ComputerIcon), tr("New connection…"), this);
    m_newConnectionAction->setObjectName(QStringLiteral("newConnectionAction"));
    m_newConnectionAction->setShortcut(QKeySequence::New);
    connect(m_newConnectionAction, &QAction::triggered, this, &MainWindow::showConnectionDialog);

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
    connect(m_removeAction, &QAction::triggered, this, &MainWindow::removeSelectedEntries);

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

    QMenu* const viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_splitViewAction);

    QMenu* const helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(m_aboutAction);
}

void MainWindow::createPaneWorkspace()
{
    m_paneWorkspace = new PaneWorkspace(this);
    connectBrowserPane(m_paneWorkspace->paneId(m_paneWorkspace->primaryPane()));
    connect(m_paneWorkspace, &PaneWorkspace::activePaneChanged, this, [this](quint64) {
        updateOperationActions();
        const FileBrowserPane* const pane = m_paneWorkspace->activePane();
        const quint64 paneId = m_paneWorkspace->paneId(pane);
        m_upAction->setEnabled(!m_busy && !m_busyPanes.contains(paneId) &&
                               pane->currentPath() != QStringLiteral("."));
        m_refreshAction->setEnabled(m_connected && !m_busy && !m_busyPanes.contains(paneId));
        m_backAction->setEnabled(!m_busy && !m_busyPanes.contains(paneId) && pane->canGoBack());
        m_forwardAction->setEnabled(!m_busy && !m_busyPanes.contains(paneId) &&
                                    pane->canGoForward());
    });
    connect(m_paneWorkspace, &PaneWorkspace::paneVisibilityChanged, this,
            [this](quint64 paneId, bool visible) {
                if (!visible) {
                    cancelDirectoryRequests(paneId);
                    return;
                }
                connectBrowserPane(paneId);
                FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
                if (m_connected &&
                    pane->property("connectionGeneration").toULongLong() !=
                        m_connectionGeneration) {
                    const QString path = m_paneWorkspace->activePane()->currentPath();
                    if (!path.isEmpty()) {
                        requestDirectoryListing(paneId, path, true, true,
                                                PaneNavigation::Initial);
                    }
                }
            });
}

void MainWindow::connectBrowserPane(quint64 paneId)
{
    FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
    if (pane == nullptr || pane->property("mainWindowConnected").toBool()) {
        return;
    }
    pane->setProperty("mainWindowConnected", true);
    connect(pane, &FileBrowserPane::navigationRequested, this,
            [this, paneId](const QString& path, PaneNavigation navigation) {
                requestDirectoryListing(paneId, path, true, true, navigation);
            });
    connect(pane, &FileBrowserPane::historyChanged, this, [this, paneId, pane] {
        if (pane == m_paneWorkspace->activePane()) {
            m_backAction->setEnabled(!m_busy && !m_busyPanes.contains(paneId) &&
                                     pane->canGoBack());
            m_forwardAction->setEnabled(!m_busy && !m_busyPanes.contains(paneId) &&
                                        pane->canGoForward());
        }
    });
    connect(pane, &FileBrowserPane::selectionChanged, this, [this, paneId] {
        if (paneId == m_paneWorkspace->paneId(m_paneWorkspace->activePane())) {
            updateOperationActions();
        }
    });
    connect(pane, &FileBrowserPane::contextMenuRequested, this,
            &MainWindow::showFileContextMenu);
}

void MainWindow::createNavigationBar()
{
    auto* const navigationBar = addToolBar(tr("Navigation"));
    navigationBar->setObjectName(QStringLiteral("navigationToolBar"));
    navigationBar->setMovable(false);

    m_backAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_ArrowBack), tr("Back"));
    m_backAction->setObjectName(QStringLiteral("backAction"));
    m_forwardAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_ArrowForward), tr("Forward"));
    m_forwardAction->setObjectName(QStringLiteral("forwardAction"));
    m_upAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_ArrowUp), tr("Parent folder"));
    m_upAction->setObjectName(QStringLiteral("upAction"));
    m_refreshAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), tr("Refresh"));
    m_refreshAction->setObjectName(QStringLiteral("refreshAction"));

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
}

void MainWindow::createPlacesDock()
{
    auto* const placesDock = new QDockWidget(tr("Places"), this);
    placesDock->setObjectName(QStringLiteral("placesDock"));
    placesDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto* const placesList = new QListWidget(placesDock);
    placesList->setObjectName(QStringLiteral("placesList"));
    auto* const remoteItem = new QListWidgetItem(style()->standardIcon(QStyle::SP_ComputerIcon),
                                                 tr("Remote server"), placesList);
    remoteItem->setFlags(remoteItem->flags() & ~Qt::ItemIsEnabled);

    placesDock->setWidget(placesList);
    addDockWidget(Qt::LeftDockWidgetArea, placesDock);
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
    connect(m_operationPanel, &OperationPanel::cancelRequested, this,
            &MainWindow::cancelTransferRequested);
}

void MainWindow::createEmptyState()
{
    auto* const emptyState = new QWidget(this);
    emptyState->setObjectName(QStringLiteral("emptyState"));

    auto* const layout = new QVBoxLayout(emptyState);
    layout->setContentsMargins(48, 48, 48, 48);
    layout->setSpacing(12);
    layout->addStretch();

    auto* const title = new QLabel(tr("No SSH connection"), emptyState);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);

    auto* const description = new QLabel(
        tr("Connect to a standard SSH server to browse and manage its files."), emptyState);
    description->setAlignment(Qt::AlignCenter);
    description->setWordWrap(true);

    auto* const connectionButton = new QPushButton(style()->standardIcon(QStyle::SP_ComputerIcon),
                                                   tr("New connection…"), emptyState);
    connectionButton->setObjectName(QStringLiteral("newConnectionButton"));
    connectionButton->setDefault(true);

    layout->addWidget(title);
    layout->addWidget(description);
    layout->addSpacing(8);
    layout->addWidget(connectionButton, 0, Qt::AlignHCenter);
    layout->addStretch();

    connect(connectionButton, &QPushButton::clicked, this, &MainWindow::showConnectionDialog);
    setCentralWidget(emptyState);
}

void MainWindow::showConnectionDialog()
{
    ConnectionDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_activeProfile = dialog.profile();
    setBusy(true, tr("Connecting securely to %1…").arg(m_activeProfile.host));
    emit connectionRequested(m_activeProfile, dialog.password());
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
    ++m_connectionGeneration;
    if (centralWidget() != m_paneWorkspace) {
        setCentralWidget(m_paneWorkspace);
    }
    FileBrowserPane* const pane = m_paneWorkspace->activePane();
    const QString displayPath = remoteDisplayUrl(m_activeProfile, path);
    pane->showDirectory(path, displayPath, entries, PaneNavigation::Initial);
    pane->setProperty("connectionGeneration", QVariant::fromValue(m_connectionGeneration));
    m_upAction->setEnabled(pane->currentPath() != QStringLiteral("."));
    m_refreshAction->setEnabled(true);
    setBusy(false);
    updateOperationActions();
    if (!m_autoRefreshTimer->isActive()) {
        m_autoRefreshTimer->start();
    }
    if (m_paneWorkspace->isSplit()) {
        const quint64 activePaneId = m_paneWorkspace->paneId(pane);
        for (const quint64 paneId : m_paneWorkspace->visiblePaneIds()) {
            if (paneId != activePaneId) {
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
    const bool expected = m_expectedDirectoryRequests.value(request.paneId) == requestId;
    FileBrowserPane* const pane = m_paneWorkspace->pane(request.paneId);
    if (expected && pane != nullptr && !pane->isHidden()) {
        const QString displayPath = remoteDisplayUrl(m_activeProfile, path);
        pane->showDirectory(path, displayPath, entries, request.navigation);
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
    m_connected = false;
    stopAutomaticRefresh();
    m_pendingTransferRequests.clear();
    m_nonTerminalTransfers.clear();
    m_operationContexts.clear();
    for (auto operation : std::as_const(m_remoteOperations)) {
        operation.state = rfm::core::OperationState::Failed;
        operation.error = message;
        m_operationPanel->updateOperation(operation);
    }
    m_remoteOperations.clear();
    m_transferPanes.clear();
    setBusy(false);
    statusBar()->showMessage(tr("Disconnected"));
    QMessageBox::critical(this, tr("SSH connection error"), message);
}

void MainWindow::showFileContextMenu(const QPoint& globalPosition)
{
    if (m_busy) {
        return;
    }
    updateOperationActions();
    QMenu menu(this);
    const bool hasSelection = !selectedEntries().isEmpty();
    if (m_paneWorkspace->isSplit() && hasSelection) {
        menu.addAction(m_copyToOtherPaneAction);
        menu.addAction(m_moveToOtherPaneAction);
        menu.addSeparator();
    }
    menu.addAction(m_createDirectoryAction);
    if (hasSelection) {
        menu.addSeparator();
        menu.addAction(m_renameAction);
        if (m_paneWorkspace->isSplit()) {
            QMenu* const advancedMenu = menu.addMenu(tr("More…"));
            advancedMenu->setObjectName(QStringLiteral("advancedOperationsMenu"));
            advancedMenu->menuAction()->setObjectName(
                QStringLiteral("advancedOperationsMenuAction"));
            advancedMenu->addAction(m_copyAction);
            advancedMenu->addAction(m_moveAction);
        } else {
            menu.addAction(m_copyAction);
            menu.addAction(m_moveAction);
        }
        menu.addAction(m_downloadAction);
        menu.addSeparator();
        menu.addAction(m_removeAction);
    }
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
    const QList<rfm::core::RemoteSelection> selection = selectedEntries();
    if (selection.isEmpty()) {
        return;
    }
    const QString destination = askDestination(tr("Move selected items"));
    if (destination.isEmpty()) {
        return;
    }
    setBusy(true, tr("Moving %1 item(s)…").arg(selection.size()));
    const quint64 id = nextOperationId();
    const quint64 paneId = m_paneWorkspace->paneId(m_paneWorkspace->activePane());
    m_operationContexts.insert(
        id, {paneId, 0, m_paneWorkspace->activePane()->currentPath(), destination});
    beginTrackedRemoteOperation(id, rfm::core::OperationKind::RemoteMove, selection, destination);
    emit moveRequested(id, selection, destination);
}

void MainWindow::copySelectedEntries()
{
    const QList<rfm::core::RemoteSelection> selection = selectedEntries();
    if (selection.isEmpty()) {
        return;
    }
    const QString destination = askDestination(tr("Copy selected items"));
    if (destination.isEmpty()) {
        return;
    }
    setBusy(true, tr("Copying %1 item(s) on the server…").arg(selection.size()));
    const quint64 id = nextOperationId();
    const quint64 paneId = m_paneWorkspace->paneId(m_paneWorkspace->activePane());
    m_operationContexts.insert(
        id, {paneId, 0, m_paneWorkspace->activePane()->currentPath(), destination});
    beginTrackedRemoteOperation(id, rfm::core::OperationKind::RemoteCopy, selection, destination);
    emit copyRequested(id, selection, destination);
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
    setBusy(true, tr("Moving %1 item(s)…").arg(selection.size()));
    const quint64 id = nextOperationId();
    m_operationContexts.insert(
        id, {sourcePaneId, destinationPaneId, sourceDirectory, destination});
    beginTrackedRemoteOperation(id, rfm::core::OperationKind::RemoteMove, selection, destination);
    emit moveRequested(id, selection, destination);
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
    setBusy(true, tr("Copying %1 item(s) on the server…").arg(selection.size()));
    const quint64 id = nextOperationId();
    m_operationContexts.insert(
        id, {sourcePaneId, destinationPaneId, sourceDirectory, destination});
    beginTrackedRemoteOperation(id, rfm::core::OperationKind::RemoteCopy, selection, destination);
    emit copyRequested(id, selection, destination);
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
        const auto request =
            TransferRequestFactory::download(nextOperationId(), entry, localDirectory);
        if (request.has_value()) {
            m_transferPanes.insert(request->id,
                                   m_paneWorkspace->paneId(m_paneWorkspace->activePane()));
            m_pendingTransferRequests.insert(request->id);
            m_nonTerminalTransfers.insert(request->id);
            updateConnectionAction();
            emit transferRequested(*request);
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
    m_operationPanel->updateOperation(operation);
}

void MainWindow::handleOperationResult(const rfm::core::RemoteOperationResult& result)
{
    const OperationContext context = m_operationContexts.take(result.id);
    if (result.kind == rfm::core::RemoteOperationKind::Copy ||
        result.kind == rfm::core::RemoteOperationKind::Move) {
        const rfm::core::OperationProgress started = m_remoteOperations.take(result.id);
        m_operationPanel->updateOperation(rfm::core::finishRemoteOperation(result, started));
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
        QMessageBox::warning(this,
                             result.allSucceeded() ? tr("Remote operation")
                                                   : tr("Remote operation incomplete"),
                             failures.join(QChar{'\n'}));
    }
    statusBar()->showMessage(failures.isEmpty() ? tr("Remote operation completed")
                                                : tr("Remote operation completed with errors"));
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

void MainWindow::handleTransferProgress(const rfm::core::TransferProgress& progress)
{
    m_operationPanel->updateOperation(rfm::core::operationProgress(progress));
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
    m_newConnectionAction->setEnabled(!m_busy && m_busyPanes.isEmpty() &&
                                      m_nonTerminalTransfers.isEmpty());
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
    m_directoryRequests.insert(requestId, {requestId, paneId, path, navigation});
    m_expectedDirectoryRequests.insert(paneId, requestId);
    m_directoryQueue.enqueue(requestId);
    if (showBusy) {
        setPaneBusy(paneId, true, tr("Refreshing %1…").arg(path));
    }
    startNextDirectoryListing();
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
    if (pane == m_paneWorkspace->activePane()) {
        m_upAction->setEnabled(!busy && !m_busy && m_connected &&
                               pane->currentPath() != QStringLiteral("."));
        m_refreshAction->setEnabled(!busy && !m_busy && m_connected);
        m_backAction->setEnabled(!busy && !m_busy && pane->canGoBack());
        m_forwardAction->setEnabled(!busy && !m_busy && pane->canGoForward());
    }
    if (busy && !message.isEmpty()) {
        statusBar()->showMessage(message);
    }
    updateConnectionAction();
    updateOperationActions();
}

void MainWindow::schedulePaneRefresh(quint64 paneId, bool showBusy)
{
    FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
    if (!m_connected || pane == nullptr || pane->isHidden() || pane->currentPath().isEmpty()) {
        return;
    }
    m_scheduledPaneRefreshes[paneId] = m_scheduledPaneRefreshes.value(paneId) || showBusy;
    m_refreshDebounceTimer->start();
}

void MainWindow::scheduleVisiblePanesForPaths(const QSet<QString>& paths, bool showBusy)
{
    for (const quint64 paneId : m_paneWorkspace->visiblePaneIds()) {
        FileBrowserPane* const pane = m_paneWorkspace->pane(paneId);
        if (paths.contains(rfm::core::RemotePath::normalize(pane->currentPath()))) {
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
    const bool available = m_connected && !m_busy && !m_busyPanes.contains(paneId);
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
                                    !m_busyPanes.contains(otherPaneId) && distinctDirectories &&
                                    compatiblePathConventions;
    m_moveToOtherPaneAction->setEnabled(otherPaneAvailable);
    m_copyToOtherPaneAction->setEnabled(otherPaneAvailable);
    m_removeAction->setEnabled(available && count > 0);
    m_uploadAction->setEnabled(available);
    m_downloadAction->setEnabled(available && count > 0);
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

quint64 MainWindow::nextOperationId() { return m_nextOperationId++; }

void MainWindow::requestParentDirectory()
{
    m_paneWorkspace->activePane()->requestParentDirectory();
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
    const quint64 activePaneId = m_paneWorkspace->paneId(m_paneWorkspace->activePane());
    m_upAction->setEnabled(!busy && m_connected && !m_busyPanes.contains(activePaneId) &&
                           m_paneWorkspace->activePane()->currentPath() != QStringLiteral("."));
    m_refreshAction->setEnabled(!busy && m_connected && !m_busyPanes.contains(activePaneId));
    m_backAction->setEnabled(!busy && !m_busyPanes.contains(activePaneId) &&
                             m_paneWorkspace->activePane()->canGoBack());
    m_forwardAction->setEnabled(!busy && !m_busyPanes.contains(activePaneId) &&
                                m_paneWorkspace->activePane()->canGoForward());
    for (const quint64 paneId : m_paneWorkspace->visiblePaneIds()) {
        m_paneWorkspace->pane(paneId)->setInteractionEnabled(!busy &&
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
           "Sprint 3: queued file and folder transfers.")
            .arg(QApplication::applicationVersion()));
}

} // namespace rfm::app
