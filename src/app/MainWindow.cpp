#include "remotefilemanager/app/MainWindow.hpp"

#include "remotefilemanager/ssh/LibsshRuntime.hpp"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFont>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

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

    statusBar()->showMessage(
        tr("Disconnected · libssh %1").arg(rfm::ssh::LibsshRuntime::version()));
}

void MainWindow::createActions()
{
    m_newConnectionAction = new QAction(
        style()->standardIcon(QStyle::SP_ComputerIcon), tr("New connection…"), this);
    m_newConnectionAction->setObjectName(QStringLiteral("newConnectionAction"));
    m_newConnectionAction->setShortcut(QKeySequence::New);
    connect(m_newConnectionAction, &QAction::triggered, this, &MainWindow::showConnectionPlaceholder);

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

    connect(connectionButton, &QPushButton::clicked, this, &MainWindow::showConnectionPlaceholder);
    setCentralWidget(emptyState);
}

void MainWindow::showConnectionPlaceholder()
{
    QMessageBox::information(
        this,
        tr("New connection"),
        tr("The secure SSH connection screen will be implemented in Sprint 1."));
}

void MainWindow::showAboutDialog()
{
    QMessageBox::about(
        this,
        tr("About RemoteFileManager"),
        tr("RemoteFileManager %1\n\nA native file manager for standard SSH/SFTP servers.")
            .arg(QApplication::applicationVersion()));
}

}  // namespace rfm::app
