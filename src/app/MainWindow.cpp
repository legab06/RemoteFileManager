#include "remotefilemanager/app/MainWindow.hpp"

#include "remotefilemanager/app/ConnectionDialog.hpp"
#include "remotefilemanager/ssh/LibsshRuntime.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QDialog>
#include <QDir>
#include <QFileIconProvider>
#include <QFont>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <limits>

namespace rfm::app {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
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
    createEmptyState();

    qRegisterMetaType<rfm::core::ConnectionProfile>();
    qRegisterMetaType<QList<rfm::core::RemoteEntry>>();
    m_sshThread = new QThread(this);
    m_sshSession = new rfm::ssh::SshSession;
    m_sshSession->moveToThread(m_sshThread);
    connect(m_sshThread, &QThread::finished, m_sshSession, &QObject::deleteLater);
    connect(this, &MainWindow::connectionRequested,
            m_sshSession, &rfm::ssh::SshSession::connectToHost);
    connect(this, &MainWindow::hostKeyDecision,
            m_sshSession, &rfm::ssh::SshSession::confirmUnknownHost);
    connect(this, &MainWindow::directoryRequested,
            m_sshSession, &rfm::ssh::SshSession::listDirectory);
    connect(this, &MainWindow::disconnectionRequested,
            m_sshSession, &rfm::ssh::SshSession::disconnectFromHost);
    connect(m_sshSession, &rfm::ssh::SshSession::hostKeyConfirmationRequired,
            this, &MainWindow::showHostKeyConfirmation);
    connect(m_sshSession, &rfm::ssh::SshSession::connected,
            this, [this](const QString& path, const QList<rfm::core::RemoteEntry>& entries) {
                showRemoteDirectory(path, entries);
                statusBar()->showMessage(tr("Connected securely to %1").arg(m_activeProfile.host));
            });
    connect(m_sshSession, &rfm::ssh::SshSession::directoryListed,
            this, &MainWindow::showRemoteDirectory);
    connect(m_sshSession, &rfm::ssh::SshSession::failed,
            this, &MainWindow::showConnectionError);
    m_sshThread->start();

    statusBar()->showMessage(
        tr("Disconnected · libssh %1").arg(rfm::ssh::LibsshRuntime::version()));
}

MainWindow::~MainWindow()
{
    if (m_sshThread != nullptr && m_sshThread->isRunning()) {
        QMetaObject::invokeMethod(m_sshSession, &rfm::ssh::SshSession::disconnectFromHost,
                                  Qt::BlockingQueuedConnection);
        m_sshThread->quit();
        m_sshThread->wait();
    }
}

void MainWindow::createActions()
{
    m_newConnectionAction = new QAction(
        style()->standardIcon(QStyle::SP_ComputerIcon), tr("New connection…"), this);
    m_newConnectionAction->setObjectName(QStringLiteral("newConnectionAction"));
    m_newConnectionAction->setShortcut(QKeySequence::New);
    connect(m_newConnectionAction, &QAction::triggered, this, &MainWindow::showConnectionDialog);

    m_quitAction = new QAction(tr("Quit"), this);
    m_quitAction->setShortcut(QKeySequence::Quit);
    connect(m_quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_aboutAction = new QAction(tr("About RemoteFileManager"), this);
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAboutDialog);
}

void MainWindow::createMenus()
{
    QMenu* const fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_newConnectionAction);
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

    m_backAction = navigationBar->addAction(
        style()->standardIcon(QStyle::SP_ArrowBack), tr("Back"));
    m_forwardAction = navigationBar->addAction(
        style()->standardIcon(QStyle::SP_ArrowForward), tr("Forward"));
    m_upAction = navigationBar->addAction(
        style()->standardIcon(QStyle::SP_ArrowUp), tr("Parent folder"));
    m_refreshAction = navigationBar->addAction(
        style()->standardIcon(QStyle::SP_BrowserReload), tr("Refresh"));

    m_backAction->setEnabled(false);
    m_forwardAction->setEnabled(false);
    m_upAction->setEnabled(false);
    m_refreshAction->setEnabled(false);
    connect(m_upAction, &QAction::triggered, this, &MainWindow::requestParentDirectory);
    connect(m_refreshAction, &QAction::triggered, this, [this] {
        if (!m_currentPath.isEmpty()) {
            setBusy(true, tr("Refreshing %1…").arg(m_currentPath));
            emit directoryRequested(m_currentPath);
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
    auto* const remoteItem = new QListWidgetItem(
        style()->standardIcon(QStyle::SP_ComputerIcon), tr("Remote server"), placesList);
    remoteItem->setFlags(remoteItem->flags() & ~Qt::ItemIsEnabled);

    placesDock->setWidget(placesList);
    addDockWidget(Qt::LeftDockWidgetArea, placesDock);
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

    auto* const connectionButton = new QPushButton(
        style()->standardIcon(QStyle::SP_ComputerIcon), tr("New connection…"), emptyState);
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
        QMessageBox::Warning,
        tr("Unknown SSH host key"),
        tr("This is the first connection to %1.\n\nSHA-256 fingerprint:\n%2\n\n"
           "Verify this fingerprint with the server administrator before continuing.")
            .arg(host, fingerprint),
        QMessageBox::NoButton,
        this);
    auto* const trustButton = confirmation.addButton(tr("Trust and connect"), QMessageBox::AcceptRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.exec();
    const bool accepted = confirmation.clickedButton() == trustButton;
    if (accepted) {
        setBusy(true, tr("Authenticating…"));
    }
    emit hostKeyDecision(accepted);
}

void MainWindow::showRemoteDirectory(
    const QString& path, const QList<rfm::core::RemoteEntry>& entries)
{
    if (m_fileTable == nullptr) {
        m_fileTable = new QTableWidget(this);
        m_fileTable->setObjectName(QStringLiteral("remoteFileTable"));
        m_fileTable->setColumnCount(3);
        m_fileTable->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified")});
        m_fileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_fileTable->setSelectionMode(QAbstractItemView::SingleSelection);
        m_fileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_fileTable->setShowGrid(false);
        m_fileTable->verticalHeader()->hide();
        m_fileTable->horizontalHeader()->setStretchLastSection(true);
        m_fileTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        connect(m_fileTable, &QTableWidget::cellDoubleClicked,
                this, &MainWindow::openSelectedEntry);
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
        const qint64 displaySize = entry.size > static_cast<quint64>(std::numeric_limits<qint64>::max())
            ? std::numeric_limits<qint64>::max()
            : static_cast<qint64>(entry.size);
        auto* const sizeItem = new QTableWidgetItem(
            entry.directory ? QString{} : QLocale{}.formattedDataSize(displaySize));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_fileTable->setItem(static_cast<int>(row), 1, sizeItem);
        m_fileTable->setItem(static_cast<int>(row), 2,
                             new QTableWidgetItem(QLocale{}.toString(entry.modifiedAt, QLocale::ShortFormat)));
    }
    m_remotePathEdit->setText(
        QStringLiteral("sftp://%1@%2:%3/%4")
            .arg(m_activeProfile.username, m_activeProfile.host)
            .arg(m_activeProfile.port)
            .arg(path == QStringLiteral(".") ? QString{} : path));
    m_upAction->setEnabled(path != QStringLiteral("."));
    m_refreshAction->setEnabled(true);
    setBusy(false);
}

void MainWindow::showConnectionError(const QString& message)
{
    setBusy(false);
    statusBar()->showMessage(tr("Disconnected"));
    QMessageBox::critical(this, tr("SSH connection error"), message);
}

void MainWindow::openSelectedEntry(int row, int /* column */)
{
    const QTableWidgetItem* const item = m_fileTable->item(row, 0);
    if (item == nullptr || !item->data(Qt::UserRole).toBool()) {
        return;
    }
    const QString nextPath = m_currentPath == QStringLiteral(".")
        ? QStringLiteral("./%1").arg(item->text())
        : QStringLiteral("%1/%2").arg(m_currentPath, item->text());
    setBusy(true, tr("Opening %1…").arg(nextPath));
    emit directoryRequested(nextPath);
}

void MainWindow::requestParentDirectory()
{
    if (m_currentPath.isEmpty() || m_currentPath == QStringLiteral(".")) {
        return;
    }
    QString parent = QDir::cleanPath(m_currentPath + QStringLiteral("/.."));
    if (parent.isEmpty()) {
        parent = QStringLiteral(".");
    }
    setBusy(true, tr("Opening %1…").arg(parent));
    emit directoryRequested(parent);
}

void MainWindow::setBusy(bool busy, const QString& message)
{
    m_newConnectionAction->setEnabled(!busy);
    if (m_fileTable != nullptr) {
        m_fileTable->setEnabled(!busy);
    }
    if (busy && !message.isEmpty()) {
        statusBar()->showMessage(message);
    }
}

void MainWindow::showAboutDialog()
{
    QMessageBox::about(
        this,
        tr("About RemoteFileManager"),
        tr("RemoteFileManager %1\n\nA native file manager for standard SSH/SFTP servers.\n"
           "Sprint 1: secure SSH connection and SFTP browsing.")
            .arg(QApplication::applicationVersion()));
}

}  // namespace rfm::app
