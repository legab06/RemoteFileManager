#include "remotefilemanager/app/MainWindow.hpp"

#include "remotefilemanager/app/ConnectionDialog.hpp"
#include "remotefilemanager/app/TransferPanel.hpp"
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
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <limits>
#include <utility>

namespace rfm::app
{
namespace
{

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

    createActions();
    createMenus();
    createNavigationBar();
    createPlacesDock();
    createTransferDock();
    createEmptyState();

    m_autoRefreshTimer = new QTimer(this);
    m_autoRefreshTimer->setObjectName(QStringLiteral("autoRefreshTimer"));
    m_autoRefreshTimer->setInterval(3000);
    connect(m_autoRefreshTimer, &QTimer::timeout, this,
            [this] { requestDirectoryListing(m_currentPath, false, false); });
    m_refreshDebounceTimer = new QTimer(this);
    m_refreshDebounceTimer->setObjectName(QStringLiteral("refreshDebounceTimer"));
    m_refreshDebounceTimer->setInterval(150);
    m_refreshDebounceTimer->setSingleShot(true);
    connect(m_refreshDebounceTimer, &QTimer::timeout, this, [this] {
        const bool showBusy = m_scheduledRefreshBusy;
        m_scheduledRefreshBusy = false;
        requestDirectoryListing(m_currentPath, showBusy, true);
    });

    qRegisterMetaType<rfm::core::ConnectionProfile>();
    qRegisterMetaType<QList<rfm::core::RemoteEntry>>();
    qRegisterMetaType<QList<rfm::core::RemoteSelection>>();
    qRegisterMetaType<rfm::core::RemoteOperationResult>();
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
            &MainWindow::showRemoteDirectory);
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
    fileMenu->addAction(m_removeAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_uploadAction);
    fileMenu->addAction(m_downloadAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_quitAction);

    QMenu* const helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(m_aboutAction);
}

void MainWindow::createNavigationBar()
{
    auto* const navigationBar = addToolBar(tr("Navigation"));
    navigationBar->setObjectName(QStringLiteral("navigationToolBar"));
    navigationBar->setMovable(false);

    m_backAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_ArrowBack), tr("Back"));
    m_forwardAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_ArrowForward), tr("Forward"));
    m_upAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_ArrowUp), tr("Parent folder"));
    m_refreshAction =
        navigationBar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), tr("Refresh"));

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
    connect(m_upAction, &QAction::triggered, this, &MainWindow::requestParentDirectory);
    connect(m_refreshAction, &QAction::triggered, this, [this] {
        if (!m_currentPath.isEmpty()) {
            requestDirectoryListing(m_currentPath, true, true);
        }
    });

    navigationBar->addSeparator();
    m_remotePathEdit = new QLineEdit(navigationBar);
    m_remotePathEdit->setObjectName(QStringLiteral("remotePathEdit"));
    m_remotePathEdit->setReadOnly(true);
    m_remotePathEdit->setPlaceholderText(tr("sftp://user@server/path"));
    m_remotePathEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    navigationBar->addWidget(m_remotePathEdit);

    navigationBar->addSeparator();
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

void MainWindow::createTransferDock()
{
    auto* const transferDock = new QDockWidget(tr("Transfers"), this);
    transferDock->setObjectName(QStringLiteral("transferDock"));
    transferDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    m_transferPanel = new TransferPanel(transferDock);
    transferDock->setWidget(m_transferPanel);
    addDockWidget(Qt::BottomDockWidgetArea, transferDock);

    connect(m_transferPanel, &TransferPanel::pauseRequested, this,
            &MainWindow::pauseTransferRequested);
    connect(m_transferPanel, &TransferPanel::resumeRequested, this,
            &MainWindow::resumeTransferRequested);
    connect(m_transferPanel, &TransferPanel::cancelRequested, this,
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
    QStringList namesToSelect;
    int previousScrollPosition = -1;
    const bool sameDirectory =
        m_fileTable != nullptr &&
        rfm::core::RemotePath::normalize(path) == rfm::core::RemotePath::normalize(m_currentPath);
    if (sameDirectory && m_fileTable->selectionModel() != nullptr) {
        for (const QModelIndex& index : m_fileTable->selectionModel()->selectedRows(0)) {
            if (const QTableWidgetItem* const item = m_fileTable->item(index.row(), 0);
                item != nullptr) {
                namesToSelect.push_back(item->text());
            }
        }
        previousScrollPosition = m_fileTable->verticalScrollBar()->value();
    }
    for (const QString& name : std::as_const(m_pendingSelectionNames)) {
        if (!namesToSelect.contains(name)) {
            namesToSelect.push_back(name);
        }
    }
    m_pendingSelectionNames.clear();

    m_connected = true;
    if (m_fileTable == nullptr) {
        m_fileTable = new QTableWidget(this);
        m_fileTable->setObjectName(QStringLiteral("remoteFileTable"));
        m_fileTable->setColumnCount(3);
        m_fileTable->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified")});
        m_fileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_fileTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_fileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_fileTable->setContextMenuPolicy(Qt::CustomContextMenu);
        m_fileTable->setShowGrid(false);
        m_fileTable->verticalHeader()->hide();
        m_fileTable->horizontalHeader()->setStretchLastSection(true);
        m_fileTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        connect(m_fileTable, &QTableWidget::cellDoubleClicked, this,
                &MainWindow::openSelectedEntry);
        connect(m_fileTable, &QWidget::customContextMenuRequested, this,
                &MainWindow::showFileContextMenu);
        connect(m_fileTable->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                &MainWindow::updateOperationActions);
        setCentralWidget(m_fileTable);
    }
    m_currentPath = path;
    m_fileTable->setRowCount(static_cast<int>(entries.size()));
    QFileIconProvider icons;
    for (qsizetype row = 0; row < entries.size(); ++row) {
        const auto& entry = entries.at(row);
        auto* const nameItem = new QTableWidgetItem(
            icons.icon(entry.directory ? QFileIconProvider::Folder : QFileIconProvider::File),
            entry.name);
        nameItem->setData(Qt::UserRole, entry.directory);
        nameItem->setData(Qt::UserRole + 1, entry.symbolicLink);
        m_fileTable->setItem(static_cast<int>(row), 0, nameItem);
        const qint64 displaySize =
            entry.size > static_cast<quint64>(std::numeric_limits<qint64>::max())
                ? std::numeric_limits<qint64>::max()
                : static_cast<qint64>(entry.size);
        auto* const sizeItem = new QTableWidgetItem(
            entry.directory ? QString{} : QLocale{}.formattedDataSize(displaySize));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_fileTable->setItem(static_cast<int>(row), 1, sizeItem);
        m_fileTable->setItem(
            static_cast<int>(row), 2,
            new QTableWidgetItem(QLocale{}.toString(entry.modifiedAt, QLocale::ShortFormat)));
    }
    if (!namesToSelect.isEmpty()) {
        for (int row = 0; row < m_fileTable->rowCount(); ++row) {
            const QTableWidgetItem* const item = m_fileTable->item(row, 0);
            if (item != nullptr && namesToSelect.contains(item->text())) {
                m_fileTable->selectionModel()->select(m_fileTable->model()->index(row, 0),
                                                      QItemSelectionModel::Select |
                                                          QItemSelectionModel::Rows);
            }
        }
    }
    if (previousScrollPosition >= 0) {
        m_fileTable->verticalScrollBar()->setValue(previousScrollPosition);
    }
    m_remotePathEdit->setText(QStringLiteral("sftp://%1@%2:%3/%4")
                                  .arg(m_activeProfile.username, m_activeProfile.host)
                                  .arg(m_activeProfile.port)
                                  .arg(path == QStringLiteral(".") ? QString{} : path));
    m_upAction->setEnabled(path != QStringLiteral("."));
    m_refreshAction->setEnabled(true);
    m_listingInProgress = false;
    setBusy(false);
    updateOperationActions();
    if (!m_autoRefreshTimer->isActive()) {
        m_autoRefreshTimer->start();
    }

    if (!m_deferredDirectoryPath.isEmpty()) {
        const QString deferredPath = std::exchange(m_deferredDirectoryPath, {});
        const bool showBusy = std::exchange(m_deferredDirectoryBusy, false);
        requestDirectoryListing(deferredPath, showBusy, true);
    }
}

void MainWindow::showConnectionError(const QString& message)
{
    m_connected = false;
    stopAutomaticRefresh();
    m_pendingTransferRequests.clear();
    m_nonTerminalTransfers.clear();
    setBusy(false);
    statusBar()->showMessage(tr("Disconnected"));
    QMessageBox::critical(this, tr("SSH connection error"), message);
}

void MainWindow::showFileContextMenu(const QPoint& position)
{
    if (m_fileTable == nullptr || m_busy) {
        return;
    }
    if (QTableWidgetItem* const item = m_fileTable->itemAt(position);
        item != nullptr && !item->isSelected()) {
        m_fileTable->clearSelection();
        m_fileTable->selectRow(item->row());
    } else if (item == nullptr) {
        m_fileTable->clearSelection();
    }
    updateOperationActions();
    QMenu menu(this);
    menu.addAction(m_createDirectoryAction);
    if (!selectedEntries().isEmpty()) {
        menu.addSeparator();
        menu.addAction(m_renameAction);
        menu.addAction(m_moveAction);
        menu.addAction(m_copyAction);
        menu.addAction(m_downloadAction);
        menu.addSeparator();
        menu.addAction(m_removeAction);
    }
    menu.exec(m_fileTable->viewport()->mapToGlobal(position));
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
    emit createDirectoryRequested(nextOperationId(), m_currentPath, name);
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
    emit renameRequested(nextOperationId(), selection.constFirst().path, newName);
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
    emit moveRequested(nextOperationId(), selection, destination);
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
    emit copyRequested(nextOperationId(), selection, destination);
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
    emit removeRequested(nextOperationId(), selection, recursive);
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
    if (!m_connected || m_currentPath.isEmpty()) {
        return;
    }
    for (const QString& path : std::as_const(localPaths)) {
        const auto request = TransferRequestFactory::upload(nextOperationId(), path, m_currentPath);
        if (request.has_value()) {
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
            m_pendingTransferRequests.insert(request->id);
            m_nonTerminalTransfers.insert(request->id);
            updateConnectionAction();
            emit transferRequested(*request);
        }
    }
}

void MainWindow::handleOperationResult(const rfm::core::RemoteOperationResult& result)
{
    QStringList failures;
    bool anySuccess = false;
    m_pendingSelectionNames.clear();
    for (const rfm::core::RemoteItemResult& item : result.items) {
        if (!item.success) {
            const QString label = item.source.isEmpty() ? item.destination : item.source;
            failures.push_back(tr("%1: %2").arg(label, item.error));
        } else if (!item.destination.isEmpty() &&
                   rfm::core::RemotePath::normalize(rfm::core::RemotePath::parent(
                       item.destination)) == rfm::core::RemotePath::normalize(m_currentPath)) {
            m_pendingSelectionNames.push_back(rfm::core::RemotePath::fileName(item.destination));
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
        scheduleCurrentDirectoryRefresh(true);
    }
}

void MainWindow::handleTransferProgress(const rfm::core::TransferProgress& progress)
{
    m_transferPanel->updateTransfer(progress);
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
        rfm::core::RemotePath::normalize(rfm::core::RemotePath::parent(progress.destination)) ==
            rfm::core::RemotePath::normalize(m_currentPath)) {
        scheduleCurrentDirectoryRefresh(false);
    }
}

void MainWindow::updateConnectionAction()
{
    m_newConnectionAction->setEnabled(!m_busy && m_nonTerminalTransfers.isEmpty());
}

void MainWindow::requestDirectoryListing(const QString& path, bool showBusy, bool deferIfActive)
{
    if (!m_connected || path.isEmpty()) {
        return;
    }
    if (m_listingInProgress || (m_busy && !showBusy)) {
        if (deferIfActive && (!m_deferredDirectoryBusy || showBusy)) {
            m_deferredDirectoryPath = path;
            m_deferredDirectoryBusy = m_deferredDirectoryBusy || showBusy;
        }
        return;
    }

    m_listingInProgress = true;
    if (showBusy) {
        setBusy(true, tr("Refreshing %1…").arg(path));
    }
    emit directoryRequested(path);
}

void MainWindow::scheduleCurrentDirectoryRefresh(bool showBusy)
{
    if (!m_connected || m_currentPath.isEmpty()) {
        return;
    }
    m_scheduledRefreshBusy = m_scheduledRefreshBusy || showBusy;
    m_refreshDebounceTimer->start();
}

void MainWindow::stopAutomaticRefresh()
{
    if (m_autoRefreshTimer != nullptr) {
        m_autoRefreshTimer->stop();
    }
    if (m_refreshDebounceTimer != nullptr) {
        m_refreshDebounceTimer->stop();
    }
    m_listingInProgress = false;
    m_deferredDirectoryPath.clear();
    m_deferredDirectoryBusy = false;
    m_scheduledRefreshBusy = false;
}

void MainWindow::updateOperationActions()
{
    const qsizetype count = selectedEntries().size();
    const bool available = m_connected && !m_busy;
    m_createDirectoryAction->setEnabled(available);
    m_renameAction->setEnabled(available && count == 1);
    m_moveAction->setEnabled(available && count > 0);
    m_copyAction->setEnabled(available && count > 0);
    m_removeAction->setEnabled(available && count > 0);
    m_uploadAction->setEnabled(available);
    m_downloadAction->setEnabled(available && count > 0);
}

QList<rfm::core::RemoteSelection> MainWindow::selectedEntries() const
{
    QList<rfm::core::RemoteSelection> selection;
    if (m_fileTable == nullptr || m_fileTable->selectionModel() == nullptr) {
        return selection;
    }
    const QModelIndexList rows = m_fileTable->selectionModel()->selectedRows(0);
    for (const QModelIndex& index : rows) {
        const QTableWidgetItem* const item = m_fileTable->item(index.row(), 0);
        if (item != nullptr) {
            selection.push_back({rfm::core::RemotePath::join(m_currentPath, item->text()),
                                 item->data(Qt::UserRole).toBool()});
        }
    }
    return selection;
}

QString MainWindow::askDestination(const QString& title)
{
    bool accepted = false;
    const QString value = QInputDialog::getText(this, title, tr("Remote destination folder:"),
                                                QLineEdit::Normal, m_currentPath, &accepted);
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

void MainWindow::openSelectedEntry(int row, int /* column */)
{
    const QTableWidgetItem* const item = m_fileTable->item(row, 0);
    if (item == nullptr || !item->data(Qt::UserRole).toBool()) {
        return;
    }
    const QString nextPath = m_currentPath == QStringLiteral(".")
                                 ? QStringLiteral("./%1").arg(item->text())
                                 : QStringLiteral("%1/%2").arg(m_currentPath, item->text());
    requestDirectoryListing(nextPath, true, true);
}

void MainWindow::requestParentDirectory()
{
    if (m_currentPath.isEmpty() || m_currentPath == QStringLiteral(".")) {
        return;
    }
    QString parent = rfm::core::RemotePath::parent(m_currentPath);
    if (parent.isEmpty()) {
        parent = QStringLiteral(".");
    }
    requestDirectoryListing(parent, true, true);
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
    m_upAction->setEnabled(!busy && m_connected && m_currentPath != QStringLiteral("."));
    m_refreshAction->setEnabled(!busy && m_connected);
    if (m_fileTable != nullptr) {
        m_fileTable->setEnabled(!busy);
    }
    if (busy && !message.isEmpty()) {
        statusBar()->showMessage(message);
    }
    updateOperationActions();

    if (!m_busy && !m_listingInProgress && !m_deferredDirectoryPath.isEmpty()) {
        const QString deferredPath = std::exchange(m_deferredDirectoryPath, {});
        const bool showBusy = std::exchange(m_deferredDirectoryBusy, false);
        requestDirectoryListing(deferredPath, showBusy, true);
    }
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
