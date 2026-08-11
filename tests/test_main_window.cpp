#include "remotefilemanager/app/ConnectionDialog.hpp"
#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/app/HomePage.hpp"
#include "remotefilemanager/app/MainWindow.hpp"
#include "remotefilemanager/app/OperationPanel.hpp"
#include "remotefilemanager/app/PaneWorkspace.hpp"
#include "remotefilemanager/app/ServerProfileDialog.hpp"
#include "remotefilemanager/app/TransferRequestFactory.hpp"
#include "remotefilemanager/core/OperationHistoryStore.hpp"
#include "remotefilemanager/core/InternalTransfer.hpp"
#include "remotefilemanager/core/ServerProfileStore.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>

namespace
{

class RecordingRemoteBackend final : public rfm::core::RemoteFileBackend
{
  public:
    rfm::core::RemoteProbeResult probe(const QString&) override
    {
        return {{rfm::core::RemoteBackendError::NotFound, {}}, {}};
    }
    rfm::core::RemoteDirectoryResult list(const QString&) override { return {}; }
    rfm::core::RemoteBackendResult createDirectory(const QString&) override { return {}; }
    rfm::core::RemoteBackendResult rename(const QString& source,
                                          const QString& destination) override
    {
        lastSource = source;
        lastDestination = destination;
        return {};
    }
    rfm::core::RemoteBackendResult removeFile(const QString&) override { return {}; }
    rfm::core::RemoteBackendResult removeDirectory(const QString&) override { return {}; }
    rfm::core::RemoteBackendResult copyOnServer(const QString& source,
                                                const QString& destination, bool) override
    {
        lastSource = source;
        lastDestination = destination;
        return {};
    }

    QString lastSource;
    QString lastDestination;
};

rfm::core::TransferProgress progress(quint64 id, rfm::core::TransferState state,
                                     quint64 transferred = 0, quint64 total = 0, quint64 speed = 0)
{
    rfm::core::TransferProgress value;
    value.id = id;
    value.state = state;
    value.source = QStringLiteral("/tmp/archive.bin");
    value.destination = QStringLiteral("/srv/archive.bin");
    value.transferredBytes = transferred;
    value.totalBytes = total;
    value.bytesPerSecond = speed;
    value.direction = rfm::core::TransferDirection::Upload;
    return value;
}

int rowForId(QTableWidget* table, quint64 id)
{
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->data(Qt::UserRole).toULongLong() == id) {
            return row;
        }
    }
    return -1;
}

void acceptNextQuestion()
{
    QTimer::singleShot(0, [] {
        auto* const messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(messageBox != nullptr);
        QAbstractButton* const yesButton = messageBox->button(QMessageBox::Yes);
        QVERIFY(yesButton != nullptr);
        yesButton->click();
    });
}

void setConnectionIdentity(rfm::app::MainWindow& window)
{
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    window.findChild<QAction*>(QStringLiteral("newConnectionAction"))->trigger();
    auto* dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    dialog->findChild<QLineEdit*>(QStringLiteral("hostEdit"))
        ->setText(QStringLiteral("history.example.test"));
    dialog->findChild<QLineEdit*>(QStringLiteral("usernameEdit"))
        ->setText(QStringLiteral("test-user"));
    dialog->findChild<QSpinBox*>(QStringLiteral("portSpin"))->setValue(2222);
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
}

} // namespace

class MainWindowTest final : public QObject
{
    Q_OBJECT

  private slots:
    void exposesInitialDisconnectedShell();
    void validatesSecureConnectionForm();
    void keepsConnectionDialogOpenAcrossFailureAndRetry();
    void loadsSavedServersAndPrefillsQuickConnection();
    void placesButtonTracksActiveAndSelectedProfiles();
    void matchesSavedProfileAgainstLiveConnectionSettings_data();
    void matchesSavedProfileAgainstLiveConnectionSettings();
    void addsEditsAndRemovesSavedServers();
    void savesManualServerOnlyAfterSuccessAndAvoidsDuplicates();
    void disconnectActionFollowsSessionLifecycle();
    void enablesMultipleRemoteSelection();
    void buildsPortableTransferRequests();
    void queuesFilesAndFoldersAsSeparateUploads();
    void queuesDownloadsFromRemoteSelection();
    void displaysTransferProgressAndMultipleEntries();
    void displaysTransferStatesInEnglish();
    void displaysRemoteCopyAndMoveOperations();
    void removesOnlyTerminalOperationsFromPanel();
    void exposesPauseResumeAndCancelIntentions();
    void displaysTerminalTransferStatesAndErrors();
    void formatsTransferSizesAndSpeeds();
    void refreshTimerIsConnectionAwareAndCoalescesListings();
    void refreshesAfterCompletedUpload();
    void appliesOnlyExpectedDirectoryResult();
    void distinguishesRequestsForSamePath();
    void newNavigationMakesActiveResultObsolete();
    void handlesOnlyExpectedListingErrorWithoutDisconnecting();
    void symbolicLinkNavigationFailureDoesNotDisconnect();
    void splitRoutesSerializedListingsPerPane();
    void activePaneOwnsNavigationRefreshAndUploadTargets();
    void navigationKeepsInitiatingPaneActive_data();
    void navigationKeepsInitiatingPaneActive();
    void splitListingErrorLeavesOtherPaneUntouched();
    void historyActionsFollowActivePaneAndIgnoreFailedOrObsoleteListings();
    void copiesAndMovesSelectionToOtherPane();
    void contextMenuUsesSharedInterPaneActions();
    void contextMenuUsesClipboardAndKeepsExplicitDestinationActions();
    void buildsCanonicalInterPanePathsThroughTheRealUiChain();
    void rejectsOtherPaneOperationsForSameDirectory();
    void refreshesAllVisiblePanesAffectedByOperationsAndUploads();
    void persistsRemovesAndClearsTerminalOperationHistory();
    void clipboardCopiesCutsPastesAndClearsSuccessfulMove();
    void dragDropOffersCopyMoveAndCancelWithoutDuplicateBackendKinds();
    void keyboardActionsExposeShortcutsAndTargetTheActivePane();
};

void MainWindowTest::exposesInitialDisconnectedShell()
{
    rfm::app::MainWindow window;

    QCOMPARE(window.objectName(), QStringLiteral("mainWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("RemoteFileManager"));

    const auto* const pathEdit = window.findChild<QLineEdit*>(QStringLiteral("remotePathEdit"));
    QVERIFY(pathEdit != nullptr);
    QVERIFY(pathEdit->isReadOnly());

    const auto* const connectionButton =
        window.findChild<QPushButton*>(QStringLiteral("homeNewConnectionButton"));
    QVERIFY(connectionButton != nullptr);
    QVERIFY(connectionButton->isEnabled());
    auto* const stack = window.findChild<QStackedWidget*>(QStringLiteral("centralStack"));
    auto* const home = window.findChild<rfm::app::HomePage*>();
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(stack != nullptr);
    QVERIFY(home != nullptr);
    QVERIFY(workspace != nullptr);
    QCOMPARE(stack->currentWidget(), home);
    QVERIFY(stack->currentWidget() != workspace);

    const auto* const operationPanel =
        window.findChild<rfm::app::OperationPanel*>(QStringLiteral("operationPanel"));
    const auto* const operationTable =
        window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(operationPanel != nullptr);
    QVERIFY(operationTable != nullptr);
    QCOMPARE(operationTable->rowCount(), 0);
    const auto* const uploadAction = window.findChild<QAction*>(QStringLiteral("uploadAction"));
    const auto* const downloadAction = window.findChild<QAction*>(QStringLiteral("downloadAction"));
    QVERIFY(uploadAction != nullptr);
    QVERIFY(downloadAction != nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("uploadFilesAction")) == nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("uploadFolderAction")) == nullptr);
    QVERIFY(uploadAction->menu() == nullptr);
    QVERIFY(!uploadAction->isEnabled());
    QVERIFY(!downloadAction->isEnabled());
    const auto* const refreshTimer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(refreshTimer != nullptr);
    QCOMPARE(refreshTimer->interval(), 3000);
    QVERIFY(!refreshTimer->isActive());
}

void MainWindowTest::enablesMultipleRemoteSelection()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("first.txt"), 10, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false},
    };
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral(".")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("remoteFileTable"));
    QVERIFY(table != nullptr);
    QCOMPARE(table->selectionMode(), QAbstractItemView::ExtendedSelection);
    table->selectionModel()->select(table->model()->index(0, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(1, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(table->selectionModel()->selectedRows(0).size(), 2);
    auto* const createAction = window.findChild<QAction*>(QStringLiteral("createDirectoryAction"));
    auto* const renameAction = window.findChild<QAction*>(QStringLiteral("renameAction"));
    auto* const moveAction = window.findChild<QAction*>(QStringLiteral("moveAction"));
    auto* const copyAction = window.findChild<QAction*>(QStringLiteral("copyAction"));
    auto* const removeAction = window.findChild<QAction*>(QStringLiteral("removeAction"));
    auto* const uploadAction = window.findChild<QAction*>(QStringLiteral("uploadAction"));
    auto* const downloadAction = window.findChild<QAction*>(QStringLiteral("downloadAction"));
    QVERIFY(createAction != nullptr);
    QVERIFY(renameAction != nullptr);
    QVERIFY(moveAction != nullptr);
    QVERIFY(copyAction != nullptr);
    QVERIFY(removeAction != nullptr);
    QVERIFY(uploadAction != nullptr);
    QVERIFY(downloadAction != nullptr);
    QVERIFY(createAction->isEnabled());
    QVERIFY(!renameAction->isEnabled());
    QVERIFY(moveAction->isEnabled());
    QVERIFY(copyAction->isEnabled());
    QVERIFY(removeAction->isEnabled());
    QVERIFY(uploadAction->isEnabled());
    QVERIFY(downloadAction->isEnabled());
    QCOMPARE(uploadAction->text(), QStringLiteral("Upload"));
    QCOMPARE(downloadAction->text(), QStringLiteral("Download"));
    QCOMPARE(uploadAction->toolTip(), QStringLiteral("Upload files or folders to the server"));
    QCOMPARE(downloadAction->toolTip(), QStringLiteral("Download the selection to this computer"));
    QVERIFY(!uploadAction->icon().isNull());
    QVERIFY(!downloadAction->icon().isNull());
    QVERIFY(uploadAction->icon().cacheKey() != downloadAction->icon().cacheKey());
    const auto* const refreshTimer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(refreshTimer != nullptr);
    QVERIFY(refreshTimer->isActive());
}

void MainWindowTest::validatesSecureConnectionForm()
{
    rfm::app::ConnectionDialog dialog;
    auto* const host = dialog.findChild<QLineEdit*>(QStringLiteral("hostEdit"));
    auto* const user = dialog.findChild<QLineEdit*>(QStringLiteral("usernameEdit"));
    auto* const port = dialog.findChild<QSpinBox*>(QStringLiteral("portSpin"));
    auto* const buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(host != nullptr);
    QVERIFY(user != nullptr);
    QVERIFY(port != nullptr);
    QVERIFY(buttons != nullptr);
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());

    host->setText(QStringLiteral("server.example.test"));
    user->setText(QStringLiteral("gabriel"));
    QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QCOMPARE(dialog.profile().port, quint16{22});
    QCOMPARE(dialog.profile().effectiveDisplayName(),
             QStringLiteral("gabriel@server.example.test"));
}

void MainWindowTest::keepsConnectionDialogOpenAcrossFailureAndRetry()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::connectionRequested);
    window.findChild<QAction*>(QStringLiteral("newConnectionAction"))->trigger();
    auto* const dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    auto* const host = dialog->findChild<QLineEdit*>(QStringLiteral("hostEdit"));
    auto* const user = dialog->findChild<QLineEdit*>(QStringLiteral("usernameEdit"));
    auto* const buttons = dialog->findChild<QDialogButtonBox*>();
    host->setText(QStringLiteral("wrong.example.test"));
    user->setText(QStringLiteral("alice"));
    buttons->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(dialog->state(), rfm::app::ConnectionDialog::State::Connecting);

    QVERIFY(QMetaObject::invokeMethod(
        &window, "showConnectionError", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("Host unreachable"))));
    QCOMPARE(dialog->state(), rfm::app::ConnectionDialog::State::Error);
    QVERIFY(dialog->isVisible());
    QCOMPARE(host->text(), QStringLiteral("wrong.example.test"));
    host->setText(QStringLiteral("correct.example.test"));
    buttons->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(requested.size(), 2);

    QSignalSpy accepted(dialog, &QDialog::accepted);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QCOMPARE(accepted.size(), 1);
}

void MainWindowTest::loadsSavedServersAndPrefillsQuickConnection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    const rfm::core::ConnectionProfile saved{
        QStringLiteral("Home NAS"), QStringLiteral("nas.example.test"),
        QStringLiteral("alice"), 2222, QStringLiteral("nas-id"), true};
    QString error;
    QVERIFY2(store.save({saved}, &error), qPrintable(error));

    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    auto* const list = window.findChild<QListWidget*>(QStringLiteral("serverProfileList"));
    auto* const homeList = window.findChild<QListWidget*>(QStringLiteral("homeServerList"));
    QVERIFY(list != nullptr);
    QVERIFY(homeList != nullptr);
    QCOMPARE(list->count(), 1);
    QCOMPARE(list->item(0)->text(), QStringLiteral("Home NAS"));
    QCOMPARE(homeList->count(), 1);
    QVERIFY(homeList->item(0)->text().startsWith(QStringLiteral("Home NAS\n")));
    homeList->setCurrentRow(0);
    auto* const connectButton = window.findChild<QPushButton*>(QStringLiteral("homeConnectButton"));
    QVERIFY(connectButton != nullptr);
    QVERIFY(connectButton->isEnabled());
    connectButton->click();

    auto* const dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    QCOMPARE(dialog->findChild<QLineEdit*>(QStringLiteral("hostEdit"))->text(),
             QStringLiteral("nas.example.test"));
    QCOMPARE(dialog->findChild<QLineEdit*>(QStringLiteral("usernameEdit"))->text(),
             QStringLiteral("alice"));
    QCOMPARE(dialog->findChild<QSpinBox*>(QStringLiteral("portSpin"))->value(), 2222);
    QVERIFY(dialog->profile().allowPasswordFallback);
    QVERIFY(dialog->password().isEmpty());
    QVERIFY(dialog->findChild<QCheckBox*>(QStringLiteral("saveServerCheck"))->isHidden());
}

void MainWindowTest::placesButtonTracksActiveAndSelectedProfiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    const QList profiles{
        rfm::core::ConnectionProfile{QStringLiteral("Active server"),
                                     QStringLiteral("active.example.test"),
                                     QStringLiteral("alice"), 22, QStringLiteral("active-id")},
        rfm::core::ConnectionProfile{QStringLiteral("Other server"),
                                     QStringLiteral("other.example.test"),
                                     QStringLiteral("alice"), 22, QStringLiteral("other-id")}};
    QString error;
    QVERIFY2(store.save(profiles, &error), qPrintable(error));

    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    auto* const list = window.findChild<QListWidget*>(QStringLiteral("serverProfileList"));
    auto* const button =
        window.findChild<QPushButton*>(QStringLiteral("connectServerProfileButton"));
    QVERIFY(list != nullptr);
    QVERIFY(button != nullptr);
    list->setCurrentRow(0);
    QCOMPARE(button->text(), QStringLiteral("Connect"));
    QVERIFY(button->isEnabled());
    button->click();
    auto* dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));

    QCOMPARE(button->text(), QStringLiteral("Disconnect"));
    QVERIFY(button->isEnabled());
    QVERIFY(list->item(0)->text().contains(QStringLiteral("Connected")));
    QVERIFY(list->item(0)->data(Qt::UserRole + 1).toBool());

    QObject::disconnect(&window, &rfm::app::MainWindow::disconnectionRequested, nullptr, nullptr);
    QSignalSpy disconnectRequested(&window, &rfm::app::MainWindow::disconnectionRequested);
    button->click();
    QCOMPARE(disconnectRequested.size(), 1);
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QCOMPARE(button->text(), QStringLiteral("Connect"));
    QVERIFY(button->isEnabled());
    QVERIFY(!list->item(0)->text().contains(QStringLiteral("Connected")));

    QTRY_VERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);
    button->click();
    dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    list->setCurrentRow(1);
    QCOMPARE(button->text(), QStringLiteral("Connect"));
    QVERIFY(!button->isEnabled());
    QSignalSpy secondConnection(&window, &rfm::app::MainWindow::connectionRequested);
    button->click();
    QCOMPARE(secondConnection.size(), 0);
}

void MainWindowTest::matchesSavedProfileAgainstLiveConnectionSettings_data()
{
    QTest::addColumn<QString>("host");
    QTest::addColumn<QString>("username");
    QTest::addColumn<int>("port");
    QTest::addColumn<bool>("savedProfileIsConnected");

    QTest::newRow("saved profile without edits") << QStringLiteral("saved.example.test")
                                                  << QStringLiteral("alice") << 22 << true;
    QTest::newRow("edited host") << QStringLiteral("other.example.test")
                                  << QStringLiteral("alice") << 22 << false;
    QTest::newRow("edited username") << QStringLiteral("saved.example.test")
                                      << QStringLiteral("bob") << 22 << false;
    QTest::newRow("edited port") << QStringLiteral("saved.example.test")
                                  << QStringLiteral("alice") << 2222 << false;
}

void MainWindowTest::matchesSavedProfileAgainstLiveConnectionSettings()
{
    QFETCH(QString, host);
    QFETCH(QString, username);
    QFETCH(int, port);
    QFETCH(bool, savedProfileIsConnected);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    const rfm::core::ConnectionProfile saved{
        QStringLiteral("Saved server"), QStringLiteral("saved.example.test"),
        QStringLiteral("alice"), 22, QStringLiteral("stable-profile-id")};
    QString error;
    QVERIFY2(store.save({saved}, &error), qPrintable(error));

    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    QSignalSpy connectionRequested(&window, &rfm::app::MainWindow::connectionRequested);
    auto* const list = window.findChild<QListWidget*>(QStringLiteral("serverProfileList"));
    auto* const profileButton =
        window.findChild<QPushButton*>(QStringLiteral("connectServerProfileButton"));
    auto* const disconnectAction =
        window.findChild<QAction*>(QStringLiteral("disconnectAction"));
    QVERIFY(list != nullptr);
    QVERIFY(profileButton != nullptr);
    QVERIFY(disconnectAction != nullptr);

    list->setCurrentRow(0);
    profileButton->click();
    auto* const dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    dialog->findChild<QLineEdit*>(QStringLiteral("hostEdit"))->setText(host);
    dialog->findChild<QLineEdit*>(QStringLiteral("usernameEdit"))->setText(username);
    dialog->findChild<QSpinBox*>(QStringLiteral("portSpin"))->setValue(port);
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();

    QCOMPARE(connectionRequested.size(), 1);
    const auto liveProfile =
        qvariant_cast<rfm::core::ConnectionProfile>(connectionRequested.constFirst().constFirst());
    QCOMPARE(liveProfile.id, saved.id);
    QCOMPARE(liveProfile.host, host);
    QCOMPARE(liveProfile.username, username);
    QCOMPARE(liveProfile.port, static_cast<quint16>(port));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));

    QCOMPARE(list->item(0)->data(Qt::UserRole + 1).toBool(), savedProfileIsConnected);
    QCOMPARE(list->item(0)->text().contains(QStringLiteral("Connected")),
             savedProfileIsConnected);
    QCOMPARE(profileButton->text(),
             savedProfileIsConnected ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
    QCOMPARE(profileButton->isEnabled(), savedProfileIsConnected);

    QObject::disconnect(&window, &rfm::app::MainWindow::disconnectionRequested, nullptr, nullptr);
    QSignalSpy disconnectionRequested(&window, &rfm::app::MainWindow::disconnectionRequested);
    profileButton->click();
    QCOMPARE(disconnectionRequested.size(), savedProfileIsConnected ? 1 : 0);
    if (!savedProfileIsConnected) {
        QVERIFY(disconnectAction->isEnabled());
        disconnectAction->trigger();
        QCOMPARE(disconnectionRequested.size(), 1);
    }
}

void MainWindowTest::addsEditsAndRemovesSavedServers()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    auto* const list = window.findChild<QListWidget*>(QStringLiteral("serverProfileList"));
    auto* const homeList = window.findChild<QListWidget*>(QStringLiteral("homeServerList"));
    auto* const add =
        window.findChild<QPushButton*>(QStringLiteral("addServerProfileButton"));
    auto* const edit =
        window.findChild<QPushButton*>(QStringLiteral("editServerProfileButton"));
    auto* const remove =
        window.findChild<QPushButton*>(QStringLiteral("removeServerProfileButton"));
    QVERIFY(list != nullptr);
    QVERIFY(homeList != nullptr);
    QVERIFY(add != nullptr);
    QVERIFY(edit != nullptr);
    QVERIFY(remove != nullptr);

    QTimer::singleShot(0, [] {
        auto* const dialog =
            qobject_cast<rfm::app::ServerProfileDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr);
        dialog->findChild<QLineEdit*>(QStringLiteral("profileNameEdit"))
            ->setText(QStringLiteral("Test server"));
        dialog->findChild<QLineEdit*>(QStringLiteral("profileHostEdit"))
            ->setText(QStringLiteral("first.example.test"));
        dialog->findChild<QLineEdit*>(QStringLiteral("profileUsernameEdit"))
            ->setText(QStringLiteral("alice"));
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    add->click();
    QCOMPARE(list->count(), 1);
    QCOMPARE(list->item(0)->text(), QStringLiteral("Test server"));
    QCOMPARE(homeList->count(), 1);
    QVERIFY(homeList->item(0)->text().startsWith(QStringLiteral("Test server\n")));

    list->setCurrentRow(0);
    QVERIFY(edit->isEnabled());
    QTimer::singleShot(0, [] {
        auto* const dialog =
            qobject_cast<rfm::app::ServerProfileDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr);
        dialog->findChild<QLineEdit*>(QStringLiteral("profileNameEdit"))
            ->setText(QStringLiteral("Updated server"));
        dialog->findChild<QLineEdit*>(QStringLiteral("profileHostEdit"))
            ->setText(QStringLiteral("updated.example.test"));
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    edit->click();
    QCOMPARE(list->count(), 1);
    QCOMPARE(list->item(0)->text(), QStringLiteral("Updated server"));
    QCOMPARE(homeList->count(), 1);
    QVERIFY(homeList->item(0)->text().startsWith(QStringLiteral("Updated server\n")));
    QString error;
    const rfm::core::ServerProfileStore store(temporary.path());
    const QList saved = store.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.constFirst().host, QStringLiteral("updated.example.test"));

    acceptNextQuestion();
    remove->click();
    QCOMPARE(store.load(&error).size(), 0);
    QVERIFY(error.isEmpty());
    QCOMPARE(homeList->count(), 1);
    QCOMPARE(homeList->item(0)->text(), QStringLiteral("No saved servers yet"));
}

void MainWindowTest::savesManualServerOnlyAfterSuccessAndAvoidsDuplicates()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    const rfm::core::ServerProfileStore store(temporary.path());
    QString error;
    const QString ephemeralSecret(18, QChar{'x'});

    const auto openManualConnection = [&window, &ephemeralSecret] {
        window.findChild<QAction*>(QStringLiteral("newConnectionAction"))->trigger();
        auto* const dialog = window.findChild<rfm::app::ConnectionDialog*>();
        if (dialog == nullptr) {
            return dialog;
        }
        dialog->findChild<QLineEdit*>(QStringLiteral("hostEdit"))
            ->setText(QStringLiteral("saved.example.test"));
        dialog->findChild<QLineEdit*>(QStringLiteral("usernameEdit"))
            ->setText(QStringLiteral("alice"));
        dialog->findChild<QCheckBox*>(QStringLiteral("passwordFallbackCheck"))->setChecked(true);
        dialog->findChild<QLineEdit*>(QStringLiteral("passwordEdit"))
            ->setText(ephemeralSecret);
        dialog->findChild<QCheckBox*>(QStringLiteral("saveServerCheck"))->setChecked(true);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        return dialog;
    };

    rfm::app::ConnectionDialog* dialog = openManualConnection();
    QVERIFY(dialog != nullptr);
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(error.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(
        &window, "showConnectionError", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("Authentication failed"))));
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(error.isEmpty());

    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QList saved = store.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(saved.size(), 1);
    QVERIFY(saved.constFirst().isValidSavedProfile());
    QCOMPARE(saved.constFirst().host, QStringLiteral("saved.example.test"));
    QVERIFY(saved.constFirst().allowPasswordFallback);
    QFile serialized(store.filePath());
    QVERIFY(serialized.open(QIODevice::ReadOnly));
    QVERIFY(!serialized.readAll().contains(ephemeralSecret.toUtf8()));

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QTRY_VERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);
    dialog = openManualConnection();
    QVERIFY(dialog != nullptr);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    saved = store.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(saved.size(), 1);
}

void MainWindowTest::disconnectActionFollowsSessionLifecycle()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    const rfm::core::ConnectionProfile saved{
        QStringLiteral("Disconnect test"), QStringLiteral("disconnect.example.test"),
        QStringLiteral("alice"), 22, QStringLiteral("disconnect-id"), false};
    QString error;
    QVERIFY2(store.save({saved}, &error), qPrintable(error));
    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    auto* const disconnect = window.findChild<QAction*>(QStringLiteral("disconnectAction"));
    auto* const refreshTimer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(disconnect != nullptr);
    QVERIFY(refreshTimer != nullptr);
    QVERIFY(!disconnect->isEnabled());
    auto* const stack = window.findChild<QStackedWidget*>(QStringLiteral("centralStack"));
    auto* const home = window.findChild<rfm::app::HomePage*>();
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(stack != nullptr);
    QVERIFY(home != nullptr);
    QVERIFY(workspace != nullptr);

    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    auto* const serverList =
        window.findChild<QListWidget*>(QStringLiteral("serverProfileList"));
    auto* const connectServer =
        window.findChild<QPushButton*>(QStringLiteral("connectServerProfileButton"));
    QVERIFY(serverList != nullptr);
    QVERIFY(connectServer != nullptr);
    serverList->setCurrentRow(0);
    connectServer->click();
    auto* const dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QVERIFY(disconnect->isEnabled());
    QVERIFY(refreshTimer->isActive());
    QCOMPARE(stack->currentWidget(), workspace);

    QObject::disconnect(&window, &rfm::app::MainWindow::disconnectionRequested, nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::disconnectionRequested);
    disconnect->trigger();
    QCOMPARE(requested.size(), 1);
    QVERIFY(!disconnect->isEnabled());

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QVERIFY(!disconnect->isEnabled());
    QVERIFY(!refreshTimer->isActive());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("newConnectionAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("uploadAction"))->isEnabled());
    QVERIFY(!window.findChild<QTableWidget*>(QStringLiteral("remoteFileTable"))->isEnabled());
    QCOMPARE(window.findChild<QTableWidget*>(QStringLiteral("remoteFileTable"))->rowCount(), 0);
    QCOMPARE(stack->currentWidget(), home);
    QVERIFY(stack->currentWidget() != workspace);
    QCOMPARE(store.load(&error).size(), 1);
    QVERIFY(error.isEmpty());
}

void MainWindowTest::buildsPortableTransferRequests()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString filePath = temporary.filePath(QStringLiteral("archive.bin"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("data"), qint64{4});
    file.close();
    const QString folderPath = temporary.filePath(QStringLiteral("photos"));
    QVERIFY(QDir().mkdir(folderPath));

    const auto fileUpload =
        rfm::app::TransferRequestFactory::upload(101, filePath, QStringLiteral("/srv/uploads"));
    QVERIFY(fileUpload.has_value());
    QCOMPARE(fileUpload->destination, QStringLiteral("/srv/uploads/archive.bin"));
    QVERIFY(!fileUpload->directory);

    const auto folderUpload =
        rfm::app::TransferRequestFactory::upload(102, folderPath, QStringLiteral("/srv/uploads"));
    QVERIFY(folderUpload.has_value());
    QCOMPARE(folderUpload->destination, QStringLiteral("/srv/uploads/photos"));
    QVERIFY(folderUpload->directory);

    const auto download = rfm::app::TransferRequestFactory::download(
        103, {QStringLiteral("/srv/photos"), true}, temporary.path());
    QVERIFY(download.has_value());
    QCOMPARE(download->destination, QDir(temporary.path()).filePath(QStringLiteral("photos")));
    QVERIFY(download->directory);

    const QString spacedFilePath = temporary.filePath(QStringLiteral("trailing-space "));
    QFile spacedFile(spacedFilePath);
    QVERIFY(spacedFile.open(QIODevice::WriteOnly));
    spacedFile.close();
    const auto spacedUpload = rfm::app::TransferRequestFactory::upload(
        104, spacedFilePath, QStringLiteral("/srv/uploads "));
    QVERIFY(spacedUpload.has_value());
    QCOMPARE(spacedUpload->source, spacedFilePath);
    QCOMPARE(spacedUpload->destination, QStringLiteral("/srv/uploads /trailing-space "));
    const auto spacedDownload = rfm::app::TransferRequestFactory::download(
        105, {QStringLiteral("/srv/remote-name "), false}, temporary.path());
    QVERIFY(spacedDownload.has_value());
    QCOMPARE(spacedDownload->source, QStringLiteral("/srv/remote-name "));
    QCOMPARE(spacedDownload->destination,
             QDir(temporary.path()).filePath(QStringLiteral("remote-name ")));
}

void MainWindowTest::queuesFilesAndFoldersAsSeparateUploads()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString filePath = temporary.filePath(QStringLiteral("archive.bin"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("data"), qint64{4});
    file.close();
    const QString folderPath = temporary.filePath(QStringLiteral("photos"));
    QVERIFY(QDir().mkdir(folderPath));

    rfm::app::MainWindow window;
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv/uploads")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QSignalSpy requested(&window, &rfm::app::MainWindow::transferRequested);
    const QStringList localPaths{filePath, folderPath};
    QVERIFY(QMetaObject::invokeMethod(&window, "queueUploads", Qt::DirectConnection,
                                      Q_ARG(QStringList, localPaths)));
    QCOMPARE(requested.size(), 2);

    const auto fileRequest = requested.at(0).constFirst().value<rfm::core::TransferRequest>();
    const auto folderRequest = requested.at(1).constFirst().value<rfm::core::TransferRequest>();
    QCOMPARE(fileRequest.destination, QStringLiteral("/srv/uploads/archive.bin"));
    QVERIFY(!fileRequest.directory);
    QCOMPARE(folderRequest.destination, QStringLiteral("/srv/uploads/photos"));
    QVERIFY(folderRequest.directory);
}

void MainWindowTest::queuesDownloadsFromRemoteSelection()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("archive.tar"), 10, {}, false, false},
        {QStringLiteral("photos"), 0, {}, true, false},
    };
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("remoteFileTable"));
    QVERIFY(table != nullptr);
    table->selectionModel()->select(table->model()->index(0, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(1, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QSignalSpy requested(&window, &rfm::app::MainWindow::transferRequested);
    QTemporaryDir destination;
    QVERIFY(destination.isValid());

    QVERIFY(QMetaObject::invokeMethod(&window, "queueDownloads", Qt::DirectConnection,
                                      Q_ARG(QString, destination.path())));
    QCOMPARE(requested.size(), 2);
    auto* const newConnectionAction =
        window.findChild<QAction*>(QStringLiteral("newConnectionAction"));
    QVERIFY(newConnectionAction != nullptr);
    QVERIFY(!newConnectionAction->isEnabled());
    QHash<QString, rfm::core::TransferRequest> bySource;
    for (const QList<QVariant>& arguments : requested) {
        const auto request = arguments.constFirst().value<rfm::core::TransferRequest>();
        bySource.insert(request.source, request);
    }
    QCOMPARE(bySource.value(QStringLiteral("/srv/archive.tar")).destination,
             QDir(destination.path()).filePath(QStringLiteral("archive.tar")));
    QVERIFY(!bySource.value(QStringLiteral("/srv/archive.tar")).directory);
    QCOMPARE(bySource.value(QStringLiteral("/srv/photos")).destination,
             QDir(destination.path()).filePath(QStringLiteral("photos")));
    QVERIFY(bySource.value(QStringLiteral("/srv/photos")).directory);
}

void MainWindowTest::displaysTransferProgressAndMultipleEntries()
{
    rfm::app::OperationPanel panel;
    panel.show();
    auto first = progress(201, rfm::core::TransferState::Queued);
    panel.updateOperation(rfm::core::operationProgress(first));
    auto second = progress(202, rfm::core::TransferState::Queued);
    second.source = QStringLiteral("/tmp/photos");
    second.destination = QStringLiteral("/srv/photos");
    second.direction = rfm::core::TransferDirection::Download;
    second.directory = true;
    panel.updateOperation(rfm::core::operationProgress(second));

    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 2);
    const int firstRow = rowForId(table, 201);
    QVERIFY(firstRow >= 0);
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("Operation"));
    QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("Status"));
    QCOMPARE(table->horizontalHeaderItem(4)->text(), QStringLiteral("Progress"));
    QCOMPARE(table->horizontalHeaderItem(5)->text(), QStringLiteral("Speed"));
    QCOMPARE(table->horizontalHeaderItem(7)->text(), QStringLiteral("Error"));
    QCOMPARE(table->item(firstRow, 0)->text(), QStringLiteral("↑ Upload"));
    QCOMPARE(table->item(firstRow, 3)->text(), QStringLiteral("Queued"));
    QCOMPARE(table->item(rowForId(table, 202), 0)->text(), QStringLiteral("↓ Download"));

    panel.updateOperation(rfm::core::operationProgress(
        progress(201, rfm::core::TransferState::Transferring, 512, 1024, 1024)));
    QCOMPARE(table->item(firstRow, 3)->text(), QStringLiteral("Running"));
    QCOMPARE(table->item(firstRow, 5)->text(), QStringLiteral("1.0 KiB/s"));
    auto* const bar = qobject_cast<QProgressBar*>(table->cellWidget(firstRow, 4));
    QVERIFY(bar != nullptr);
    QCOMPARE(bar->value(), 500);
    QCOMPARE(bar->format(), QStringLiteral("512 B / 1.0 KiB"));
}

void MainWindowTest::displaysTransferStatesInEnglish()
{
    rfm::app::OperationPanel panel;
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    const QList<QPair<rfm::core::TransferState, QString>> states{
        {rfm::core::TransferState::Queued, QStringLiteral("Queued")},
        {rfm::core::TransferState::Preparing, QStringLiteral("Preparing")},
        {rfm::core::TransferState::Transferring, QStringLiteral("Running")},
        {rfm::core::TransferState::Paused, QStringLiteral("Paused")},
        {rfm::core::TransferState::Finalizing, QStringLiteral("Finalizing")},
        {rfm::core::TransferState::Cancelling, QStringLiteral("Cancelling")},
        {rfm::core::TransferState::Completed, QStringLiteral("Completed")},
        {rfm::core::TransferState::Cancelled, QStringLiteral("Cancelled")},
        {rfm::core::TransferState::Failed, QStringLiteral("Failed")},
    };
    quint64 id = 600;
    for (const auto& [state, text] : states) {
        panel.updateOperation(rfm::core::operationProgress(progress(++id, state)));
        QCOMPARE(table->item(rowForId(table, id), 3)->text(), text);
    }
}

void MainWindowTest::displaysRemoteCopyAndMoveOperations()
{
    rfm::app::OperationPanel panel;
    panel.show();
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);

    const QList<rfm::core::RemoteSelection> copySources{
        {QStringLiteral("/source/first.txt"), false},
        {QStringLiteral("/source/second.txt"), false}};
    auto copy = rfm::core::beginRemoteOperation(
        701, rfm::core::OperationKind::RemoteCopy, copySources, QStringLiteral("/destination"));
    copy.serverHost = QStringLiteral("files.example.test");
    copy.serverPort = 2222;
    panel.updateOperation(copy);
    const int copyRow = rowForId(table, 701);
    QVERIFY(copyRow >= 0);
    QCOMPARE(table->item(copyRow, 0)->text(), QStringLiteral("Remote Copy"));
    QCOMPARE(table->item(copyRow, 0)->toolTip(),
             QStringLiteral("Server: files.example.test:2222"));
    QCOMPARE(table->item(copyRow, 1)->text(), QStringLiteral("first.txt (+1)"));
    QCOMPARE(table->item(copyRow, 2)->text(), QStringLiteral("/destination"));
    QCOMPARE(table->item(copyRow, 3)->text(), QStringLiteral("Running"));
    QCOMPARE(table->item(copyRow, 5)->text(), QStringLiteral("—"));
    auto* const indeterminate =
        qobject_cast<QProgressBar*>(table->cellWidget(copyRow, 4));
    QVERIFY(indeterminate != nullptr);
    QCOMPARE(indeterminate->minimum(), 0);
    QCOMPARE(indeterminate->maximum(), 0);
    auto* const copyActions = table->cellWidget(copyRow, 6);
    QVERIFY(copyActions != nullptr);
    auto* const cancelCopy =
        copyActions->findChild<QPushButton*>(QStringLiteral("cancelTransferButton"));
    QVERIFY(cancelCopy != nullptr);
    QVERIFY(cancelCopy->isVisible());

    const rfm::core::RemoteOperationResult completedCopy{
        701,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/first.txt"), QStringLiteral("/destination/first.txt"), true, {}},
         {QStringLiteral("/source/second.txt"), QStringLiteral("/destination/second.txt"), true,
          {}}}};
    panel.updateOperation(rfm::core::finishRemoteOperation(completedCopy, copy));
    QCOMPARE(table->item(copyRow, 3)->text(), QStringLiteral("Completed"));
    QCOMPARE(table->item(copyRow, 4)->text(), QStringLiteral("2 / 2 completed"));

    const QList<rfm::core::RemoteSelection> moveSources{
        {QStringLiteral("/source/a.txt"), false},
        {QStringLiteral("/source/b.txt"), false}};
    const auto move = rfm::core::beginRemoteOperation(
        702, rfm::core::OperationKind::RemoteMove, moveSources, QStringLiteral("/archive"));
    panel.updateOperation(move);
    const rfm::core::RemoteOperationResult partialMove{
        702,
        rfm::core::RemoteOperationKind::Move,
        {{QStringLiteral("/source/a.txt"), QStringLiteral("/archive/a.txt"), true, {}},
         {QStringLiteral("/source/b.txt"), QStringLiteral("/archive/b.txt"), false,
          QStringLiteral("permission denied")}}};
    panel.updateOperation(rfm::core::finishRemoteOperation(partialMove, move));
    const int moveRow = rowForId(table, 702);
    QVERIFY(moveRow >= 0);
    QCOMPARE(table->item(moveRow, 0)->text(), QStringLiteral("Remote Move"));
    QCOMPARE(table->item(moveRow, 3)->text(), QStringLiteral("Failed"));
    QCOMPARE(table->item(moveRow, 4)->text(), QStringLiteral("1 / 2 completed"));
    QVERIFY(table->item(moveRow, 7)->text().contains(QStringLiteral("permission denied")));
    QVERIFY(table->cellWidget(moveRow, 4) == nullptr);
    QVERIFY(table->cellWidget(moveRow, 6) == nullptr);
}

void MainWindowTest::removesOnlyTerminalOperationsFromPanel()
{
    rfm::app::OperationPanel panel;
    panel.show();
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    auto* const remove =
        panel.findChild<QPushButton*>(QStringLiteral("removeOperationButton"));
    auto* const clear =
        panel.findChild<QPushButton*>(QStringLiteral("clearOperationHistoryButton"));
    QVERIFY(table != nullptr);
    QVERIFY(remove != nullptr);
    QVERIFY(clear != nullptr);
    QSignalSpy removals(&panel, &rfm::app::OperationPanel::removeTerminalRequested);
    QSignalSpy clears(&panel, &rfm::app::OperationPanel::clearTerminalRequested);

    panel.updateOperation(rfm::core::beginRemoteOperation(
        801, rfm::core::OperationKind::RemoteCopy,
        {{QStringLiteral("/source/active"), false}}, QStringLiteral("/destination")));
    auto completed = progress(802, rfm::core::TransferState::Completed, 10, 10);
    panel.updateOperation(rfm::core::operationProgress(completed));
    auto failed = progress(803, rfm::core::TransferState::Failed, 4, 10);
    failed.error = QStringLiteral("failure");
    panel.updateOperation(rfm::core::operationProgress(failed));
    QCOMPARE(table->rowCount(), 3);

    table->selectRow(rowForId(table, 801));
    QVERIFY(!remove->isEnabled());
    QVERIFY(clear->isEnabled());
    QVERIFY(!panel.removeTerminalOperation(801));
    QCOMPARE(table->rowCount(), 3);

    table->selectRow(rowForId(table, 802));
    QVERIFY(remove->isEnabled());
    QTest::mouseClick(remove, Qt::LeftButton);
    QCOMPARE(removals.size(), 1);
    QCOMPARE(removals.constFirst().constFirst().toULongLong(), quint64{802});
    QVERIFY(panel.removeTerminalOperation(802));
    QCOMPARE(table->rowCount(), 2);

    QTest::mouseClick(clear, Qt::LeftButton);
    QCOMPARE(clears.size(), 1);
    panel.clearTerminalOperations();
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(rowForId(table, 801), 0);
    QVERIFY(!clear->isEnabled());
}

void MainWindowTest::exposesPauseResumeAndCancelIntentions()
{
    rfm::app::OperationPanel panel;
    panel.show();
    QSignalSpy pauses(&panel, &rfm::app::OperationPanel::pauseRequested);
    QSignalSpy resumes(&panel, &rfm::app::OperationPanel::resumeRequested);
    QSignalSpy cancellations(&panel, &rfm::app::OperationPanel::cancelRequested);
    panel.updateOperation(rfm::core::operationProgress(
        progress(301, rfm::core::TransferState::Transferring, 1, 10)));
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    const int row = rowForId(table, 301);
    QWidget* const actions = table->cellWidget(row, 6);
    auto* const pauseResume = actions->findChild<QPushButton*>(QStringLiteral("pauseResumeButton"));
    auto* const cancel = actions->findChild<QPushButton*>(QStringLiteral("cancelTransferButton"));
    QVERIFY(pauseResume != nullptr);
    QVERIFY(cancel != nullptr);
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(1), QHeaderView::Stretch);
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(2), QHeaderView::Stretch);
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(6), QHeaderView::ResizeToContents);
    QVERIFY(pauseResume->minimumWidth() >= pauseResume->sizeHint().width());
    QVERIFY(cancel->minimumWidth() >= cancel->sizeHint().width());
    QVERIFY(actions->minimumWidth() >= pauseResume->minimumWidth() + cancel->minimumWidth());
    for (const int width : {1000, 760, 520}) {
        panel.resize(width, 240);
        QCoreApplication::processEvents();
        QVERIFY(table->columnWidth(6) >= actions->minimumWidth());
        QVERIFY(pauseResume->width() >= pauseResume->minimumWidth());
        QVERIFY(cancel->width() >= cancel->minimumWidth());
    }
    QCOMPARE(pauseResume->text(), QStringLiteral("Pause"));
    QTest::mouseClick(pauseResume, Qt::LeftButton);
    QCOMPARE(pauses.size(), 1);

    panel.updateOperation(
        rfm::core::operationProgress(progress(301, rfm::core::TransferState::Paused, 1, 10)));
    QCOMPARE(pauseResume->text(), QStringLiteral("Resume"));
    QTest::mouseClick(pauseResume, Qt::LeftButton);
    QCOMPARE(resumes.size(), 1);
    panel.updateOperation(rfm::core::operationProgress(
        progress(301, rfm::core::TransferState::Transferring, 2, 10)));
    QCOMPARE(pauseResume->text(), QStringLiteral("Pause"));
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(cancellations.size(), 1);
    QCOMPARE(cancellations.constFirst().constFirst().toULongLong(), quint64{301});
    QCOMPARE(cancel->text(), QStringLiteral("Cancel"));
}

void MainWindowTest::displaysTerminalTransferStatesAndErrors()
{
    rfm::app::OperationPanel panel;
    panel.show();
    panel.updateOperation(rfm::core::operationProgress(
        progress(401, rfm::core::TransferState::Completed, 10, 10)));
    auto failure = progress(402, rfm::core::TransferState::Failed, 4, 10);
    failure.error = QStringLiteral("/srv/archive: permission denied");
    panel.updateOperation(rfm::core::operationProgress(failure));
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    const int completedRow = rowForId(table, 401);
    const int failedRow = rowForId(table, 402);
    QCOMPARE(table->item(completedRow, 3)->text(), QStringLiteral("Completed"));
    QCOMPARE(table->item(failedRow, 3)->text(), QStringLiteral("Failed"));
    QCOMPARE(table->item(failedRow, 7)->text(), failure.error);
    QWidget* const completedActions = table->cellWidget(completedRow, 6);
    QVERIFY(
        completedActions->findChild<QPushButton*>(QStringLiteral("pauseResumeButton"))->isHidden());
    QVERIFY(completedActions->findChild<QPushButton*>(QStringLiteral("cancelTransferButton"))
                ->isHidden());
}

void MainWindowTest::formatsTransferSizesAndSpeeds()
{
    QCOMPARE(rfm::app::OperationPanel::formatBytes(0), QStringLiteral("0 B"));
    QCOMPARE(rfm::app::OperationPanel::formatBytes(1536), QStringLiteral("1.5 KiB"));
    QCOMPARE(rfm::app::OperationPanel::formatSpeed(0), QStringLiteral("—"));
    QCOMPARE(rfm::app::OperationPanel::formatSpeed(1024), QStringLiteral("1.0 KiB/s"));
}

void MainWindowTest::refreshTimerIsConnectionAwareAndCoalescesListings()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("selected.txt"), 10, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false},
    };
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("remoteFileTable"));
    auto* const timer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(table != nullptr);
    QVERIFY(timer != nullptr);
    QVERIFY(timer->isActive());
    table->selectRow(0);

    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().at(1).toString(), QStringLiteral("/srv"));
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);

    const quint64 requestId = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, requestId),
        Q_ARG(QString, QStringLiteral("/srv")),
        Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QCOMPARE(table->selectionModel()->selectedRows(0).size(), 1);
    QCOMPARE(table->item(table->selectionModel()->selectedRows(0).constFirst().row(), 0)->text(),
             QStringLiteral("selected.txt"));
}

void MainWindowTest::refreshesAfterCompletedUpload()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("before.txt"), 10, {}, false, false},
    };
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const debounce = window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"));
    QVERIFY(debounce != nullptr);

    auto upload = progress(501, rfm::core::TransferState::Completed, 10, 10);
    upload.destination = QStringLiteral("/srv/uploaded.txt");
    upload.direction = rfm::core::TransferDirection::Upload;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, upload)));
    QVERIFY(debounce->isActive());
    debounce->stop();
    QVERIFY(QMetaObject::invokeMethod(debounce, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().at(1).toString(), QStringLiteral("/srv"));

    const quint64 requestId = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, requestId),
        Q_ARG(QString, QStringLiteral("/srv")),
        Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    requested.clear();
    auto download = progress(502, rfm::core::TransferState::Completed, 10, 10);
    download.direction = rfm::core::TransferDirection::Download;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, download)));
    QVERIFY(!debounce->isActive());
    QCOMPARE(requested.size(), 0);
}

void MainWindowTest::appliesOnlyExpectedDirectoryResult()
{
    rfm::app::MainWindow window;
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const timer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    const quint64 expectedId = requested.constFirst().constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> wrongEntries{
        {QStringLiteral("wrong.txt"), 1, {}, false, false}};
    const QList<rfm::core::RemoteEntry> expectedEntries{
        {QStringLiteral("expected.txt"), 1, {}, false, false}};

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, expectedId + 100), Q_ARG(QString, QStringLiteral("/wrong")),
        Q_ARG(QList<rfm::core::RemoteEntry>, wrongEntries)));
    auto* const pane = window.findChild<rfm::app::FileBrowserPane*>();
    QCOMPARE(pane->currentPath(), QStringLiteral("/srv"));

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, expectedId),
        Q_ARG(QString, QStringLiteral("/srv")),
        Q_ARG(QList<rfm::core::RemoteEntry>, expectedEntries)));
    QCOMPARE(pane->fileTable()->rowCount(), 1);
    QCOMPARE(pane->fileTable()->item(0, 0)->text(), QStringLiteral("expected.txt"));
}

void MainWindowTest::distinguishesRequestsForSamePath()
{
    rfm::app::MainWindow window;
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const timer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    auto* const pane = window.findChild<rfm::app::FileBrowserPane*>();
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    const quint64 firstId = requested.constFirst().constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> oldEntries{
        {QStringLiteral("old.txt"), 1, {}, false, false}};
    const QList<rfm::core::RemoteEntry> newEntries{
        {QStringLiteral("new.txt"), 1, {}, false, false}};
    pane->requestRefresh();

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, firstId),
        Q_ARG(QString, QStringLiteral("/srv")),
        Q_ARG(QList<rfm::core::RemoteEntry>, oldEntries)));
    QCOMPARE(requested.size(), 2);
    const quint64 secondId = requested.at(1).constFirst().toULongLong();
    QVERIFY(firstId != secondId);
    QCOMPARE(requested.at(0).at(1).toString(), requested.at(1).at(1).toString());
    QCOMPARE(pane->fileTable()->rowCount(), 0);

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, secondId),
        Q_ARG(QString, QStringLiteral("/srv")),
        Q_ARG(QList<rfm::core::RemoteEntry>, newEntries)));
    QCOMPARE(pane->fileTable()->item(0, 0)->text(), QStringLiteral("new.txt"));
}

void MainWindowTest::newNavigationMakesActiveResultObsolete()
{
    rfm::app::MainWindow window;
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const timer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    auto* const pane = window.findChild<rfm::app::FileBrowserPane*>();
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    const quint64 oldId = requested.constFirst().constFirst().toULongLong();
    pane->navigateTo(QStringLiteral("/other"));

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, oldId),
        Q_ARG(QString, QStringLiteral("/srv")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QCOMPARE(pane->currentPath(), QStringLiteral("/srv"));
    QCOMPARE(requested.size(), 2);
    QCOMPARE(requested.at(1).at(1).toString(), QStringLiteral("/other"));
}

void MainWindowTest::handlesOnlyExpectedListingErrorWithoutDisconnecting()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("kept.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy disconnected(&window, &rfm::app::MainWindow::disconnectionRequested);
    auto* const timer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    auto* const pane = window.findChild<rfm::app::FileBrowserPane*>();
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    const quint64 requestId = requested.constFirst().constFirst().toULongLong();

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListingError", Qt::DirectConnection,
        Q_ARG(quint64, requestId + 1), Q_ARG(QString, QStringLiteral("/wrong")),
        Q_ARG(QString, QStringLiteral("ignored"))));
    QVERIFY(!window.statusBar()->currentMessage().contains(QStringLiteral("ignored")));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListingError", Qt::DirectConnection,
        Q_ARG(quint64, requestId), Q_ARG(QString, QStringLiteral("/root")),
        Q_ARG(QString, QStringLiteral("Permission denied"))));

    QCOMPARE(disconnected.size(), 0);
    QCOMPARE(pane->currentPath(), QStringLiteral("/srv"));
    QCOMPARE(pane->fileTable()->rowCount(), 1);
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("/root")));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Permission denied")));
    QVERIFY(timer->isActive());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("newConnectionAction"))->isEnabled());
}

void MainWindowTest::symbolicLinkNavigationFailureDoesNotDisconnect()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("broken-link"), 0, {}, false, true}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "showRemoteDirectory", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("/srv")), Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::disconnectionRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy disconnections(&window, &rfm::app::MainWindow::disconnectionRequested);
    auto* const pane = window.findChild<rfm::app::FileBrowserPane*>();
    QVERIFY(pane != nullptr);

    QVERIFY(QMetaObject::invokeMethod(pane->fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 0), Q_ARG(int, 0)));
    QCOMPARE(listings.size(), 1);
    const quint64 requestId = listings.constFirst().constFirst().toULongLong();
    QCOMPARE(listings.constFirst().at(1).toString(), QStringLiteral("/srv/broken-link"));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListingError", Qt::DirectConnection, Q_ARG(quint64, requestId),
        Q_ARG(QString, QStringLiteral("/srv/broken-link")),
        Q_ARG(QString, QStringLiteral("Not a directory"))));

    QCOMPARE(pane->currentPath(), QStringLiteral("/srv"));
    QCOMPARE(disconnections.size(), 0);
    QVERIFY(pane->fileTable()->isEnabled());
}

void MainWindowTest::splitRoutesSerializedListingsPerPane()
{
    rfm::app::MainWindow window;
    window.show();
    const QList<rfm::core::RemoteEntry> initialEntries{
        {QStringLiteral("initial.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, initialEntries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const splitAction = window.findChild<QAction*>(QStringLiteral("splitViewAction"));
    QVERIFY(workspace != nullptr);
    QVERIFY(splitAction != nullptr);
    QCOMPARE(workspace->visiblePaneIds().size(), 1);

    splitAction->trigger();
    QVERIFY(workspace->isSplit());
    QCOMPARE(workspace->visiblePaneIds().size(), 2);
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().at(1).toString(), QStringLiteral("/srv"));
    auto* const primary = workspace->primaryPane();
    auto* const secondary = workspace->otherVisiblePane();
    const quint64 initialSecondaryRequest = requested.constFirst().constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> secondaryInitial{
        {QStringLiteral("secondary.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, initialSecondaryRequest), Q_ARG(QString, QStringLiteral("/srv")),
        Q_ARG(QList<rfm::core::RemoteEntry>, secondaryInitial)));
    QCOMPARE(primary->fileTable()->item(0, 0)->text(), QStringLiteral("initial.txt"));
    QCOMPARE(secondary->fileTable()->item(0, 0)->text(), QStringLiteral("secondary.txt"));

    requested.clear();
    QTest::mouseClick(secondary->fileTable()->viewport(), Qt::LeftButton);
    secondary->navigateTo(QStringLiteral("/same"));
    const quint64 obsoleteSecondaryId = requested.constFirst().constFirst().toULongLong();
    QTest::mouseClick(primary->fileTable()->viewport(), Qt::LeftButton);
    primary->navigateTo(QStringLiteral("/same"));
    secondary->navigateTo(QStringLiteral("/latest"));
    QCOMPARE(requested.size(), 1);

    const QList<rfm::core::RemoteEntry> obsoleteEntries{
        {QStringLiteral("obsolete.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, obsoleteSecondaryId), Q_ARG(QString, QStringLiteral("/same")),
        Q_ARG(QList<rfm::core::RemoteEntry>, obsoleteEntries)));
    QCOMPARE(requested.size(), 2);
    QCOMPARE(requested.at(1).at(1).toString(), QStringLiteral("/same"));
    QCOMPARE(secondary->currentPath(), QStringLiteral("/srv"));

    const quint64 primaryId = requested.at(1).constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> primaryEntries{
        {QStringLiteral("primary-new.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, primaryId),
        Q_ARG(QString, QStringLiteral("/same")),
        Q_ARG(QList<rfm::core::RemoteEntry>, primaryEntries)));
    QCOMPARE(requested.size(), 3);
    QCOMPARE(requested.at(2).at(1).toString(), QStringLiteral("/latest"));
    QCOMPARE(primary->currentPath(), QStringLiteral("/same"));
    QCOMPARE(secondary->currentPath(), QStringLiteral("/srv"));

    const quint64 secondaryId = requested.at(2).constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> latestEntries{
        {QStringLiteral("secondary-new.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, secondaryId),
        Q_ARG(QString, QStringLiteral("/latest")),
        Q_ARG(QList<rfm::core::RemoteEntry>, latestEntries)));
    QCOMPARE(secondary->currentPath(), QStringLiteral("/latest"));
    QCOMPARE(primary->fileTable()->item(0, 0)->text(), QStringLiteral("primary-new.txt"));

    splitAction->trigger();
    QVERIFY(!workspace->isSplit());
    QCOMPARE(workspace->visiblePaneIds().size(), 1);
    QCOMPARE(primary->currentPath(), QStringLiteral("/same"));

    const qsizetype listingCountBeforeReopen = requested.size();
    splitAction->trigger();
    QVERIFY(workspace->isSplit());
    QCOMPARE(workspace->visiblePaneIds().size(), 2);
    QCOMPARE(primary->currentPath(), QStringLiteral("/same"));
    QCOMPARE(secondary->currentPath(), QStringLiteral("/latest"));
    QCOMPARE(requested.size(), listingCountBeforeReopen);
}

void MainWindowTest::activePaneOwnsNavigationRefreshAndUploadTargets()
{
    rfm::app::MainWindow window;
    window.show();
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/one")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    auto* const secondary = workspace->otherVisiblePane();
    const quint64 initialId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, initialId),
        Q_ARG(QString, QStringLiteral("/one")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    secondary->showDirectory(QStringLiteral("/two/child"), QStringLiteral("sftp://host/two/child"),
                             {{QStringLiteral("selected.txt"), 1, {}, false, false}});
    QTest::mouseClick(secondary->fileTable()->viewport(), Qt::LeftButton);
    secondary->fileTable()->selectRow(0);
    QCOMPARE(workspace->activePane(), secondary);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("renameAction"))->isEnabled());
    QTest::mouseClick(workspace->primaryPane()->fileTable()->viewport(), Qt::LeftButton);
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("renameAction"))->isEnabled());
    QTest::mouseClick(secondary->fileTable()->viewport(), Qt::LeftButton);

    listings.clear();
    window.findChild<QAction*>(QStringLiteral("refreshAction"))->trigger();
    QCOMPARE(listings.size(), 1);
    QCOMPARE(listings.constFirst().at(1).toString(), QStringLiteral("/two/child"));
    const quint64 refreshId = listings.constFirst().constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> refreshedEntries{
        {QStringLiteral("selected.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, refreshId),
        Q_ARG(QString, QStringLiteral("/two/child")),
        Q_ARG(QList<rfm::core::RemoteEntry>, refreshedEntries)));

    listings.clear();
    window.findChild<QAction*>(QStringLiteral("upAction"))->trigger();
    QCOMPARE(listings.size(), 1);
    QCOMPARE(listings.constFirst().at(1).toString(), QStringLiteral("/two"));
    const quint64 parentId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, parentId),
        Q_ARG(QString, QStringLiteral("/two")),
        Q_ARG(QList<rfm::core::RemoteEntry>, refreshedEntries)));

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString uploadPath = temporary.filePath(QStringLiteral("upload.txt"));
    QFile upload(uploadPath);
    QVERIFY(upload.open(QIODevice::WriteOnly));
    upload.write("data");
    upload.close();
    QSignalSpy transfers(&window, &rfm::app::MainWindow::transferRequested);
    QVERIFY(QMetaObject::invokeMethod(&window, "queueUploads", Qt::DirectConnection,
                                      Q_ARG(QStringList, QStringList{uploadPath})));
    QCOMPARE(transfers.size(), 1);
    const auto transfer = transfers.constFirst().constFirst().value<rfm::core::TransferRequest>();
    QCOMPARE(transfer.destination, QStringLiteral("/two/upload.txt"));

    secondary->fileTable()->selectRow(0);
    QVERIFY(QMetaObject::invokeMethod(&window, "queueDownloads", Qt::DirectConnection,
                                      Q_ARG(QString, temporary.path())));
    QCOMPARE(transfers.size(), 2);
    const auto download = transfers.at(1).constFirst().value<rfm::core::TransferRequest>();
    QCOMPARE(download.source, QStringLiteral("/two/selected.txt"));
}

void MainWindowTest::navigationKeepsInitiatingPaneActive_data()
{
    QTest::addColumn<bool>("useSecondaryPane");
    QTest::addColumn<QString>("navigation");

    const QStringList navigations{QStringLiteral("open"), QStringLiteral("parent"),
                                  QStringLiteral("back"), QStringLiteral("forward"),
                                  QStringLiteral("refresh")};
    for (const bool useSecondaryPane : {false, true}) {
        const QString paneName = useSecondaryPane ? QStringLiteral("right")
                                                  : QStringLiteral("left");
        for (const QString& navigation : navigations) {
            QTest::newRow(qPrintable(paneName + QLatin1Char('-') + navigation))
                << useSecondaryPane << navigation;
        }
    }
}

void MainWindowTest::navigationKeepsInitiatingPaneActive()
{
    QFETCH(bool, useSecondaryPane);
    QFETCH(QString, navigation);

    rfm::app::MainWindow window;
    window.show();
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/root")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    rfm::app::FileBrowserPane* const secondary = workspace->otherVisiblePane();
    const quint64 initialRequestId = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, initialRequestId), Q_ARG(QString, QStringLiteral("/root")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));

    rfm::app::FileBrowserPane* const pane =
        useSecondaryPane ? secondary : workspace->primaryPane();
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("child"), 0, {}, true, false}};
    pane->showDirectory(QStringLiteral("/root"), QStringLiteral("/root"), {},
                        rfm::app::PaneNavigation::Initial);
    pane->showDirectory(QStringLiteral("/root/current"), QStringLiteral("/root/current"),
                        entries, rfm::app::PaneNavigation::Normal);
    if (navigation == QStringLiteral("forward")) {
        pane->showDirectory(QStringLiteral("/root"), QStringLiteral("/root"), entries,
                            rfm::app::PaneNavigation::Back);
    }

    QTest::mouseClick(pane->fileTable()->viewport(), Qt::LeftButton);
    QCOMPARE(workspace->activePane(), pane);
    requested.clear();

    if (navigation == QStringLiteral("open")) {
        QVERIFY(QMetaObject::invokeMethod(pane->fileTable(), "cellDoubleClicked",
                                          Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 0)));
    } else if (navigation == QStringLiteral("parent")) {
        window.findChild<QAction*>(QStringLiteral("upAction"))->trigger();
    } else if (navigation == QStringLiteral("back")) {
        window.findChild<QAction*>(QStringLiteral("backAction"))->trigger();
    } else if (navigation == QStringLiteral("forward")) {
        window.findChild<QAction*>(QStringLiteral("forwardAction"))->trigger();
    } else {
        window.findChild<QAction*>(QStringLiteral("refreshAction"))->trigger();
    }

    QCoreApplication::processEvents();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(workspace->activePane(), pane);
    const quint64 requestId = requested.constFirst().constFirst().toULongLong();
    const QString resultPath = requested.constFirst().at(1).toString();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, requestId),
        Q_ARG(QString, resultPath), Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QCoreApplication::processEvents();

    QCOMPARE(workspace->activePane(), pane);
}

void MainWindowTest::splitListingErrorLeavesOtherPaneUntouched()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> primaryEntries{
        {QStringLiteral("kept.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, primaryEntries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    auto* const secondary = workspace->otherVisiblePane();
    const quint64 requestId = requested.constFirst().constFirst().toULongLong();

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListingError", Qt::DirectConnection,
        Q_ARG(quint64, requestId), Q_ARG(QString, QStringLiteral("/srv")),
        Q_ARG(QString, QStringLiteral("Permission denied"))));

    QCOMPARE(workspace->primaryPane()->currentPath(), QStringLiteral("/srv"));
    QCOMPARE(workspace->primaryPane()->fileTable()->item(0, 0)->text(),
             QStringLiteral("kept.txt"));
    QVERIFY(secondary->currentPath().isEmpty());
    QVERIFY(secondary->fileTable()->isEnabled());
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Permission denied")));
}

void MainWindowTest::historyActionsFollowActivePaneAndIgnoreFailedOrObsoleteListings()
{
    rfm::app::MainWindow window;
    window.show();
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/a")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const back = window.findChild<QAction*>(QStringLiteral("backAction"));
    auto* const forward = window.findChild<QAction*>(QStringLiteral("forwardAction"));
    auto* const primary = workspace->primaryPane();
    QVERIFY(!back->isEnabled());
    QVERIFY(!forward->isEnabled());

    primary->navigateTo(QStringLiteral("/b"));
    const quint64 toB = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, toB),
        Q_ARG(QString, QStringLiteral("/b")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QVERIFY(back->isEnabled());
    QVERIFY(!forward->isEnabled());

    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    auto* const secondary = workspace->otherVisiblePane();
    const quint64 secondaryInitial = requested.constLast().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, secondaryInitial), Q_ARG(QString, QStringLiteral("/b")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QTest::mouseClick(secondary->fileTable()->viewport(), Qt::LeftButton);
    QVERIFY(!back->isEnabled());
    QTest::mouseClick(primary->fileTable()->viewport(), Qt::LeftButton);
    QVERIFY(back->isEnabled());

    primary->navigateTo(QStringLiteral("/missing"));
    const quint64 failed = requested.constLast().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListingError", Qt::DirectConnection, Q_ARG(quint64, failed),
        Q_ARG(QString, QStringLiteral("/missing")), Q_ARG(QString, QStringLiteral("missing"))));
    QVERIFY(back->isEnabled());

    primary->navigateTo(QStringLiteral("/obsolete"));
    const quint64 obsolete = requested.constLast().constFirst().toULongLong();
    primary->navigateTo(QStringLiteral("/current"));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, obsolete),
        Q_ARG(QString, QStringLiteral("/obsolete")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    const quint64 current = requested.constLast().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, current),
        Q_ARG(QString, QStringLiteral("/current")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    back->trigger();
    QCOMPARE(requested.constLast().at(1).toString(), QStringLiteral("/b"));
}

void MainWindowTest::copiesAndMovesSelectionToOtherPane()
{
    rfm::app::MainWindow window;
    window.show();
    const QList<rfm::core::RemoteEntry> sourceEntries{
        {QStringLiteral("file.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, sourceEntries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::copyRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::moveRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy copies(&window, &rfm::app::MainWindow::copyRequested);
    QSignalSpy moves(&window, &rfm::app::MainWindow::moveRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const copyOther = window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"));
    auto* const moveOther = window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction"));
    workspace->primaryPane()->fileTable()->selectRow(0);
    QVERIFY(!copyOther->isEnabled());
    QVERIFY(!moveOther->isEnabled());

    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    auto* const destinationPane = workspace->otherVisiblePane();
    const quint64 initialId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, initialId),
        Q_ARG(QString, QStringLiteral("/destination")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QTest::mouseClick(workspace->primaryPane()->fileTable()->viewport(), Qt::LeftButton);
    workspace->primaryPane()->fileTable()->clearSelection();
    QVERIFY(!copyOther->isEnabled());
    QVERIFY(!moveOther->isEnabled());
    workspace->primaryPane()->fileTable()->selectRow(0);
    QVERIFY(copyOther->isEnabled());
    QVERIFY(moveOther->isEnabled());

    QToolBar actionToolbar;
    actionToolbar.addAction(copyOther);
    actionToolbar.addAction(moveOther);
    actionToolbar.show();
    QTest::mouseClick(destinationPane->fileTable()->viewport(), Qt::LeftButton);
    QTest::mouseClick(workspace->primaryPane()->fileTable()->viewport(), Qt::LeftButton);
    workspace->primaryPane()->fileTable()->selectRow(0);
    QCOMPARE(workspace->activePane(), workspace->primaryPane());
    QVERIFY(copyOther->isEnabled());
    actionToolbar.setFocus();
    acceptNextQuestion();
    copyOther->trigger();
    QCOMPARE(copies.size(), 1);
    const auto copiedSources = copies.constFirst().at(1).value<QList<rfm::core::RemoteSelection>>();
    QCOMPARE(copiedSources.constFirst().path, QStringLiteral("/source/file.txt"));
    QCOMPARE(copies.constFirst().at(2).toString(), destinationPane->currentPath());
    const quint64 copyId = copies.constFirst().constFirst().toULongLong();
    auto* const operationTable =
        window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(operationTable != nullptr);
    const int copyRow = rowForId(operationTable, copyId);
    QVERIFY(copyRow >= 0);
    QCOMPARE(operationTable->item(copyRow, 0)->text(), QStringLiteral("Remote Copy"));
    QCOMPARE(operationTable->item(copyRow, 3)->text(), QStringLiteral("Running"));
    QVERIFY(operationTable->cellWidget(copyRow, 4) != nullptr);
    auto* const copyActions = operationTable->cellWidget(copyRow, 6);
    QVERIFY(copyActions != nullptr);
    auto* const cancelCopy =
        copyActions->findChild<QPushButton*>(QStringLiteral("cancelTransferButton"));
    QVERIFY(cancelCopy != nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::cancelRemoteOperationRequested, nullptr,
                        nullptr);
    QSignalSpy copyCancellations(&window,
                                 &rfm::app::MainWindow::cancelRemoteOperationRequested);
    cancelCopy->click();
    QCOMPARE(copyCancellations.size(), 1);
    QCOMPARE(copyCancellations.constFirst().constFirst().toULongLong(), copyId);
    const rfm::core::RemoteOperationResult copyResult{
        copyId,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/file.txt"), QStringLiteral("/destination/file.txt"), true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, copyResult)));
    QCOMPARE(operationTable->item(copyRow, 3)->text(), QStringLiteral("Completed"));

    auto* const debounce = window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"));
    debounce->stop();
    destinationPane->showDirectory(
        QStringLiteral("/destination"), QStringLiteral("/destination"),
        {{QStringLiteral("back.txt"), 1, {}, false, false}});
    QTest::mouseClick(destinationPane->fileTable()->viewport(), Qt::LeftButton);
    destinationPane->fileTable()->selectRow(0);
    QCOMPARE(workspace->activePane(), destinationPane);
    actionToolbar.setFocus();
    acceptNextQuestion();
    moveOther->trigger();
    QCOMPARE(moves.size(), 1);
    const auto movedSources = moves.constFirst().at(1).value<QList<rfm::core::RemoteSelection>>();
    QCOMPARE(movedSources.constFirst().path, QStringLiteral("/destination/back.txt"));
    QCOMPARE(moves.constFirst().at(2).toString(), QStringLiteral("/source"));
    QVERIFY(workspace->paneId(destinationPane) !=
            workspace->paneId(workspace->otherVisiblePane(workspace->paneId(destinationPane))));
    const quint64 moveId = moves.constFirst().constFirst().toULongLong();
    const int moveRow = rowForId(operationTable, moveId);
    QVERIFY(moveRow >= 0);
    QCOMPARE(operationTable->item(moveRow, 0)->text(), QStringLiteral("Remote Move"));
    QCOMPARE(operationTable->item(moveRow, 3)->text(), QStringLiteral("Running"));
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    const rfm::core::RemoteOperationResult moveResult{
        moveId,
        rfm::core::RemoteOperationKind::Move,
        {{QStringLiteral("/destination/back.txt"), QStringLiteral("/source/back.txt"), true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, moveResult)));
    QCOMPARE(operationTable->item(moveRow, 3)->text(), QStringLiteral("Completed"));
    const int operationCount = operationTable->rowCount();
    const rfm::core::RemoteOperationResult createDirectoryResult{
        moveId + 100,
        rfm::core::RemoteOperationKind::CreateDirectory,
        {{QStringLiteral("/source"), QStringLiteral("/source/new"), true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, createDirectoryResult)));
    QCOMPARE(operationTable->rowCount(), operationCount);
    QVERIFY(!workspace->isSplit());
}

void MainWindowTest::rejectsOtherPaneOperationsForSameDirectory()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("file.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/same")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy copies(&window, &rfm::app::MainWindow::copyRequested);
    QSignalSpy moves(&window, &rfm::app::MainWindow::moveRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const copyOther = window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"));
    auto* const moveOther = window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction"));

    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    const quint64 listingId = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("/same/./")),
        Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QTest::mouseClick(workspace->primaryPane()->fileTable()->viewport(), Qt::LeftButton);
    workspace->primaryPane()->fileTable()->selectRow(0);

    QVERIFY(!copyOther->isEnabled());
    QVERIFY(!moveOther->isEnabled());
    copyOther->trigger();
    moveOther->trigger();
    QCOMPARE(copies.size(), 0);
    QCOMPARE(moves.size(), 0);

    workspace->primaryPane()->showDirectory(QStringLiteral("rfm-sprint4"),
                                             QStringLiteral("sftp://host/~/rfm-sprint4"),
                                             entries);
    workspace->otherVisiblePane()->showDirectory(
        QStringLiteral("/home/gabriel/rfm-sprint4"),
        QStringLiteral("sftp://host/home/gabriel/rfm-sprint4"), entries);
    QTest::mouseClick(workspace->primaryPane()->fileTable()->viewport(), Qt::LeftButton);
    workspace->primaryPane()->fileTable()->clearSelection();
    workspace->primaryPane()->fileTable()->selectRow(0);
    QVERIFY(!copyOther->isEnabled());
    QVERIFY(!moveOther->isEnabled());
}

void MainWindowTest::contextMenuUsesSharedInterPaneActions()
{
    rfm::app::MainWindow window;
    window.show();
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("deplacement.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("rfm-sprint4/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::copyRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::moveRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy copies(&window, &rfm::app::MainWindow::copyRequested);
    QSignalSpy moves(&window, &rfm::app::MainWindow::moveRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const sourcePane = workspace->primaryPane();
    sourcePane->fileTable()->selectRow(0);

    QTimer::singleShot(0, [&window] {
        auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu != nullptr);
        const QList<QAction*> actions = menu->actions();
        QCOMPARE(actions.size(), 12);
        QCOMPARE(actions.at(0),
                 window.findChild<QAction*>(QStringLiteral("clipboardCopyAction")));
        QCOMPARE(actions.at(1),
                 window.findChild<QAction*>(QStringLiteral("clipboardCutAction")));
        QCOMPARE(actions.at(2),
                 window.findChild<QAction*>(QStringLiteral("clipboardPasteAction")));
        QVERIFY(!actions.at(2)->isEnabled());
        QVERIFY(actions.at(3)->isSeparator());
        QCOMPARE(actions.at(4), window.findChild<QAction*>(QStringLiteral("copyAction")));
        QCOMPARE(actions.at(5), window.findChild<QAction*>(QStringLiteral("moveAction")));
        QVERIFY(actions.at(6)->isSeparator());
        QCOMPARE(actions.at(7), window.findChild<QAction*>(QStringLiteral("renameAction")));
        QCOMPARE(actions.at(8), window.findChild<QAction*>(QStringLiteral("downloadAction")));
        QCOMPARE(actions.at(9), window.findChild<QAction*>(QStringLiteral("removeAction")));
        QVERIFY(actions.at(10)->isSeparator());
        QCOMPARE(actions.at(11),
                 window.findChild<QAction*>(QStringLiteral("createDirectoryAction")));
        QVERIFY(!actions.contains(
            window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"))));
        QVERIFY(!actions.contains(
            window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction"))));
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(
        sourcePane, "contextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, sourcePane->fileTable()->viewport()->mapToGlobal(QPoint{4, 4}))));

    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    const quint64 listingId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("rfm-sprint4/dossier-test")),
        Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QTest::mouseClick(sourcePane->fileTable()->viewport(), Qt::LeftButton);
    sourcePane->fileTable()->selectRow(0);

    QTimer::singleShot(0, [&window] {
        auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu != nullptr);
        const QList<QAction*> actions = menu->actions();
        QCOMPARE(actions.size(), 14);
        QCOMPARE(actions.at(0),
                 window.findChild<QAction*>(QStringLiteral("clipboardCopyAction")));
        QCOMPARE(actions.at(1),
                 window.findChild<QAction*>(QStringLiteral("clipboardCutAction")));
        QCOMPARE(actions.at(2),
                 window.findChild<QAction*>(QStringLiteral("clipboardPasteAction")));
        QVERIFY(actions.at(3)->isSeparator());
        QCOMPARE(actions.at(4),
                 window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction")));
        QCOMPARE(actions.at(5),
                 window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction")));
        QCOMPARE(actions.at(6), window.findChild<QAction*>(QStringLiteral("copyAction")));
        QCOMPARE(actions.at(7), window.findChild<QAction*>(QStringLiteral("moveAction")));
        QVERIFY(actions.at(8)->isSeparator());
        QCOMPARE(actions.at(9), window.findChild<QAction*>(QStringLiteral("renameAction")));
        QCOMPARE(actions.at(10), window.findChild<QAction*>(QStringLiteral("downloadAction")));
        QCOMPARE(actions.at(11), window.findChild<QAction*>(QStringLiteral("removeAction")));
        QVERIFY(actions.at(12)->isSeparator());
        QCOMPARE(actions.at(13),
                 window.findChild<QAction*>(QStringLiteral("createDirectoryAction")));
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(
        sourcePane, "contextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, sourcePane->fileTable()->viewport()->mapToGlobal(QPoint{4, 4}))));

    const auto triggerContextAction = [&](const QString& objectName) {
        QAction* const sharedAction = window.findChild<QAction*>(objectName);
        QVERIFY(sharedAction != nullptr);
        QTimer::singleShot(0, [&window, sharedAction] {
            auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            QVERIFY(menu != nullptr);
            QAction* visibleAction = nullptr;
            for (QAction* const action : menu->actions()) {
                if (action->objectName() == sharedAction->objectName()) {
                    visibleAction = action;
                    break;
                }
            }
            QCOMPARE(visibleAction, sharedAction);
            acceptNextQuestion();
            visibleAction->trigger();
            menu->close();
        });
        QVERIFY(QMetaObject::invokeMethod(
            sourcePane, "contextMenuRequested", Qt::DirectConnection,
            Q_ARG(QPoint, sourcePane->fileTable()->viewport()->mapToGlobal(QPoint{4, 4}))));
    };

    triggerContextAction(QStringLiteral("copyToOtherPaneAction"));
    QCOMPARE(copies.size(), 1);
    QCOMPARE(copies.constFirst().at(1).value<QList<rfm::core::RemoteSelection>>().constFirst().path,
             QStringLiteral("rfm-sprint4/source/deplacement.txt"));
    QCOMPARE(copies.constFirst().at(2).toString(), QStringLiteral("rfm-sprint4/dossier-test"));
    const rfm::core::RemoteOperationResult copyResult{
        copies.constFirst().constFirst().toULongLong(), rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("rfm-sprint4/source/deplacement.txt"),
          QStringLiteral("rfm-sprint4/dossier-test/deplacement.txt"), true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, copyResult)));
    window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"))->stop();

    sourcePane->fileTable()->selectRow(0);
    triggerContextAction(QStringLiteral("moveToOtherPaneAction"));
    QCOMPARE(moves.size(), 1);
    QCOMPARE(moves.constFirst().at(1).value<QList<rfm::core::RemoteSelection>>().constFirst().path,
             QStringLiteral("rfm-sprint4/source/deplacement.txt"));
    QCOMPARE(moves.constFirst().at(2).toString(), QStringLiteral("rfm-sprint4/dossier-test"));
}

void MainWindowTest::contextMenuUsesClipboardAndKeepsExplicitDestinationActions()
{
    rfm::app::MainWindow window;
    window.show();
    const QList<rfm::core::RemoteEntry> sourceEntries{
        {QStringLiteral("first.txt"), 1, {}, false, false},
        {QStringLiteral("second.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, sourceEntries)));
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::copyRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::moveRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy copies(&window, &rfm::app::MainWindow::copyRequested);
    QSignalSpy moves(&window, &rfm::app::MainWindow::moveRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const sourcePane = workspace->primaryPane();

    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    const quint64 listingId = listings.constFirst().constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> destinationEntries{
        {QStringLiteral("folder"), 0, {}, true, false}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("/destination")),
        Q_ARG(QList<rfm::core::RemoteEntry>, destinationEntries)));
    auto* const destinationPane = workspace->otherVisiblePane();

    const auto triggerContextAction =
        [&](rfm::app::FileBrowserPane* pane, const QPoint& position,
            const QString& actionName, const QString& explicitDestination = QString{}) {
            QAction* const sharedAction = window.findChild<QAction*>(actionName);
            QVERIFY(sharedAction != nullptr);
            QTimer::singleShot(0, [&window, sharedAction, explicitDestination] {
                auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                QVERIFY(menu != nullptr);
                QVERIFY(menu->actions().contains(sharedAction));
                QVERIFY(sharedAction->isEnabled());
                if (!explicitDestination.isEmpty()) {
                    QTimer::singleShot(0, [explicitDestination] {
                        auto* const dialog =
                            qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
                        QVERIFY(dialog != nullptr);
                        dialog->setTextValue(explicitDestination);
                        dialog->accept();
                    });
                }
                sharedAction->trigger();
                menu->close();
            });
            QVERIFY(QMetaObject::invokeMethod(
                pane->fileTable(), "customContextMenuRequested", Qt::DirectConnection,
                Q_ARG(QPoint, position)));
        };

    QTest::mouseClick(sourcePane->fileTable()->viewport(), Qt::LeftButton);
    sourcePane->fileTable()->selectAll();
    const QPoint selectedRow =
        sourcePane->fileTable()->visualItemRect(sourcePane->fileTable()->item(0, 0)).center();
    triggerContextAction(sourcePane, selectedRow, QStringLiteral("clipboardCopyAction"));
    QCOMPARE(sourcePane->selectedEntries().size(), 2);
    QCOMPARE(copies.size(), 0);
    QCOMPARE(moves.size(), 0);
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("clipboardPasteAction"))->isEnabled());

    QTest::mouseClick(destinationPane->fileTable()->viewport(), Qt::LeftButton);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("clipboardPasteAction"))->isEnabled());
    const QPoint emptyArea(8, destinationPane->fileTable()->viewport()->height() - 3);
    triggerContextAction(destinationPane, emptyArea, QStringLiteral("clipboardPasteAction"));
    QCOMPARE(destinationPane->selectedEntries().size(), 0);
    QCOMPARE(copies.size(), 1);
    QCOMPARE(copies.constFirst().at(1).value<QList<rfm::core::RemoteSelection>>().size(), 2);
    QCOMPARE(copies.constFirst().at(2).toString(), QStringLiteral("/destination"));
    const quint64 firstPasteId = copies.constFirst().constFirst().toULongLong();
    const rfm::core::RemoteOperationResult firstPasteResult{
        firstPasteId, rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/first.txt"), QStringLiteral("/destination/first.txt"), true,
          {}},
         {QStringLiteral("/source/second.txt"), QStringLiteral("/destination/second.txt"), true,
          {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, firstPasteResult)));
    window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"))->stop();

    const QPoint folderRow = destinationPane->fileTable()
                                 ->visualItemRect(destinationPane->fileTable()->item(0, 0))
                                 .center();
    triggerContextAction(destinationPane, folderRow, QStringLiteral("clipboardPasteAction"));
    QCOMPARE(destinationPane->selectedEntries().size(), 1);
    QCOMPARE(copies.size(), 2);
    QCOMPARE(copies.at(1).at(2).toString(), QStringLiteral("/destination"));
    const quint64 secondPasteId = copies.at(1).constFirst().toULongLong();
    const rfm::core::RemoteOperationResult secondPasteResult{
        secondPasteId, rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/first.txt"), QStringLiteral("/destination/first.txt"), true,
          {}},
         {QStringLiteral("/source/second.txt"), QStringLiteral("/destination/second.txt"), true,
          {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, secondPasteResult)));
    window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"))->stop();

    QTest::mouseClick(sourcePane->fileTable()->viewport(), Qt::LeftButton);
    sourcePane->fileTable()->selectRow(0);
    triggerContextAction(sourcePane, selectedRow, QStringLiteral("clipboardCutAction"));
    QVERIFY(sourcePane->fileTable()->item(0, 0)->font().italic());
    QCOMPARE(moves.size(), 0);
    window.findChild<QAction*>(QStringLiteral("cancelCutAction"))->trigger();

    sourcePane->fileTable()->selectRow(0);
    triggerContextAction(sourcePane, selectedRow, QStringLiteral("copyAction"),
                         QStringLiteral("/manual-copy"));
    QCOMPARE(copies.size(), 3);
    QCOMPARE(copies.at(2).at(2).toString(), QStringLiteral("/manual-copy"));
    const quint64 manualCopyId = copies.at(2).constFirst().toULongLong();
    const rfm::core::RemoteOperationResult manualCopyResult{
        manualCopyId, rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/first.txt"), QStringLiteral("/manual-copy/first.txt"), true,
          {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, manualCopyResult)));
    window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"))->stop();

    sourcePane->fileTable()->selectRow(0);
    triggerContextAction(sourcePane, selectedRow, QStringLiteral("moveAction"),
                         QStringLiteral("/manual-move"));
    QCOMPARE(moves.size(), 1);
    QCOMPARE(moves.constFirst().at(2).toString(), QStringLiteral("/manual-move"));
}

void MainWindowTest::buildsCanonicalInterPanePathsThroughTheRealUiChain()
{
    rfm::app::MainWindow window;
    window.show();
    const QList<rfm::core::RemoteEntry> fileEntry{
        {QStringLiteral("fichier.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "showRemoteDirectory", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("rfm-sprint4/dossier-test/./")),
        Q_ARG(QList<rfm::core::RemoteEntry>, fileEntry)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::copyRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::moveRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy copies(&window, &rfm::app::MainWindow::copyRequested);
    QSignalSpy moves(&window, &rfm::app::MainWindow::moveRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const primary = workspace->primaryPane();
    QCOMPARE(primary->currentPath(), QStringLiteral("rfm-sprint4/dossier-test"));
    QVERIFY(!primary->pathEdit()->text().contains(QStringLiteral(":22//")));
    QVERIFY(primary->pathEdit()->text().contains(QStringLiteral("/~/rfm-sprint4/dossier-test")));

    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    auto* const secondary = workspace->otherVisiblePane();
    const quint64 initialListingId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, initialListingId), Q_ARG(QString, QStringLiteral("rfm-sprint4/./")),
        Q_ARG(QList<rfm::core::RemoteEntry>, fileEntry)));
    QCOMPARE(secondary->currentPath(), QStringLiteral("rfm-sprint4"));

    QTest::mouseClick(primary->fileTable()->viewport(), Qt::LeftButton);
    primary->fileTable()->selectRow(0);
    acceptNextQuestion();
    window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"))->trigger();
    QCOMPARE(copies.size(), 1);
    const auto relativeSources =
        copies.constFirst().at(1).value<QList<rfm::core::RemoteSelection>>();
    const QString relativeDestination = copies.constFirst().at(2).toString();
    QCOMPARE(relativeSources.constFirst().path,
             QStringLiteral("rfm-sprint4/dossier-test/fichier.txt"));
    QCOMPARE(relativeDestination, QStringLiteral("rfm-sprint4"));

    RecordingRemoteBackend relativeBackend;
    rfm::core::RemoteFileOperations relativeOperations(relativeBackend);
    const auto relativeResult = relativeOperations.copy(
        copies.constFirst().constFirst().toULongLong(), relativeSources, relativeDestination);
    QVERIFY(relativeResult.allSucceeded());
    QCOMPARE(relativeResult.items.constFirst().source,
             QStringLiteral("rfm-sprint4/dossier-test/fichier.txt"));
    QCOMPARE(relativeResult.items.constFirst().destination,
             QStringLiteral("rfm-sprint4/fichier.txt"));
    QCOMPARE(relativeBackend.lastDestination, QStringLiteral("rfm-sprint4/fichier.txt"));

    const rfm::core::RemoteOperationResult completedCopy{
        copies.constFirst().constFirst().toULongLong(), rfm::core::RemoteOperationKind::Copy,
        relativeResult.items};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, completedCopy)));
    window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"))->stop();

    primary->showDirectory(
        QStringLiteral("/home/gabriel/rfm-sprint4/dossier-test/"),
        QStringLiteral("sftp://gabriel@example.test/home/gabriel/rfm-sprint4/dossier-test"),
        fileEntry, rfm::app::PaneNavigation::Initial);
    secondary->showDirectory(
        QStringLiteral("/home/gabriel/rfm-sprint4/./"),
        QStringLiteral("sftp://gabriel@example.test/home/gabriel/rfm-sprint4"), fileEntry,
        rfm::app::PaneNavigation::Initial);
    QTest::mouseClick(primary->fileTable()->viewport(), Qt::LeftButton);
    primary->fileTable()->selectRow(0);
    acceptNextQuestion();
    window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction"))->trigger();
    QCOMPARE(moves.size(), 1);
    const auto absoluteSources =
        moves.constFirst().at(1).value<QList<rfm::core::RemoteSelection>>();
    const QString absoluteDestination = moves.constFirst().at(2).toString();
    QCOMPARE(absoluteSources.constFirst().path,
             QStringLiteral("/home/gabriel/rfm-sprint4/dossier-test/fichier.txt"));
    QCOMPARE(absoluteDestination, QStringLiteral("/home/gabriel/rfm-sprint4"));

    RecordingRemoteBackend absoluteBackend;
    rfm::core::RemoteFileOperations absoluteOperations(absoluteBackend);
    const auto absoluteResult = absoluteOperations.move(
        moves.constFirst().constFirst().toULongLong(), absoluteSources, absoluteDestination);
    QVERIFY(absoluteResult.allSucceeded());
    QCOMPARE(absoluteResult.items.constFirst().destination,
             QStringLiteral("/home/gabriel/rfm-sprint4/fichier.txt"));
    QCOMPARE(absoluteBackend.lastSource,
             QStringLiteral("/home/gabriel/rfm-sprint4/dossier-test/fichier.txt"));
    QCOMPARE(absoluteBackend.lastDestination,
             QStringLiteral("/home/gabriel/rfm-sprint4/fichier.txt"));
}

void MainWindowTest::refreshesAllVisiblePanesAffectedByOperationsAndUploads()
{
    rfm::app::MainWindow window;
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64,QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    auto* const secondary = workspace->otherVisiblePane();
    const quint64 initialId = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, initialId),
        Q_ARG(QString, QStringLiteral("/destination")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    requested.clear();
    auto* const debounce = window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"));

    const rfm::core::RemoteOperationResult copyResult{
        700,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/a"), QStringLiteral("/destination/a"), true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, copyResult)));
    debounce->stop();
    QVERIFY(QMetaObject::invokeMethod(debounce, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().at(1).toString(), QStringLiteral("/destination"));
    const quint64 copyRefresh = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, copyRefresh),
        Q_ARG(QString, QStringLiteral("/destination")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));

    requested.clear();
    const rfm::core::RemoteOperationResult moveResult{
        701,
        rfm::core::RemoteOperationKind::Move,
        {{QStringLiteral("/source/a"), QStringLiteral("/destination/a"), true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, moveResult)));
    debounce->stop();
    QVERIFY(QMetaObject::invokeMethod(debounce, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);
    const QString firstMovePath = requested.constFirst().at(1).toString();
    QVERIFY(firstMovePath == QStringLiteral("/source") ||
            firstMovePath == QStringLiteral("/destination"));
    const QString secondMovePath = firstMovePath == QStringLiteral("/source")
                                       ? QStringLiteral("/destination")
                                       : QStringLiteral("/source");
    const quint64 sourceRefresh = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, sourceRefresh),
        Q_ARG(QString, firstMovePath), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QCOMPARE(requested.size(), 2);
    QCOMPARE(requested.at(1).at(1).toString(), secondMovePath);
    const quint64 destinationRefresh = requested.at(1).constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, destinationRefresh), Q_ARG(QString, secondMovePath),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));

    workspace->primaryPane()->showDirectory(QStringLiteral("/destination"), QStringLiteral("/destination"), {});
    secondary->showDirectory(QStringLiteral("/destination"), QStringLiteral("/destination"), {});
    requested.clear();
    auto upload = progress(702, rfm::core::TransferState::Completed, 10, 10);
    upload.destination = QStringLiteral("/destination/uploaded.txt");
    upload.direction = rfm::core::TransferDirection::Upload;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, upload)));
    debounce->stop();
    QVERIFY(QMetaObject::invokeMethod(debounce, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);
    const quint64 firstUploadRefresh = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, firstUploadRefresh), Q_ARG(QString, QStringLiteral("/destination")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QCOMPARE(requested.size(), 2);

    const quint64 secondUploadRefresh = requested.at(1).constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, secondUploadRefresh), Q_ARG(QString, QStringLiteral("/destination")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    requested.clear();
    auto download = progress(703, rfm::core::TransferState::Completed, 10, 10);
    download.direction = rfm::core::TransferDirection::Download;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, download)));
    QVERIFY(!debounce->isActive());
    QCOMPARE(requested.size(), 0);
}

void MainWindowTest::persistsRemovesAndClearsTerminalOperationHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    {
        rfm::app::MainWindow window(nullptr, directory.path());
        setConnectionIdentity(window);
        auto completed = progress(901, rfm::core::TransferState::Completed, 10, 10);
        auto failed = progress(902, rfm::core::TransferState::Failed, 3, 10);
        failed.error = QStringLiteral("permission denied");
        QVERIFY(QMetaObject::invokeMethod(
            &window, "handleTransferProgress", Qt::DirectConnection,
            Q_ARG(rfm::core::TransferProgress, completed)));
        QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                          Q_ARG(rfm::core::TransferProgress, failed)));
        auto* const saveTimer =
            window.findChild<QTimer*>(QStringLiteral("operationHistorySaveTimer"));
        QVERIFY(saveTimer != nullptr);
        QVERIFY(saveTimer->isActive());
        saveTimer->stop();
        QVERIFY(QMetaObject::invokeMethod(saveTimer, "timeout", Qt::DirectConnection));
    }

    {
        rfm::app::MainWindow window(nullptr, directory.path());
        auto* const table = window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
        auto* const remove =
            window.findChild<QPushButton*>(QStringLiteral("removeOperationButton"));
        auto* const clear =
            window.findChild<QPushButton*>(QStringLiteral("clearOperationHistoryButton"));
        auto* const saveTimer =
            window.findChild<QTimer*>(QStringLiteral("operationHistorySaveTimer"));
        QVERIFY(table != nullptr);
        QVERIFY(remove != nullptr);
        QVERIFY(clear != nullptr);
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->item(rowForId(table, 901), 3)->text(), QStringLiteral("Completed"));
        QCOMPARE(table->item(rowForId(table, 902), 3)->text(), QStringLiteral("Failed"));
        QCOMPARE(table->item(rowForId(table, 901), 0)->toolTip(),
                 QStringLiteral("Server: history.example.test:2222"));

        table->selectRow(rowForId(table, 901));
        QVERIFY(remove->isEnabled());
        QTest::mouseClick(remove, Qt::LeftButton);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(rowForId(table, 901), -1);

        auto active = progress(903, rfm::core::TransferState::Transferring, 1, 10);
        QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                          Q_ARG(rfm::core::TransferProgress, active)));
        QCOMPARE(table->rowCount(), 2);
        table->selectRow(rowForId(table, 903));
        QVERIFY(!remove->isEnabled());
        QVERIFY(clear->isEnabled());
        QTest::mouseClick(clear, Qt::LeftButton);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(rowForId(table, 903), 0);
        QVERIFY(!clear->isEnabled());

        saveTimer->stop();
        QVERIFY(QMetaObject::invokeMethod(saveTimer, "timeout", Qt::DirectConnection));
    }

    rfm::app::MainWindow restored(nullptr, directory.path());
    auto* const table = restored.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 0);
}

void MainWindowTest::clipboardCopiesCutsPastesAndClearsSuccessfulMove()
{
    rfm::app::MainWindow window;
    window.show();
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("file.txt"), 1, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    QVERIFY(!listings.isEmpty());
    const quint64 listingId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("/destination")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));

    auto* const sourcePane = workspace->primaryPane();
    auto* const destinationPane = workspace->otherVisiblePane();
    sourcePane->fileTable()->selectRow(0);
    auto* const copyAction =
        window.findChild<QAction*>(QStringLiteral("clipboardCopyAction"));
    auto* const cutAction = window.findChild<QAction*>(QStringLiteral("clipboardCutAction"));
    auto* const pasteAction =
        window.findChild<QAction*>(QStringLiteral("clipboardPasteAction"));
    auto* const cancelCut = window.findChild<QAction*>(QStringLiteral("cancelCutAction"));
    QVERIFY(copyAction != nullptr);
    QVERIFY(cutAction != nullptr);
    QVERIFY(pasteAction != nullptr);

    copyAction->trigger();
    QVERIFY(!pasteAction->isEnabled());
    QVERIFY(!sourcePane->fileTable()->item(0, 0)->font().italic());
    QTest::mouseClick(destinationPane->fileTable()->viewport(), Qt::LeftButton);
    QVERIFY(pasteAction->isEnabled());
    QObject::disconnect(&window, &rfm::app::MainWindow::copyRequested, nullptr, nullptr);
    QSignalSpy copies(&window, &rfm::app::MainWindow::copyRequested);
    pasteAction->trigger();
    QCOMPARE(copies.size(), 1);
    const quint64 copyId = copies.constFirst().constFirst().toULongLong();
    QCOMPARE(copies.constFirst().at(2).toString(), QStringLiteral("/destination"));
    const rfm::core::RemoteOperationResult copyResult{
        copyId, rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/file.txt"), QStringLiteral("/destination/file.txt"), true,
          {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, copyResult)));
    QVERIFY(pasteAction->isEnabled());

    QTest::mouseClick(sourcePane->fileTable()->viewport(), Qt::LeftButton);
    sourcePane->fileTable()->selectRow(0);
    cutAction->trigger();
    QVERIFY(sourcePane->fileTable()->item(0, 0)->font().italic());
    QTest::mouseClick(destinationPane->fileTable()->viewport(), Qt::LeftButton);
    QObject::disconnect(&window, &rfm::app::MainWindow::moveRequested, nullptr, nullptr);
    QSignalSpy moves(&window, &rfm::app::MainWindow::moveRequested);
    pasteAction->trigger();
    QCOMPARE(moves.size(), 1);
    const quint64 moveId = moves.constFirst().constFirst().toULongLong();
    const rfm::core::RemoteOperationResult moveResult{
        moveId, rfm::core::RemoteOperationKind::Move,
        {{QStringLiteral("/source/file.txt"), QStringLiteral("/destination/file.txt"), true,
          {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, moveResult)));
    QVERIFY(!pasteAction->isEnabled());
    QVERIFY(!sourcePane->fileTable()->item(0, 0)->font().italic());

    QTest::mouseClick(sourcePane->fileTable()->viewport(), Qt::LeftButton);
    sourcePane->fileTable()->selectRow(0);
    cutAction->trigger();
    QVERIFY(cancelCut->isEnabled());
    cancelCut->trigger();
    QVERIFY(!pasteAction->isEnabled());
    QVERIFY(!sourcePane->fileTable()->item(0, 0)->font().italic());

    cutAction->trigger();
    QVERIFY(cancelCut->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QVERIFY(!pasteAction->isEnabled());
    QVERIFY(!cancelCut->isEnabled());
    QCOMPARE(sourcePane->fileTable()->rowCount(), 0);
}

void MainWindowTest::dragDropOffersCopyMoveAndCancelWithoutDuplicateBackendKinds()
{
    rfm::app::MainWindow window;
    window.show();
    const QList<rfm::core::RemoteEntry> sourceEntries{
        {QStringLiteral("a.txt"), 1, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "showRemoteDirectory", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("/source")),
        Q_ARG(QList<rfm::core::RemoteEntry>, sourceEntries)));
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    const quint64 listingId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("/destination")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    auto* const sourcePane = workspace->primaryPane();
    auto* const destinationPane = workspace->otherVisiblePane();
    sourcePane->fileTable()->selectAll();
    const auto payload =
        rfm::core::decodeInternalTransfer(sourcePane->createInternalDragData());
    QVERIFY(payload.has_value());
    QCOMPARE(payload->sources.size(), 2);

    QObject::disconnect(&window, &rfm::app::MainWindow::copyRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::moveRequested, nullptr, nullptr);
    QSignalSpy copies(&window, &rfm::app::MainWindow::copyRequested);
    QSignalSpy moves(&window, &rfm::app::MainWindow::moveRequested);
    const quint64 destinationPaneId = workspace->paneId(destinationPane);
    auto choose = [](const QString& buttonName) {
        QTimer::singleShot(0, [buttonName] {
            auto* const messageBox =
                qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(messageBox != nullptr);
            auto* const button = messageBox->findChild<QPushButton*>(buttonName);
            QVERIFY(button != nullptr);
            button->click();
        });
    };

    choose(QStringLiteral("dropCopyButton"));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleInternalDrop", Qt::DirectConnection,
        Q_ARG(rfm::core::InternalTransferPayload, *payload),
        Q_ARG(quint64, destinationPaneId), Q_ARG(QString, QStringLiteral("/destination"))));
    QCOMPARE(copies.size(), 1);
    QCOMPARE(moves.size(), 0);
    const quint64 copyId = copies.constFirst().constFirst().toULongLong();
    const rfm::core::RemoteOperationResult copyResult{
        copyId, rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/a.txt"), QStringLiteral("/destination/a.txt"), true, {}},
         {QStringLiteral("/source/folder"), QStringLiteral("/destination/folder"), true,
          {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, copyResult)));

    choose(QStringLiteral("dropMoveButton"));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleInternalDrop", Qt::DirectConnection,
        Q_ARG(rfm::core::InternalTransferPayload, *payload),
        Q_ARG(quint64, destinationPaneId), Q_ARG(QString, QStringLiteral("/destination"))));
    QCOMPARE(moves.size(), 1);
    const quint64 moveId = moves.constFirst().constFirst().toULongLong();
    const rfm::core::RemoteOperationResult moveResult{
        moveId, rfm::core::RemoteOperationKind::Move,
        {{QStringLiteral("/source/a.txt"), QStringLiteral("/destination/a.txt"), true, {}},
         {QStringLiteral("/source/folder"), QStringLiteral("/destination/folder"), true,
          {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, moveResult)));

    choose(QStringLiteral("dropCancelButton"));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleInternalDrop", Qt::DirectConnection,
        Q_ARG(rfm::core::InternalTransferPayload, *payload),
        Q_ARG(quint64, destinationPaneId), Q_ARG(QString, QStringLiteral("/destination"))));
    QCOMPARE(copies.size(), 1);
    QCOMPARE(moves.size(), 1);
}

void MainWindowTest::keyboardActionsExposeShortcutsAndTargetTheActivePane()
{
    rfm::app::MainWindow window;
    window.show();
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("file.txt"), 1, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/one/two")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    auto action = [&window](const QString& name) -> QAction* {
        return window.findChild<QAction*>(name);
    };
    QCOMPARE(action(QStringLiteral("renameAction"))->shortcut(), QKeySequence(Qt::Key_F2));
    QCOMPARE(action(QStringLiteral("removeAction"))->shortcut(), QKeySequence::Delete);
    QCOMPARE(action(QStringLiteral("clipboardCopyAction"))->shortcut(), QKeySequence::Copy);
    QCOMPARE(action(QStringLiteral("clipboardCutAction"))->shortcut(), QKeySequence::Cut);
    QCOMPARE(action(QStringLiteral("clipboardPasteAction"))->shortcut(), QKeySequence::Paste);
    QCOMPARE(action(QStringLiteral("selectAllAction"))->shortcut(), QKeySequence::SelectAll);
    QCOMPARE(action(QStringLiteral("refreshAction"))->shortcut(), QKeySequence(Qt::Key_F5));
    QCOMPARE(action(QStringLiteral("backAction"))->shortcut(),
             QKeySequence(QStringLiteral("Alt+Left")));
    QCOMPARE(action(QStringLiteral("forwardAction"))->shortcut(),
             QKeySequence(QStringLiteral("Alt+Right")));
    QCOMPARE(action(QStringLiteral("upAction"))->shortcut(),
             QKeySequence(QStringLiteral("Alt+Up")));
    QCOMPARE(action(QStringLiteral("focusLocationAction"))->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+L")));
    QCOMPARE(action(QStringLiteral("switchPaneAction"))->shortcut(),
             QKeySequence(Qt::Key_F6));

    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    auto* const table = workspace->activePane()->fileTable();
    action(QStringLiteral("selectAllAction"))->trigger();
    QCOMPARE(table->selectionModel()->selectedRows(0).size(), 2);
    action(QStringLiteral("focusLocationAction"))->trigger();
    QVERIFY(!workspace->activePane()->pathEdit()->selectedText().isEmpty());

    table->clearSelection();
    table->selectRow(0);
    QObject::disconnect(&window, &rfm::app::MainWindow::renameRequested, nullptr, nullptr);
    QSignalSpy renames(&window, &rfm::app::MainWindow::renameRequested);
    QTimer::singleShot(0, [] {
        auto* const dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr);
        dialog->setTextValue(QStringLiteral("renamed.txt"));
        dialog->accept();
    });
    action(QStringLiteral("renameAction"))->trigger();
    QCOMPARE(renames.size(), 1);
    const quint64 renameId = renames.constFirst().constFirst().toULongLong();
    const rfm::core::RemoteOperationResult renameResult{
        renameId, rfm::core::RemoteOperationKind::Rename,
        {{QStringLiteral("/one/two/file.txt"), QStringLiteral("/one/two/renamed.txt"), true,
          {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, renameResult)));

    QObject::disconnect(&window, &rfm::app::MainWindow::removeRequested, nullptr, nullptr);
    QSignalSpy removals(&window, &rfm::app::MainWindow::removeRequested);
    acceptNextQuestion();
    action(QStringLiteral("removeAction"))->trigger();
    QCOMPARE(removals.size(), 1);
    const quint64 removeId = removals.constFirst().constFirst().toULongLong();
    const rfm::core::RemoteOperationResult removeResult{
        removeId, rfm::core::RemoteOperationKind::Remove,
        {{QStringLiteral("/one/two/file.txt"), {}, true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleOperationResult", Qt::DirectConnection,
        Q_ARG(rfm::core::RemoteOperationResult, removeResult)));

    QSignalSpy navigation(workspace->activePane(),
                          &rfm::app::FileBrowserPane::navigationRequested);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    action(QStringLiteral("upAction"))->trigger();
    QCOMPARE(navigation.size(), 1);
    QCOMPARE(navigation.constFirst().constFirst().toString(), QStringLiteral("/one"));
    QVERIFY(!listings.isEmpty());
    quint64 listingId = listings.constLast().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("/one")), Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    navigation.clear();
    action(QStringLiteral("refreshAction"))->trigger();
    QCOMPARE(navigation.size(), 1);
    QCOMPARE(navigation.constFirst().constFirst().toString(), QStringLiteral("/one"));
    listingId = listings.constLast().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("/one")), Q_ARG(QList<rfm::core::RemoteEntry>, entries)));

    listings.clear();
    action(QStringLiteral("splitViewAction"))->trigger();
    listingId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("/other")),
        Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    auto* const firstPane = workspace->activePane();
    action(QStringLiteral("switchPaneAction"))->trigger();
    QVERIFY(workspace->activePane() != firstPane);
}

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("RemoteFileManagerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("rfm_ui_tests"));
    const rfm::core::OperationHistoryStore history;
    QFile::remove(history.filePath());
    MainWindowTest test;
    const int result = QTest::qExec(&test, argc, argv);
    QFile::remove(history.filePath());
    return result;
}

#include "test_main_window.moc"
