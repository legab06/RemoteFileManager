#include "remotefilemanager/app/ConnectionDialog.hpp"
#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/app/HomePage.hpp"
#include "remotefilemanager/app/MainWindow.hpp"
#include "remotefilemanager/app/NavigationTree.hpp"
#include "remotefilemanager/app/OperationPanel.hpp"
#include "remotefilemanager/app/PaneWorkspace.hpp"
#include "remotefilemanager/app/PasswordAuthenticationDialog.hpp"
#include "remotefilemanager/app/ServerProfileDialog.hpp"
#include "remotefilemanager/app/TransferRequestFactory.hpp"
#include "remotefilemanager/app/VolumeAuthenticationDialog.hpp"
#include "remotefilemanager/app/WorkspaceTabs.hpp"
#include "remotefilemanager/core/InternalTransfer.hpp"
#include "remotefilemanager/core/LocalFileSystem.hpp"
#include "remotefilemanager/core/OperationHistoryStore.hpp"
#include "remotefilemanager/core/ServerProfileStore.hpp"
#include "remotefilemanager/core/TransferCoordinator.hpp"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

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
    rfm::core::RemoteBackendResult copyOnServer(const QString& source, const QString& destination,
                                                bool) override
    {
        lastSource = source;
        lastDestination = destination;
        return {};
    }

    QString lastSource;
    QString lastDestination;
};

class UrlHandler final : public QObject
{
    Q_OBJECT

  public slots:
    void handleUrl(const QUrl& url) { urls.push_back(url); }

  public:
    QList<QUrl> urls;
};

class ScopedUrlHandler final
{
  public:
    ~ScopedUrlHandler() { QDesktopServices::unsetUrlHandler(QStringLiteral("file")); }
};

class FixedVolumeService final : public rfm::core::VolumeService
{
  public:
    explicit FixedVolumeService(rfm::core::VolumeOperationError error) : m_error(error) {}

    rfm::core::VolumeOperationResult
    execute(const rfm::core::VolumeOperationRequest& request) override
    {
        return {request.id, request.operation, request.target.device, m_error,
                m_error == rfm::core::VolumeOperationError::None
                    ? QString{}
                    : QStringLiteral("technical fixture detail")};
    }

  private:
    rfm::core::VolumeOperationError m_error;
};

struct BlockingVolumeServiceState {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{false};
    bool cancellationRequested{false};
    bool completed{false};
};

class BlockingVolumeService final : public rfm::core::VolumeService
{
  public:
    explicit BlockingVolumeService(std::shared_ptr<BlockingVolumeServiceState> state)
        : m_state(std::move(state))
    {}

    rfm::core::VolumeOperationResult
    execute(const rfm::core::VolumeOperationRequest& request) override
    {
        std::unique_lock lock(m_state->mutex);
        m_state->entered = true;
        m_state->condition.notify_all();
        m_state->condition.wait(lock, [this] { return m_state->cancellationRequested; });
        m_state->completed = true;
        m_state->condition.notify_all();
        return rfm::core::makeVolumeOperationResult(request,
                                                    rfm::core::VolumeOperationError::Cancelled);
    }

    void requestCancellation() override
    {
        std::lock_guard lock(m_state->mutex);
        m_state->cancellationRequested = true;
        m_state->condition.notify_all();
    }

  private:
    std::shared_ptr<BlockingVolumeServiceState> m_state;
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

rfm::core::TransferCoordinator* detachRemoteOperationExecutor(rfm::app::MainWindow& window)
{
    auto* const coordinator = window.findChild<rfm::core::TransferCoordinator*>();
    if (coordinator != nullptr) {
        QObject::disconnect(coordinator,
                            &rfm::core::TransferCoordinator::startRemoteOperationRequested, nullptr,
                            nullptr);
    }
    return coordinator;
}

QList<quint64> operationIds(QTableWidget* table)
{
    QList<quint64> ids;
    ids.reserve(table->rowCount());
    for (int row = 0; row < table->rowCount(); ++row) {
        ids.push_back(table->item(row, 0)->data(Qt::UserRole).toULongLong());
    }
    return ids;
}

quint64 topVisibleOperationId(QTableWidget* table)
{
    const int row = table->rowAt(0);
    return row < 0 ? 0 : table->item(row, 0)->data(Qt::UserRole).toULongLong();
}

int rowNamed(QTableWidget* table, const QString& name)
{
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->text() == name) {
            return row;
        }
    }
    return -1;
}

void chooseCollisionButton(const QString& label)
{
    auto* const timer = new QTimer(qApp);
    timer->setInterval(1);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, label] {
        auto* const messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (messageBox == nullptr) {
            return;
        }
        timer->stop();
        timer->deleteLater();
        for (QAbstractButton* const button : messageBox->buttons()) {
            if (button->text() == label) {
                button->click();
                return;
            }
        }
        QFAIL("Requested collision button was not found");
    });
    timer->start();
}

void chooseCrossFilesystemDropButton(const QString& objectName)
{
    QTimer::singleShot(0, [objectName] {
        auto* const messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(messageBox != nullptr);
        auto* const button = messageBox->findChild<QAbstractButton*>(objectName);
        QVERIFY(button != nullptr);
        button->click();
    });
}

void showLocalSplit(rfm::app::MainWindow& window, const QString& source, const QString& destination,
                    const QList<rfm::core::RemoteEntry>& sourceEntries,
                    const QList<rfm::core::RemoteEntry>& destinationEntries)
{
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    auto* const primary = workspace->primaryPane();
    workspace->setSplit(true);
    auto* const secondary = workspace->otherVisiblePane();
    QVERIFY(secondary != nullptr);
    primary->showDirectory({rfm::core::FileSource::Local, rfm::core::LocalMachineId, source},
                           source, sourceEntries, rfm::app::PaneNavigation::Initial);
    secondary->showDirectory({rfm::core::FileSource::Local, rfm::core::LocalMachineId, destination},
                             destination, destinationEntries, rfm::app::PaneNavigation::Initial);
    primary->setInteractionEnabled(true);
    secondary->setInteractionEnabled(true);
    auto* const stack = window.findChild<QStackedWidget*>(QStringLiteral("centralStack"));
    QVERIFY(stack != nullptr);
    stack->setCurrentWidget(window.findChild<rfm::app::WorkspaceTabs*>());
    QVERIFY(QMetaObject::invokeMethod(primary, "activated", Qt::DirectConnection));
    window.show();
    QTest::mouseClick(primary->fileTable()->viewport(), Qt::LeftButton);
}

QTreeWidgetItem* categoryItem(QTreeWidget* tree, const QString& name)
{
    if (tree == nullptr) {
        return nullptr;
    }
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        QTreeWidgetItem* const item = tree->topLevelItem(index);
        if (item->text(0) == name) {
            return item;
        }
    }
    return nullptr;
}

QTreeWidgetItem* remoteCategoryItem(QTreeWidget* tree)
{
    return categoryItem(tree, QStringLiteral("Distant"));
}

QTreeWidgetItem* serverProfileItem(QTreeWidget* tree, int index)
{
    QTreeWidgetItem* const remoteCategory = remoteCategoryItem(tree);
    return remoteCategory != nullptr && index >= 0 && index < remoteCategory->childCount()
               ? remoteCategory->child(index)
               : nullptr;
}

QTreeWidgetItem* childNamed(QTreeWidgetItem* parent, const QString& name)
{
    if (parent == nullptr) {
        return nullptr;
    }
    for (int index = 0; index < parent->childCount(); ++index) {
        if (parent->child(index)->text(0) == name) {
            return parent->child(index);
        }
    }
    return nullptr;
}

QTreeWidgetItem* localPlacesRoot(QTreeWidget* tree)
{
    return categoryItem(tree, QStringLiteral("Local"));
}

QString plainToolTip(const QTreeWidgetItem* item)
{
    QTextDocument document;
    document.setHtml(item == nullptr ? QString{} : item->toolTip(0));
    return document.toPlainText();
}

int serverProfileCount(QTreeWidget* tree)
{
    QTreeWidgetItem* const remoteCategory = remoteCategoryItem(tree);
    return remoteCategory != nullptr ? remoteCategory->childCount() : 0;
}

void selectServerProfile(QTreeWidget* tree, int index)
{
    tree->setCurrentItem(serverProfileItem(tree, index));
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

void dismissNextMessageBox()
{
    QTimer::singleShot(0, [] {
        auto* const messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(messageBox != nullptr);
        messageBox->accept();
    });
}

void setConnectionIdentity(rfm::app::MainWindow& window,
                           const QString& hostName = QStringLiteral("history.example.test"),
                           const QString& userName = QStringLiteral("test-user"),
                           const QString& initialPath = QStringLiteral("."))
{
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    window.findChild<QAction*>(QStringLiteral("newConnectionAction"))->trigger();
    auto* dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    dialog->findChild<QLineEdit*>(QStringLiteral("serverHostEdit"))->setText(hostName);
    dialog->findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"))->setText(userName);
    dialog->findChild<QSpinBox*>(QStringLiteral("serverPortSpin"))->setValue(2222);
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, initialPath),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
}

QTreeWidgetItem* volumeItemByDevice(QTreeWidgetItem* root, const QString& device)
{
    if (root == nullptr) {
        return nullptr;
    }
    if (root->data(0, Qt::UserRole + 6).isValid() &&
        root->data(0, Qt::UserRole + 6).value<rfm::core::StorageVolume>().device == device) {
        return root;
    }
    for (int index = 0; index < root->childCount(); ++index) {
        if (QTreeWidgetItem* const match = volumeItemByDevice(root->child(index), device)) {
            return match;
        }
    }
    return nullptr;
}

QTreeWidgetItem* volumeItemByDeviceAndPath(QTreeWidgetItem* root, const QString& device,
                                           const QString& path)
{
    if (root == nullptr) {
        return nullptr;
    }
    if (root->data(0, Qt::UserRole + 6).isValid()) {
        const auto volume = root->data(0, Qt::UserRole + 6).value<rfm::core::StorageVolume>();
        if (volume.device == device && volume.rootPath == path) {
            return root;
        }
    }
    for (int index = 0; index < root->childCount(); ++index) {
        if (QTreeWidgetItem* const match =
                volumeItemByDeviceAndPath(root->child(index), device, path)) {
            return match;
        }
    }
    return nullptr;
}

void showPaneLocation(rfm::app::FileBrowserPane* pane, rfm::core::FileSource source,
                      const QString& path,
                      rfm::app::PaneNavigation navigation = rfm::app::PaneNavigation::Initial)
{
    const QString machineId = source == rfm::core::FileSource::Local
                                  ? QString::fromLatin1(rfm::core::LocalMachineId)
                                  : QStringLiteral("ssh:fixture");
    pane->showDirectory({source, machineId, path}, path, {}, navigation);
}

void waitForInitialLocalStorageRefresh(rfm::app::MainWindow& window)
{
    QObject::disconnect(&window, &rfm::app::MainWindow::localStorageProbeRequested, nullptr,
                        nullptr);
    QSignalSpy probes(&window, &rfm::app::MainWindow::localStorageProbeRequested);
    auto* const timer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(timer != nullptr);
    QTRY_VERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection) &&
                !probes.isEmpty());
}

} // namespace

class MainWindowTest final : public QObject
{
    Q_OBJECT

  private slots:
    void exposesInitialDisconnectedShell();
    void workspaceActionsResolveCurrentTab();
    void persistsShowHiddenFilesPreference();
    void resetFileViewActionResetsAllPanes();
    void dockVisibilityActionsTrackPanels();
    void validatesSecureConnectionForm();
    void keepsConnectionDialogOpenAcrossFailureAndRetry();
    void savedServerConnectsDirectlyAndUsesCompactAuthentication();
    void homeAndNavigationDoubleClickConnectDirectly();
    void passwordPromptHandlesRejectionAndCancellation();
    void placesButtonTracksActiveAndSelectedProfiles();
    void addsEditsAndRemovesSavedServers();
    void savesManualServerOnlyAfterSuccessAndAvoidsDuplicates();
    void disconnectActionFollowsSessionLifecycle();
    void coalescesConnectionErrorsFromOneDisconnect();
    void terminalTransferReleasesDisconnectProtection();
    void enablesMultipleRemoteSelection();
    void buildsPortableTransferRequests();
    void displaysTransferProgressAndMultipleEntries();
    void ordersOperationRowsWithoutDuplicates();
    void restoresOperationHistoryInSingleRebuild();
    void keepsOperationRowsSingleLineAndRetainsExplicitCurrentItem();
    void preservesOperationPanelContextAcrossReordering();
    void displaysDirectoryTransferDetails();
    void displaysTransferStatesInEnglish();
    void displaysRemoteCopyAndMoveOperations();
    void acceptsRemoteMoveProgressFromWorker();
    void removesOnlyTerminalOperationsFromPanel();
    void exposesPauseResumeAndCancelIntentions();
    void displaysTerminalTransferStatesAndErrors();
    void formatsTransferSizesAndSpeeds();
    void displaysSpeedOnlyForRunningTransfers();
    void displaysQueuedOperationUntilWorkerStarts();
    void displaysLocalSuccessWarning();
    void refreshTimerIsConnectionAwareAndCoalescesListings();
    void refreshesAfterCompletedUpload();
    void localAndRemoteNavigationShowOpeningStatusMessage();
    void manualRefreshShowsStatusMessage();
    void crossSourceMoveUnsupportedReplacesRefreshMessage();
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
    void queuesRemoteCopiesWithoutBlockingTheWindow();
    void remoteExecutorFailureShowsOnlyConnectionDialog();
    void fileContextMenuOffersProperties_data();
    void fileContextMenuOffersProperties();
    void contextMenuOffersOpenForSingleLocalFilesOnly();
    void contextMenuUsesSharedInterPaneActions();
    void contextMenuUsesClipboardAndKeepsExplicitDestinationActions();
    void buildsCanonicalInterPanePathsThroughTheRealUiChain();
    void rejectsOtherPaneOperationsForSameDirectory();
    void refreshesAllVisiblePanesAffectedByOperationsAndUploads();
    void persistsRemovesAndClearsTerminalOperationHistory();
    void clipboardCopiesCutsPastesAndClearsSuccessfulMove();
    void remoteDragDropRoutesModifierIntentionsToExistingRequests();
    void remoteCrossFilesystemPreflightRoutesOriginalDrop();
    void localDragDropRoutesModifierIntentionsToExistingRequests_data();
    void localDragDropRoutesModifierIntentionsToExistingRequests();
    void promptsForConfirmedCrossVolumeLocalDragDrop();
    void localToSshDragDropQueuesExistingUploadsAndRejectsMove();
    void sshToLocalDragDropQueuesExistingDownloadsAndRejectsStaleSessions();
    void copyToOtherPaneQueuesCrossSourceTransfers();
    void keyboardActionsExposeShortcutsAndTargetTheActivePane();
    void opensLocalDirectoryWithoutSshAndNavigatesAsynchronously();
    void opensLocalFilesWithTheSystemUrlHandler();
    void mutatesLocalEntriesAndRefreshesMatchingPanes();
    void preservesCutAfterIndependentLocalMove();
    void consumesCutAfterSuccessfulLocalPaste();
    void allowsCopyFromReadOnlyLocalDirectory();
    void aggregatesLocalCollisionSkip();
    void aggregatesLocalCollisionReplace();
    void cancelsLocalCollisionAfterPartialSuccess();
    void keepsLocalAndRemoteSourcesDistinctAcrossSplitAndDisconnect();
    void mainRefreshWorksWithoutConnectionAndUpdatesOpenTree();
    void mainRefreshRequestsRemoteDiscoveryAndCoalescesRequests();
    void temporaryConnectionAppearsInPlacesAndRejectsStaleStorage();
    void ignoresStaleResultsAfterSwitchingNavigationSource();
    void refreshesRemoteStorageOnlyAfterProbeFingerprintChanges();
    void volumeRequestsPreserveDeviceMountPoints();
    void volumeRequestsPreserveInconsistentSiblingEvidence();
    void volumeOperationSuccessWaitsForSystemRefresh();
    void volumeOperationErrorsRestoreUi_data();
    void volumeOperationErrorsRestoreUi();
    void closingWindowCancelsBusyLocalVolumeWorker();
    void successfulUnmountEvacuatesOnlyAffectedPanes_data();
    void successfulUnmountEvacuatesOnlyAffectedPanes();
    void failedUnmountDoesNotEvacuatePane();
    void safetyFallbackPurgesUnmountedPathsFromHistory();
    void remoteMountWaitsForRefreshAndOpensObservedMountPoint();
    void remoteAuthenticationDialogShowsContextAndCancelReleasesBusy();
    void remoteAuthenticationSubmitsEphemeralPassword();
    void remoteInteractiveBusinessErrorsReleaseBusy_data();
    void remoteInteractiveBusinessErrorsReleaseBusy();
    void disconnectClosesRemoteAuthenticationDialog();
    void failedRemoteUnmountDoesNotEvacuateOrRefresh();
    void remoteTimeoutRestoresUiWithoutRefresh();
    void successfulRemoteUnmountEvacuatesOnlyMatchingNamespace();
    void remoteSafetyFallbackPurgesOnlyMatchingHistory();
    void remoteDisconnectClearsPendingVolumeState();
    void remoteDisconnectAfterCommandBeforeRefreshKeepsObservedModel();
};

void MainWindowTest::volumeRequestsPreserveDeviceMountPoints()
{
    const QString device = QStringLiteral("/dev/sde1");
    rfm::core::StorageVolume first;
    first.displayName = QStringLiteral("First attachment");
    first.device = device;
    first.rootPath = QStringLiteral("/mnt/a");
    first.kind = rfm::core::StorageKind::External;
    rfm::core::StorageVolume second = first;
    second.displayName = QStringLiteral("Second attachment");
    second.rootPath = QStringLiteral("/mnt/b");
    const QList<rfm::core::StorageVolume> volumes{first, second};

    {
        rfm::app::MainWindow window;
        QObject::disconnect(&window, &rfm::app::MainWindow::volumeOperationRequested, nullptr,
                            nullptr);
        QSignalSpy operations(&window, &rfm::app::MainWindow::volumeOperationRequested);
        QVERIFY(QMetaObject::invokeMethod(&window, "handleLocalStorageVolumes",
                                          Qt::DirectConnection,
                                          Q_ARG(QList<rfm::core::StorageVolume>, volumes)));
        auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
        QTreeWidgetItem* const selected =
            volumeItemByDeviceAndPath(localPlacesRoot(navigation->tree()), device, first.rootPath);
        QVERIFY(selected != nullptr);
        navigation->tree()->setCurrentItem(selected);
        window.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"))->click();
        QCOMPARE(operations.size(), 1);
        const auto request =
            operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
        QCOMPARE(request.target.mountPoint, first.rootPath);
        QCOMPARE(request.target.knownMountPoints,
                 QStringList({QStringLiteral("/mnt/a"), QStringLiteral("/mnt/b")}));
    }

    {
        rfm::app::MainWindow window;
        QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr,
                            nullptr);
        QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                            nullptr);
        QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
        QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
        setConnectionIdentity(window, QStringLiteral("multi.example.test"));
        const quint64 refreshId = storageRequests.constFirst().constFirst().toULongLong();
        QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteStorageVolumes",
                                          Qt::DirectConnection, Q_ARG(quint64, refreshId),
                                          Q_ARG(QList<rfm::core::StorageVolume>, volumes)));
        auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
        QTreeWidgetItem* const selected = volumeItemByDeviceAndPath(
            remoteCategoryItem(navigation->tree()), device, first.rootPath);
        QVERIFY(selected != nullptr);
        navigation->tree()->setCurrentItem(selected);
        window.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"))->click();
        QCOMPARE(operations.size(), 1);
        const auto request =
            operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
        QCOMPARE(request.target.mountPoint, first.rootPath);
        QCOMPARE(request.target.knownMountPoints,
                 QStringList({QStringLiteral("/mnt/a"), QStringLiteral("/mnt/b")}));
    }
}

void MainWindowTest::volumeRequestsPreserveInconsistentSiblingEvidence()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::volumeOperationRequested, nullptr, nullptr);
    QSignalSpy operations(&window, &rfm::app::MainWindow::volumeOperationRequested);
    rfm::core::StorageVolume selected;
    selected.displayName = QStringLiteral("Selected attachment");
    selected.device = QStringLiteral("/dev/sde1");
    selected.rootPath = QStringLiteral("/mnt/data");
    selected.kind = rfm::core::StorageKind::External;
    rfm::core::StorageVolume invalidSibling = selected;
    invalidSibling.displayName = QStringLiteral("Invalid sibling");
    invalidSibling.rootPath.clear();
    rfm::core::StorageVolume duplicateSibling = selected;
    duplicateSibling.displayName = QStringLiteral("Duplicate sibling");
    rfm::core::StorageVolume contradictorySibling = selected;
    contradictorySibling.displayName = QStringLiteral("Contradictory sibling");
    contradictorySibling.mounted = false;
    contradictorySibling.rootPath.clear();
    const QList<rfm::core::StorageVolume> volumes{selected, invalidSibling, duplicateSibling,
                                                  contradictorySibling};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleLocalStorageVolumes", Qt::DirectConnection,
                                      Q_ARG(QList<rfm::core::StorageVolume>, volumes)));
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    QTreeWidgetItem* const item = volumeItemByDeviceAndPath(localPlacesRoot(navigation->tree()),
                                                            selected.device, selected.rootPath);
    QVERIFY(item != nullptr);
    navigation->tree()->setCurrentItem(item);

    window.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"))->click();

    QCOMPARE(operations.size(), 1);
    const auto request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    QCOMPARE(request.target.knownMountPoints,
             QStringList(
                 {QStringLiteral("/mnt/data"), QString{}, QStringLiteral("/mnt/data"), QString{}}));
    QCOMPARE(rfm::core::volumeUnmountTargetMode(request),
             rfm::core::VolumeUnmountTargetMode::Invalid);
}

void MainWindowTest::mainRefreshWorksWithoutConnectionAndUpdatesOpenTree()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    rfm::app::MainWindow window;
    auto* const action = window.findChild<QAction*>(QStringLiteral("refreshAction"));
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(action != nullptr);
    QVERIFY(navigation != nullptr);
    QVERIFY(workspace != nullptr);
    QVERIFY(window.findChild<QObject*>(QStringLiteral("storageRefreshAction")) == nullptr);
    QVERIFY(window.findChild<QObject*>(QStringLiteral("storageRefreshButton")) == nullptr);

    waitForInitialLocalStorageRefresh(window);
    QObject::disconnect(&window, &rfm::app::MainWindow::localVolumesRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::localDirectoryRequested, nullptr, nullptr);
    showPaneLocation(workspace->primaryPane(), rfm::core::FileSource::Local, first.path());
    QVERIFY(action->isEnabled());
    QSignalSpy localRequests(&window, &rfm::app::MainWindow::localVolumesRequested);
    QSignalSpy remoteRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy directoryRequests(&window, &rfm::app::MainWindow::localDirectoryRequested);
    action->trigger();
    QCOMPARE(localRequests.size(), 1);
    QCOMPARE(remoteRequests.size(), 0);
    QCOMPARE(directoryRequests.size(), 1);

    QList<rfm::core::StorageVolume> initial{{QStringLiteral("First"),
                                             first.path(),
                                             {},
                                             {},
                                             0,
                                             rfm::core::StorageKind::Internal,
                                             false,
                                             false,
                                             false}};
    initial[0].device = QStringLiteral("/dev/first1");
    initial[0].fileSystemType = QByteArrayLiteral("ext4");
    initial[0].deviceModel = QStringLiteral("First model");
    const quint64 firstDirectoryRequest = directoryRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleLocalDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, firstDirectoryRequest), Q_ARG(QString, first.path()),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleLocalStorageVolumes", Qt::DirectConnection,
                                      Q_ARG(QList<rfm::core::StorageVolume>, initial)));
    QVERIFY(action->isEnabled());

    QTreeWidgetItem* const localMachine = localPlacesRoot(navigation->tree());
    QTreeWidgetItem* volumes = nullptr;
    for (int index = 0; index < localMachine->childCount(); ++index) {
        if (localMachine->child(index)->text(0) == QStringLiteral("Volumes")) {
            volumes = localMachine->child(index);
        }
    }
    QVERIFY(volumes != nullptr);
    QCOMPARE(volumes->childCount(), 1);
    QCOMPARE(volumes->child(0)->text(0), first.path());
    QVERIFY(plainToolTip(volumes->child(0)).contains(QStringLiteral("Filesystem: ext4")));
    QVERIFY(plainToolTip(volumes->child(0)).contains(QStringLiteral("Model: First model")));

    QTreeWidgetItem* const firstVolume = volumes->child(0);
    firstVolume->setExpanded(true);
    QCOMPARE(directoryRequests.size(), 2);
    const quint64 treeRequest = directoryRequests.at(1).constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> beforeEntries{
        {QStringLiteral("before"), 0, {}, true, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleLocalDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, treeRequest), Q_ARG(QString, first.path()),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, beforeEntries)));
    QVERIFY(firstVolume->isExpanded());
    QVERIFY(childNamed(firstVolume, QStringLiteral("before")) != nullptr);
    navigation->tree()->setCurrentItem(firstVolume);

    action->trigger();
    QCOMPARE(localRequests.size(), 2);
    QCOMPARE(directoryRequests.size(), 3);
    const quint64 secondDirectoryRequest = directoryRequests.at(2).constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> afterEntries{{QStringLiteral("after"), 0, {}, true, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleLocalDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, secondDirectoryRequest),
                                      Q_ARG(QString, first.path()),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, afterEntries)));
    QVERIFY(childNamed(firstVolume, QStringLiteral("before")) == nullptr);
    QVERIFY(childNamed(firstVolume, QStringLiteral("after")) != nullptr);
    QVERIFY(firstVolume->isExpanded());
    QCOMPARE(navigation->tree()->currentItem(), firstVolume);

    QList<rfm::core::StorageVolume> replacement{{QStringLiteral("Second"),
                                                 second.path(),
                                                 {},
                                                 {},
                                                 0,
                                                 rfm::core::StorageKind::Internal,
                                                 false,
                                                 false,
                                                 false}};
    replacement[0].device = QStringLiteral("/dev/second1");
    replacement[0].fileSystemType = QByteArrayLiteral("xfs");
    replacement[0].deviceModel = QStringLiteral("Second model");
    QVERIFY(QMetaObject::invokeMethod(&window, "handleLocalStorageVolumes", Qt::DirectConnection,
                                      Q_ARG(QList<rfm::core::StorageVolume>, replacement)));
    QCOMPARE(volumes->childCount(), 1);
    QCOMPARE(volumes->child(0)->text(0), second.path());
    const QString replacementToolTip = plainToolTip(volumes->child(0));
    QVERIFY(replacementToolTip.contains(QStringLiteral("Device: /dev/second1")));
    QVERIFY(replacementToolTip.contains(QStringLiteral("Filesystem: xfs")));
    QVERIFY(replacementToolTip.contains(QStringLiteral("Model: Second model")));
    QVERIFY(!replacementToolTip.contains(QStringLiteral("First model")));
}

void MainWindowTest::mainRefreshRequestsRemoteDiscoveryAndCoalescesRequests()
{
    QTemporaryDir history;
    QTemporaryDir profiles;
    QVERIFY(history.isValid());
    QVERIFY(profiles.isValid());
    rfm::app::MainWindow window(nullptr, history.path(), profiles.path());
    auto* const action = window.findChild<QAction*>(QStringLiteral("refreshAction"));
    QVERIFY(action != nullptr);

    waitForInitialLocalStorageRefresh(window);
    QObject::disconnect(&window, &rfm::app::MainWindow::localVolumesRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy localRequests(&window, &rfm::app::MainWindow::localVolumesRequested);
    QSignalSpy remoteRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy directoryRequests(&window, &rfm::app::MainWindow::directoryRequested);
    setConnectionIdentity(window);
    QCOMPARE(remoteRequests.size(), 1);
    const quint64 requestId = remoteRequests.constFirst().constFirst().toULongLong();
    QVERIFY(requestId != 0);
    action->trigger();
    QCOMPARE(remoteRequests.size(), 1);
    QCOMPARE(localRequests.size(), 1);
    QCOMPARE(directoryRequests.size(), 1);

    const QList<rfm::core::StorageVolume> remoteVolumes{
        {QStringLiteral("USB"), QStringLiteral("/media/usb"), QStringLiteral("/dev/sdb1"),
         QByteArrayLiteral("vfat"), 0, rfm::core::StorageKind::External, false, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteStorageVolumes", Qt::DirectConnection,
                                      Q_ARG(quint64, requestId),
                                      Q_ARG(QList<rfm::core::StorageVolume>, remoteVolumes)));
    const quint64 firstDirectoryRequest = directoryRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, firstDirectoryRequest), Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleLocalStorageVolumes", Qt::DirectConnection,
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{})));
    QVERIFY(action->isEnabled());
    action->trigger();
    QCOMPARE(remoteRequests.size(), 2);
    QCOMPARE(localRequests.size(), 2);
    QCOMPARE(directoryRequests.size(), 2);

    const quint64 secondRequestId = remoteRequests.at(1).constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection,
        Q_ARG(quint64, secondRequestId),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{})));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QVERIFY(!action->isEnabled());
}

void MainWindowTest::temporaryConnectionAppearsInPlacesAndRejectsStaleStorage()
{
    QTemporaryDir history;
    QTemporaryDir profiles;
    QVERIFY(history.isValid());
    QVERIFY(profiles.isValid());
    rfm::app::MainWindow window(nullptr, history.path(), profiles.path());
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(navigation != nullptr);
    QVERIFY(workspace != nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy directoryRequests(&window, &rfm::app::MainWindow::directoryRequested);

    setConnectionIdentity(window, QStringLiteral("server-a.test"), QStringLiteral("alice"));
    QCOMPARE(storageRequests.size(), 1);
    const quint64 serverARequest = storageRequests.constFirst().constFirst().toULongLong();
    QTreeWidgetItem* const servers = remoteCategoryItem(navigation->tree());
    QCOMPARE(servers->childCount(), 1);
    QTreeWidgetItem* const serverA = servers->child(0);
    QVERIFY(serverA->text(0).contains(QStringLiteral("Connected")));
    QVERIFY(childNamed(serverA, QStringLiteral("Home")) != nullptr);
    QVERIFY(childNamed(serverA, QStringLiteral("/")) != nullptr);
    QVERIFY(childNamed(serverA, QStringLiteral("Volumes")) != nullptr);
    const QString serverAMachineId = serverA->data(0, Qt::UserRole + 2).toString();
    QVERIFY(serverAMachineId.contains(QStringLiteral("server-a.test")));

    const QList<rfm::core::StorageVolume> serverAVolumes{
        {QStringLiteral("SERVER_A_USB"), QStringLiteral("/media/a"), QStringLiteral("/dev/sdb1"),
         QByteArrayLiteral("vfat"), 0, rfm::core::StorageKind::External, false, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteStorageVolumes", Qt::DirectConnection,
                                      Q_ARG(quint64, serverARequest),
                                      Q_ARG(QList<rfm::core::StorageVolume>, serverAVolumes)));
    QTreeWidgetItem* const externalA = childNamed(serverA, QStringLiteral("External devices"));
    QVERIFY(externalA != nullptr);
    QCOMPARE(externalA->childCount(), 1);
    QCOMPARE(externalA->child(0)->text(0), QStringLiteral("/media/a"));
    rfm::app::FileBrowserPane* const activePane = workspace->activePane();
    QVERIFY(QMetaObject::invokeMethod(navigation->tree(), "itemActivated", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, externalA->child(0)), Q_ARG(int, 0)));
    QCOMPARE(directoryRequests.size(), 1);
    QCOMPARE(directoryRequests.constFirst().at(1).toString(), QStringLiteral("/media/a"));
    QCOMPARE(workspace->activePane(), activePane);

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QTRY_VERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);
    setConnectionIdentity(window, QStringLiteral("server-b.test"), QStringLiteral("bob"));
    QCOMPARE(storageRequests.size(), 2);
    const quint64 serverBRequest = storageRequests.at(1).constFirst().toULongLong();
    QVERIFY(serverBRequest != serverARequest);
    QCOMPARE(servers->childCount(), 1);
    QTreeWidgetItem* const serverB = servers->child(0);
    QVERIFY(
        serverB->data(0, Qt::UserRole + 2).toString().contains(QStringLiteral("server-b.test")));

    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteStorageVolumes", Qt::DirectConnection,
                                      Q_ARG(quint64, serverARequest),
                                      Q_ARG(QList<rfm::core::StorageVolume>, serverAVolumes)));
    QVERIFY(childNamed(serverB, QStringLiteral("External devices")) == nullptr);

    const QList<rfm::core::StorageVolume> serverBVolumes{
        {QStringLiteral("SERVER_B_USB"), QStringLiteral("/media/b"), QStringLiteral("/dev/sdc1"),
         QByteArrayLiteral("vfat"), 0, rfm::core::StorageKind::External, false, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteStorageVolumes", Qt::DirectConnection,
                                      Q_ARG(quint64, serverBRequest),
                                      Q_ARG(QList<rfm::core::StorageVolume>, serverBVolumes)));
    QTreeWidgetItem* const externalB = childNamed(serverB, QStringLiteral("External devices"));
    QVERIFY(externalB != nullptr);
    QCOMPARE(externalB->childCount(), 1);
    QCOMPARE(externalB->child(0)->text(0), QStringLiteral("/media/b"));
}

void MainWindowTest::ignoresStaleResultsAfterSwitchingNavigationSource()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::MainWindow window;
    setConnectionIdentity(window);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    QVERIFY(workspace != nullptr);
    QVERIFY(navigation != nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::localDirectoryRequested, nullptr, nullptr);
    QSignalSpy remoteRequests(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy localRequests(&window, &rfm::app::MainWindow::localDirectoryRequested);

    window.findChild<QAction*>(QStringLiteral("refreshAction"))->trigger();
    QCOMPARE(remoteRequests.size(), 1);
    const quint64 staleRemoteId = remoteRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(navigation, "localLocationActivated", Qt::DirectConnection,
                                      Q_ARG(QString, temporary.path())));
    QCOMPARE(localRequests.size(), 1);
    const quint64 localId = localRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleLocalDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, localId),
        Q_ARG(QString, temporary.path()),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QCOMPARE(workspace->activePane()->source(), rfm::core::FileSource::Local);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, staleRemoteId),
        Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QCOMPARE(workspace->activePane()->source(), rfm::core::FileSource::Local);

    QVERIFY(QMetaObject::invokeMethod(navigation, "localLocationActivated", Qt::DirectConnection,
                                      Q_ARG(QString, temporary.path())));
    QCOMPARE(localRequests.size(), 2);
    const quint64 staleLocalId = localRequests.at(1).constFirst().toULongLong();
    QTreeWidgetItem* const server = serverProfileItem(navigation->tree(), 0);
    QVERIFY(server != nullptr);
    const QString machineId = server->data(0, Qt::UserRole + 2).toString();
    QVERIFY(QMetaObject::invokeMethod(navigation, "remoteLocationActivated", Qt::DirectConnection,
                                      Q_ARG(QString, machineId),
                                      Q_ARG(QString, QStringLiteral("/srv"))));
    QCOMPARE(remoteRequests.size(), 2);
    const quint64 remoteId = remoteRequests.at(1).constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, remoteId),
        Q_ARG(QString, QStringLiteral("/srv")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QCOMPARE(workspace->activePane()->source(), rfm::core::FileSource::Ssh);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleLocalDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, staleLocalId),
        Q_ARG(QString, temporary.path()),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QCOMPARE(workspace->activePane()->source(), rfm::core::FileSource::Ssh);
}

void MainWindowTest::refreshesRemoteStorageOnlyAfterProbeFingerprintChanges()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageProbeRequested, nullptr,
                        nullptr);
    QSignalSpy refreshes(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy probes(&window, &rfm::app::MainWindow::remoteStorageProbeRequested);
    setConnectionIdentity(window);
    QCOMPARE(refreshes.size(), 1);
    const quint64 initialRefresh = refreshes.constFirst().constFirst().toULongLong();
    const QByteArray fingerprint = QByteArrayLiteral("initial-mounts");
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteStorageFingerprint",
                                      Qt::DirectConnection, Q_ARG(quint64, initialRefresh),
                                      Q_ARG(QByteArray, fingerprint)));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, initialRefresh),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{})));

    QVERIFY(QMetaObject::invokeMethod(&window, "probeStorage", Qt::DirectConnection));
    QCOMPARE(probes.size(), 1);
    const quint64 unchangedProbe = probes.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteStorageProbe", Qt::DirectConnection,
                                      Q_ARG(quint64, unchangedProbe),
                                      Q_ARG(QByteArray, fingerprint)));
    QCOMPARE(refreshes.size(), 1);

    QVERIFY(QMetaObject::invokeMethod(&window, "probeStorage", Qt::DirectConnection));
    QCOMPARE(probes.size(), 2);
    const quint64 changedProbe = probes.at(1).constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteStorageProbe", Qt::DirectConnection,
                                      Q_ARG(quint64, changedProbe),
                                      Q_ARG(QByteArray, QByteArrayLiteral("changed-mounts"))));
    QCOMPARE(refreshes.size(), 2);
}

void MainWindowTest::opensLocalDirectoryWithoutSshAndNavigatesAsynchronously()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QFile file(QDir(temporary.path()).filePath(QStringLiteral("local.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("local"), qint64{5});
    file.close();

    rfm::app::MainWindow window;
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const stack = window.findChild<QStackedWidget*>(QStringLiteral("centralStack"));
    QVERIFY(navigation != nullptr);
    QVERIFY(workspace != nullptr);
    QVERIFY(stack != nullptr);
    QVERIFY(QMetaObject::invokeMethod(navigation, "localLocationActivated", Qt::DirectConnection,
                                      Q_ARG(QString, temporary.path())));

    QTRY_COMPARE(workspace->activePane()->source(), rfm::core::FileSource::Local);
    QCOMPARE(workspace->activePane()->currentPath(), QDir(temporary.path()).absolutePath());
    QCOMPARE(workspace->activePane()->fileTable()->rowCount(), 1);
    QCOMPARE(workspace->activePane()->fileTable()->item(0, 0)->text(), QStringLiteral("local.txt"));
    QCOMPARE(stack->currentWidget(), window.findChild<rfm::app::WorkspaceTabs*>());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("refreshAction"))->isEnabled());
    auto* const createAction = window.findChild<QAction*>(QStringLiteral("createDirectoryAction"));
    auto* const renameAction = window.findChild<QAction*>(QStringLiteral("renameAction"));
    auto* const removeAction = window.findChild<QAction*>(QStringLiteral("removeAction"));
    QVERIFY(createAction->isEnabled());
    QVERIFY(!renameAction->isEnabled());
    QVERIFY(!removeAction->isEnabled());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("uploadAction")) == nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("downloadAction")) == nullptr);
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("copyAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("moveAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("clipboardCopyAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("clipboardCutAction"))->isEnabled());
    workspace->activePane()->fileTable()->selectRow(0);
    QVERIFY(renameAction->isEnabled());
    QVERIFY(removeAction->isEnabled());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("copyAction"))->isEnabled());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("moveAction"))->isEnabled());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("clipboardCopyAction"))->isEnabled());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("clipboardCutAction"))->isEnabled());
}

void MainWindowTest::opensLocalFilesWithTheSystemUrlHandler()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString filePath = QDir(temporary.path()).filePath(QStringLiteral("document.txt"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    rfm::app::MainWindow window;
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    auto* const pane = workspace->activePane();
    pane->showDirectory({rfm::core::FileSource::Local,
                         QString::fromLatin1(rfm::core::LocalMachineId), temporary.path()},
                        QUrl::fromLocalFile(temporary.path()).toDisplayString(),
                        {{QStringLiteral("document.txt"), 0, {}, false, false}});

    UrlHandler handler;
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &handler, "handleUrl");
    ScopedUrlHandler resetHandler;
    QVERIFY(QMetaObject::invokeMethod(pane->fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 0), Q_ARG(int, 0)));

    QCOMPARE(handler.urls.size(), 1);
    QCOMPARE(handler.urls.constFirst(), QUrl::fromLocalFile(filePath));
}

void MainWindowTest::mutatesLocalEntriesAndRefreshesMatchingPanes()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::MainWindow window;
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(navigation != nullptr);
    QVERIFY(workspace != nullptr);
    waitForInitialLocalStorageRefresh(window);
    QObject::disconnect(&window, &rfm::app::MainWindow::localVolumesRequested, nullptr, nullptr);
    rfm::core::StorageVolume fixtureVolume;
    fixtureVolume.displayName = QStringLiteral("Local mutation fixture");
    fixtureVolume.rootPath = temporary.path();
    fixtureVolume.device = QStringLiteral("fixture-local-mutations");
    fixtureVolume.kind = rfm::core::StorageKind::Internal;
    navigation->setStorageVolumes({fixtureVolume});
    QTreeWidgetItem* const volumes =
        childNamed(localPlacesRoot(navigation->tree()), QStringLiteral("Volumes"));
    QVERIFY(volumes != nullptr);
    QTreeWidgetItem* const fixture = childNamed(volumes, temporary.path());
    QVERIFY(fixture != nullptr);
    fixture->setExpanded(true);
    QTRY_COMPARE(fixture->childCount(), 0);
    navigation->tree()->setCurrentItem(fixture);

    QVERIFY(QMetaObject::invokeMethod(navigation, "localLocationActivated", Qt::DirectConnection,
                                      Q_ARG(QString, temporary.path())));
    QTRY_COMPARE(workspace->primaryPane()->source(), rfm::core::FileSource::Local);

    workspace->setSplit(true);
    rfm::app::FileBrowserPane* const primary = workspace->primaryPane();
    rfm::app::FileBrowserPane* const secondary = workspace->otherVisiblePane();
    QVERIFY(secondary != nullptr);
    QTRY_COMPARE(secondary->source(), rfm::core::FileSource::Local);
    QCOMPARE(secondary->currentPath(), primary->currentPath());

    auto* const createAction = window.findChild<QAction*>(QStringLiteral("createDirectoryAction"));
    auto* const renameAction = window.findChild<QAction*>(QStringLiteral("renameAction"));
    auto* const removeAction = window.findChild<QAction*>(QStringLiteral("removeAction"));
    QVERIFY(QMetaObject::invokeMethod(primary, "activated", Qt::DirectConnection));
    QVERIFY(createAction->isEnabled());
    QTimer::singleShot(0, [] {
        auto* const dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr);
        dialog->setTextValue(QStringLiteral("created"));
        dialog->accept();
    });
    createAction->trigger();
    const QString createdPath = QDir(temporary.path()).filePath(QStringLiteral("created"));
    QTRY_VERIFY(QFileInfo(createdPath).isDir());
    QTRY_VERIFY(rowNamed(primary->fileTable(), QStringLiteral("created")) >= 0);
    QTRY_VERIFY(rowNamed(secondary->fileTable(), QStringLiteral("created")) >= 0);
    QTRY_VERIFY(childNamed(fixture, QStringLiteral("created")) != nullptr);
    QVERIFY(fixture->isExpanded());
    QCOMPARE(navigation->tree()->currentItem(), fixture);

    primary->fileTable()->selectRow(rowNamed(primary->fileTable(), QStringLiteral("created")));
    QVERIFY(renameAction->isEnabled());
    QTimer::singleShot(0, [] {
        auto* const dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr);
        dialog->setTextValue(QStringLiteral("renamed"));
        dialog->accept();
    });
    renameAction->trigger();
    const QString renamedPath = QDir(temporary.path()).filePath(QStringLiteral("renamed"));
    QTRY_VERIFY(!QFileInfo(createdPath).exists() && QFileInfo(renamedPath).isDir());
    QTRY_VERIFY(rowNamed(primary->fileTable(), QStringLiteral("renamed")) >= 0);
    QTRY_VERIFY(rowNamed(secondary->fileTable(), QStringLiteral("renamed")) >= 0);
    QTRY_VERIFY(childNamed(fixture, QStringLiteral("created")) == nullptr);
    QTRY_VERIFY(childNamed(fixture, QStringLiteral("renamed")) != nullptr);
    QVERIFY(fixture->isExpanded());
    QCOMPARE(navigation->tree()->currentItem(), fixture);

    primary->fileTable()->selectRow(rowNamed(primary->fileTable(), QStringLiteral("renamed")));
    QVERIFY(removeAction->isEnabled());
    acceptNextQuestion();
    removeAction->trigger();
    QTRY_VERIFY(!QFileInfo(renamedPath).exists());
    QTRY_COMPARE(rowNamed(primary->fileTable(), QStringLiteral("renamed")), -1);
    QTRY_COMPARE(rowNamed(secondary->fileTable(), QStringLiteral("renamed")), -1);
    QTRY_VERIFY(childNamed(fixture, QStringLiteral("renamed")) == nullptr);
    QVERIFY(fixture->isExpanded());
    QCOMPARE(navigation->tree()->currentItem(), fixture);
}

void MainWindowTest::preservesCutAfterIndependentLocalMove()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = QDir(temporary.path()).filePath(QStringLiteral("source"));
    const QString destination = QDir(temporary.path()).filePath(QStringLiteral("destination"));
    QVERIFY(QDir().mkpath(source));
    QVERIFY(QDir().mkpath(destination));
    for (const QString& name : {QStringLiteral("A"), QStringLiteral("B")}) {
        QFile file(QDir(source).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(name.toUtf8()) == name.size());
        file.close();
    }
    rfm::app::MainWindow window;
    showLocalSplit(
        window, source, destination,
        {{QStringLiteral("A"), 1, {}, false, false}, {QStringLiteral("B"), 1, {}, false, false}},
        {});
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const primary = workspace->primaryPane();
    primary->fileTable()->selectRow(0);
    QCOMPARE(primary->selectedEntries().size(), 1);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("clipboardCutAction"))->isEnabled());
    window.findChild<QAction*>(QStringLiteral("clipboardCutAction"))->trigger();
    QVERIFY(window.findChild<QAction*>(QStringLiteral("cancelCutAction"))->isEnabled());
    primary->fileTable()->selectRow(1);
    window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction"))->trigger();
    QTRY_VERIFY(QFileInfo(QDir(destination).filePath(QStringLiteral("B"))).exists());
    QTRY_VERIFY(window.findChild<QAction*>(QStringLiteral("cancelCutAction"))->isEnabled());
}

void MainWindowTest::consumesCutAfterSuccessfulLocalPaste()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = QDir(temporary.path()).filePath(QStringLiteral("source"));
    const QString destination = QDir(temporary.path()).filePath(QStringLiteral("destination"));
    QVERIFY(QDir().mkpath(source));
    QVERIFY(QDir().mkpath(destination));
    QFile file(QDir(source).filePath(QStringLiteral("A")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("A") == 1);
    file.close();
    rfm::app::MainWindow window;
    showLocalSplit(window, source, destination, {{QStringLiteral("A"), 1, {}, false, false}}, {});
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const primary = workspace->primaryPane();
    auto* const secondary = workspace->otherVisiblePane();
    primary->fileTable()->selectRow(0);
    QCOMPARE(primary->selectedEntries().size(), 1);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("clipboardCutAction"))->isEnabled());
    window.findChild<QAction*>(QStringLiteral("clipboardCutAction"))->trigger();
    QVERIFY(window.findChild<QAction*>(QStringLiteral("cancelCutAction"))->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(secondary, "activated", Qt::DirectConnection));
    QTRY_VERIFY(window.findChild<QAction*>(QStringLiteral("clipboardPasteAction"))->isEnabled());
    window.findChild<QAction*>(QStringLiteral("clipboardPasteAction"))->trigger();
    QTRY_VERIFY(QFileInfo(QDir(destination).filePath(QStringLiteral("A"))).exists());
    QTRY_VERIFY(!QFileInfo(QDir(source).filePath(QStringLiteral("A"))).exists());
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QTRY_COMPARE(table->item(0, 3)->text(), QStringLiteral("Completed"));
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("clipboardPasteAction"))->isEnabled());
}

void MainWindowTest::allowsCopyFromReadOnlyLocalDirectory()
{
    const QString source = QStringLiteral("/sys");
    if (!QFileInfo(source).isDir() || QFileInfo(source).isWritable()) {
        QSKIP("No known read-only local directory is available.");
    }
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::MainWindow window;
    showLocalSplit(window, source, temporary.path(),
                   {{QStringLiteral("kernel"), 0, {}, true, false}}, {});
    auto* const primary = window.findChild<rfm::app::PaneWorkspace*>()->primaryPane();
    primary->fileTable()->selectRow(0);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("copyAction"))->isEnabled());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("clipboardCopyAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("moveAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("clipboardCutAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("renameAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("removeAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("createDirectoryAction"))->isEnabled());
}

void MainWindowTest::aggregatesLocalCollisionSkip()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = QDir(temporary.path()).filePath(QStringLiteral("source"));
    const QString destination = QDir(temporary.path()).filePath(QStringLiteral("destination"));
    QVERIFY(QDir().mkpath(source));
    QVERIFY(QDir().mkpath(destination));
    for (const QString& name : {QStringLiteral("a"), QStringLiteral("b")}) {
        QFile file(QDir(source).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(name.toUtf8()) == name.size());
    }
    QFile existing(QDir(destination).filePath(QStringLiteral("b")));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QVERIFY(existing.write("old") == 3);
    existing.close();
    rfm::app::MainWindow window;
    showLocalSplit(
        window, source, destination,
        {{QStringLiteral("a"), 1, {}, false, false}, {QStringLiteral("b"), 1, {}, false, false}},
        {{QStringLiteral("b"), 3, {}, false, false}});
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const primary = workspace->primaryPane();
    primary->fileTable()->selectAll();
    QSignalSpy refreshes(&window, &rfm::app::MainWindow::localDirectoryRequested);
    chooseCollisionButton(QStringLiteral("Skip"));
    window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"))->trigger();
    QTRY_VERIFY(QFileInfo(QDir(destination).filePath(QStringLiteral("a"))).exists());
    const auto* const table = window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    const int row = rowForId(const_cast<QTableWidget*>(table),
                             topVisibleOperationId(const_cast<QTableWidget*>(table)));
    QTRY_COMPARE(table->item(row, 3)->text(), QStringLiteral("Completed"));
    QVERIFY(table->item(row, 4)->text().contains(QStringLiteral("1 / 2")) ||
            table->cellWidget(row, 4) != nullptr);
    QTRY_VERIFY(std::ranges::any_of(
        refreshes, [&](const auto& args) { return args.at(1).toString() == destination; }));
}

void MainWindowTest::aggregatesLocalCollisionReplace()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = QDir(temporary.path()).filePath(QStringLiteral("source"));
    const QString destination = QDir(temporary.path()).filePath(QStringLiteral("destination"));
    QVERIFY(QDir().mkpath(source));
    QVERIFY(QDir().mkpath(destination));
    for (const QString& name : {QStringLiteral("a"), QStringLiteral("b")}) {
        QFile file(QDir(source).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(name.toUtf8()) == name.size());
    }
    QFile existing(QDir(destination).filePath(QStringLiteral("b")));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QVERIFY(existing.write("old") == 3);
    existing.close();
    rfm::app::MainWindow window;
    showLocalSplit(
        window, source, destination,
        {{QStringLiteral("a"), 1, {}, false, false}, {QStringLiteral("b"), 1, {}, false, false}},
        {{QStringLiteral("b"), 3, {}, false, false}});
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    workspace->primaryPane()->fileTable()->selectAll();
    chooseCollisionButton(QStringLiteral("Replace"));
    window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"))->trigger();
    QTRY_COMPARE(QFileInfo(QDir(destination).filePath(QStringLiteral("b"))).size(), qint64{1});
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QTRY_COMPARE(table->item(0, 3)->text(), QStringLiteral("Completed"));
    QVERIFY(table->item(0, 4)->text().contains(QStringLiteral("2 / 2")) ||
            table->cellWidget(0, 4) != nullptr);
    QVERIFY(table->item(0, 1)->toolTip().contains(QDir(source).filePath(QStringLiteral("a"))));
    QVERIFY(table->item(0, 1)->toolTip().contains(QDir(source).filePath(QStringLiteral("b"))));
}

void MainWindowTest::cancelsLocalCollisionAfterPartialSuccess()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = QDir(temporary.path()).filePath(QStringLiteral("source"));
    const QString destination = QDir(temporary.path()).filePath(QStringLiteral("destination"));
    QVERIFY(QDir().mkpath(source));
    QVERIFY(QDir().mkpath(destination));
    for (const QString& name : {QStringLiteral("a"), QStringLiteral("b")}) {
        QFile file(QDir(source).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(name.toUtf8()) == name.size());
    }
    QFile existing(QDir(destination).filePath(QStringLiteral("b")));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QVERIFY(existing.write("old") == 3);
    existing.close();
    rfm::app::MainWindow window;
    showLocalSplit(
        window, source, destination,
        {{QStringLiteral("a"), 1, {}, false, false}, {QStringLiteral("b"), 1, {}, false, false}},
        {{QStringLiteral("b"), 3, {}, false, false}});
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    workspace->primaryPane()->fileTable()->selectAll();
    chooseCollisionButton(QStringLiteral("Cancel"));
    window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"))->trigger();
    QTRY_VERIFY(QFileInfo(QDir(destination).filePath(QStringLiteral("a"))).exists());
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QTRY_COMPARE(table->item(0, 3)->text(), QStringLiteral("Cancelled"));
    QCOMPARE(QFileInfo(QDir(destination).filePath(QStringLiteral("b"))).size(), qint64{3});
}

void MainWindowTest::keepsLocalAndRemoteSourcesDistinctAcrossSplitAndDisconnect()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::MainWindow window;
    setConnectionIdentity(window);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const stack = window.findChild<QStackedWidget*>(QStringLiteral("centralStack"));
    QVERIFY(workspace != nullptr);
    QVERIFY(navigation != nullptr);
    workspace->activePane()->showDirectory(QStringLiteral("/remote"),
                                           QStringLiteral("sftp://host/remote"),
                                           {{QStringLiteral("remote.txt"), 1, {}, false, false}});
    workspace->setSplit(true);
    rfm::app::FileBrowserPane* const remotePane = workspace->primaryPane();
    rfm::app::FileBrowserPane* const localPane =
        workspace->otherVisiblePane(workspace->paneId(remotePane));
    QVERIFY(localPane != nullptr);
    QVERIFY(QMetaObject::invokeMethod(localPane, "activated", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(navigation, "localLocationActivated", Qt::DirectConnection,
                                      Q_ARG(QString, temporary.path())));
    QTRY_COMPARE(localPane->source(), rfm::core::FileSource::Local);
    QCOMPARE(remotePane->source(), rfm::core::FileSource::Ssh);
    QVERIFY(remotePane->currentLocation().machineId != localPane->currentLocation().machineId);

    QVERIFY(QMetaObject::invokeMethod(remotePane, "activated", Qt::DirectConnection));
    remotePane->fileTable()->selectRow(0);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"))->isEnabled());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction"))->isEnabled());

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QVERIFY(!remotePane->hasLocation());
    QCOMPARE(localPane->source(), rfm::core::FileSource::Local);
    QCOMPARE(stack->currentWidget(), window.findChild<rfm::app::WorkspaceTabs*>());
}

void MainWindowTest::workspaceActionsResolveCurrentTab()
{
    rfm::app::MainWindow window;
    auto* const tabs = window.findChild<rfm::app::WorkspaceTabs*>();
    QVERIFY(tabs != nullptr);
    auto* const first = tabs->activeWorkspace();
    auto* const widget = tabs->findChild<QTabWidget*>();
    QVERIFY(widget != nullptr);
    // Test fixture only: the application exposes no action for adding tabs yet.
    auto* const second = new rfm::app::PaneWorkspace(widget);
    second->setSplit(true);
    widget->addTab(second, QStringLiteral("Fixture"));
    widget->setCurrentWidget(second);
    QCOMPARE(tabs->activeWorkspace(), second);
    QCOMPARE(tabs->pane(tabs->paneId(first->primaryPane())), first->primaryPane());
    QCOMPARE(tabs->paneIds().size(), 3);
    QCOMPARE(tabs->visiblePaneIds().size(), 2);
    auto* const split = window.findChild<QAction*>(QStringLiteral("splitViewAction"));
    QVERIFY(split != nullptr);
    QVERIFY(split->isChecked());
    split->trigger();
    QVERIFY(!second->isSplit());
    split->trigger();
    QVERIFY(second->isSplit());
    QVERIFY(!first->isSplit());
    auto* const secondary = second->otherVisiblePane();
    window.findChild<QAction*>(QStringLiteral("switchPaneAction"))->trigger();
    QCOMPARE(tabs->activePane(), secondary);
    QCOMPARE(first->activePane(), first->primaryPane());
    first->primaryPane()->fileTable()->horizontalHeader()->moveSection(0, 1);
    second->primaryPane()->fileTable()->horizontalHeader()->moveSection(0, 1);
    window.findChild<QAction*>(QStringLiteral("resetFileViewAction"))->trigger();
    QCOMPARE(second->primaryPane()->fileTable()->horizontalHeader()->visualIndex(0), 0);
    // FileBrowserPane deliberately shares reset preferences across all panes.
    QCOMPARE(first->primaryPane()->fileTable()->horizontalHeader()->visualIndex(0), 0);
    widget->setCurrentWidget(first);
    QVERIFY(!split->isChecked());
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("switchPaneAction"))->isEnabled());
}

void MainWindowTest::resetFileViewActionResetsAllPanes()
{
    rfm::app::MainWindow window;
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    workspace->setSplit(true);
    auto* const primary = workspace->primaryPane();
    auto* const secondary = workspace->otherVisiblePane();
    QVERIFY(primary != nullptr);
    QVERIFY(secondary != nullptr);
    for (rfm::app::FileBrowserPane* const pane : {primary, secondary}) {
        auto* const header = pane->fileTable()->horizontalHeader();
        header->moveSection(header->visualIndex(0), 3);
        pane->fileTable()->sortItems(1, Qt::AscendingOrder);
    }
    auto* const action = window.findChild<QAction*>(QStringLiteral("resetFileViewAction"));
    QVERIFY(action != nullptr);
    action->trigger();
    for (rfm::app::FileBrowserPane* const pane : {primary, secondary}) {
        const auto* const header = pane->fileTable()->horizontalHeader();
        QCOMPARE(header->sortIndicatorSection(), -1);
        QVERIFY(!header->isSortIndicatorShown());
        for (int logicalIndex = 0; logicalIndex < header->count(); ++logicalIndex) {
            QCOMPARE(header->visualIndex(logicalIndex), logicalIndex);
        }
    }
}

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
    QVERIFY(window.findChild<QAction*>(QStringLiteral("uploadAction")) == nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("downloadAction")) == nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("uploadFilesAction")) == nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("uploadFolderAction")) == nullptr);
    const auto* const refreshTimer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(refreshTimer != nullptr);
    QCOMPARE(refreshTimer->interval(), 3000);
    QVERIFY(refreshTimer->isActive());
}

void MainWindowTest::persistsShowHiddenFilesPreference()
{
    QSettings settings;
    const QString key = QStringLiteral("ui/showHiddenFiles");
    const bool hadValue = settings.contains(key);
    const QVariant previousValue = settings.value(key);
    settings.remove(key);
    settings.sync();

    {
        rfm::app::MainWindow window;
        auto* const action = window.findChild<QAction*>(QStringLiteral("showHiddenFilesAction"));
        QVERIFY(action != nullptr);
        QVERIFY(action->isCheckable());
        QVERIFY(!action->isChecked());
        auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
        auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
        QVERIFY(workspace != nullptr);
        QVERIFY(navigation != nullptr);
        rfm::app::FileBrowserPane* const pane = workspace->primaryPane();
        pane->showDirectory({rfm::core::FileSource::Local,
                             QString::fromLatin1(rfm::core::LocalMachineId), QDir::homePath()},
                            QDir::homePath(),
                            {{QStringLiteral(".hidden"), 0, {}, true, false, true}});
        navigation->setLocalDirectory(QDir::homePath(),
                                      {{QStringLiteral(".hidden"), 0, {}, true, false, true}});
        QTreeWidgetItem* const home =
            childNamed(localPlacesRoot(navigation->tree()), QStringLiteral("Home"));
        QTreeWidgetItem* const hiddenPlace = childNamed(home, QStringLiteral(".hidden"));
        QVERIFY(pane->fileTable()->isRowHidden(0));
        QVERIFY(hiddenPlace != nullptr);
        QVERIFY(hiddenPlace->isHidden());
        action->setChecked(true);
        QVERIFY(!pane->fileTable()->isRowHidden(0));
        QVERIFY(!hiddenPlace->isHidden());
    }
    QSettings{}.sync();
    {
        rfm::app::MainWindow window;
        const auto* const action =
            window.findChild<QAction*>(QStringLiteral("showHiddenFilesAction"));
        QVERIFY(action != nullptr);
        QVERIFY(action->isChecked());
    }

    if (hadValue) {
        settings.setValue(key, previousValue);
    } else {
        settings.remove(key);
    }
    settings.sync();
}

void MainWindowTest::dockVisibilityActionsTrackPanels()
{
    rfm::app::MainWindow window;
    window.show();
    QApplication::processEvents();

    auto* const placesDock = window.findChild<QDockWidget*>(QStringLiteral("placesDock"));
    auto* const operationDock = window.findChild<QDockWidget*>(QStringLiteral("operationDock"));
    auto* const placesAction = window.findChild<QAction*>(QStringLiteral("placesDockAction"));
    auto* const operationAction = window.findChild<QAction*>(QStringLiteral("operationDockAction"));
    auto* const hiddenAction = window.findChild<QAction*>(QStringLiteral("showHiddenFilesAction"));
    QVERIFY(placesDock != nullptr);
    QVERIFY(operationDock != nullptr);
    QVERIFY(placesAction != nullptr);
    QVERIFY(operationAction != nullptr);
    QVERIFY(hiddenAction != nullptr);
    QCOMPARE(placesAction->text(), QStringLiteral("Places"));
    QCOMPARE(operationAction->text(), QStringLiteral("Operations"));
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("storageHeaderLabel")) == nullptr);

    QMenu* viewMenu = nullptr;
    for (QMenu* const menu : window.findChildren<QMenu*>()) {
        if (menu->title() == QStringLiteral("&View")) {
            viewMenu = menu;
            break;
        }
    }
    QVERIFY(viewMenu != nullptr);
    QVERIFY(viewMenu->actions().contains(placesAction));
    QVERIFY(viewMenu->actions().contains(operationAction));
    QVERIFY(viewMenu->actions().contains(hiddenAction));
    QVERIFY(hiddenAction->isCheckable());

    QWidget* const placesWidget = placesDock->widget();
    QWidget* const operationWidget = operationDock->widget();
    QVERIFY(placesDock->isVisible());
    QVERIFY(operationDock->isVisible());
    QVERIFY(placesAction->isCheckable());
    QVERIFY(operationAction->isCheckable());
    QVERIFY(placesAction->isChecked());
    QVERIFY(operationAction->isChecked());

    placesDock->close();
    QVERIFY(!placesDock->isVisible());
    QVERIFY(!placesAction->isChecked());
    placesAction->trigger();
    QVERIFY(placesDock->isVisible());
    QVERIFY(placesAction->isChecked());
    QCOMPARE(placesDock->widget(), placesWidget);

    placesAction->trigger();
    QVERIFY(!placesDock->isVisible());
    QVERIFY(!placesAction->isChecked());
    placesAction->trigger();
    QVERIFY(placesDock->isVisible());
    QVERIFY(placesAction->isChecked());

    operationDock->close();
    QVERIFY(!operationDock->isVisible());
    QVERIFY(!operationAction->isChecked());
    operationAction->trigger();
    QVERIFY(operationDock->isVisible());
    QVERIFY(operationAction->isChecked());
    QCOMPARE(operationDock->widget(), operationWidget);

    operationAction->trigger();
    QVERIFY(!operationDock->isVisible());
    QVERIFY(!operationAction->isChecked());
    operationAction->trigger();
    QVERIFY(operationDock->isVisible());
    QVERIFY(operationAction->isChecked());
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
    QVERIFY(createAction != nullptr);
    QVERIFY(renameAction != nullptr);
    QVERIFY(moveAction != nullptr);
    QVERIFY(copyAction != nullptr);
    QVERIFY(removeAction != nullptr);
    QVERIFY(createAction->isEnabled());
    QVERIFY(!renameAction->isEnabled());
    QVERIFY(moveAction->isEnabled());
    QVERIFY(copyAction->isEnabled());
    QVERIFY(removeAction->isEnabled());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("uploadAction")) == nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("downloadAction")) == nullptr);
    const auto* const refreshTimer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(refreshTimer != nullptr);
    QVERIFY(refreshTimer->isActive());
}

void MainWindowTest::validatesSecureConnectionForm()
{
    rfm::app::ConnectionDialog dialog;
    auto* const host = dialog.findChild<QLineEdit*>(QStringLiteral("serverHostEdit"));
    auto* const user = dialog.findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"));
    auto* const port = dialog.findChild<QSpinBox*>(QStringLiteral("serverPortSpin"));
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
    auto* const host = dialog->findChild<QLineEdit*>(QStringLiteral("serverHostEdit"));
    auto* const user = dialog->findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"));
    auto* const buttons = dialog->findChild<QDialogButtonBox*>();
    host->setText(QStringLiteral("wrong.example.test"));
    user->setText(QStringLiteral("alice"));
    buttons->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(dialog->state(), rfm::app::ConnectionDialog::State::Connecting);

    QVERIFY(QMetaObject::invokeMethod(&window, "showConnectionError", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("Host unreachable"))));
    QCOMPARE(dialog->state(), rfm::app::ConnectionDialog::State::Error);
    QVERIFY(dialog->isVisible());
    QCOMPARE(host->text(), QStringLiteral("wrong.example.test"));
    host->setText(QStringLiteral("correct.example.test"));
    buttons->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(requested.size(), 2);

    QSignalSpy accepted(dialog, &QDialog::accepted);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QCOMPARE(accepted.size(), 1);
}

void MainWindowTest::savedServerConnectsDirectlyAndUsesCompactAuthentication()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    const rfm::core::ConnectionProfile saved{QStringLiteral("Home NAS"),
                                             QStringLiteral("nas.example.test"),
                                             QStringLiteral("alice"),
                                             2222,
                                             QStringLiteral("nas-id"),
                                             true,
                                             QStringLiteral("~/.ssh/rfm_windows_server")};
    QString error;
    QVERIFY2(store.save({saved}, &error), qPrintable(error));

    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::connectionRequested);
    auto* const list = window.findChild<QTreeWidget*>(QStringLiteral("navigationTree"));
    auto* const homeList = window.findChild<QListWidget*>(QStringLiteral("homeServerList"));
    QVERIFY(list != nullptr);
    QVERIFY(homeList != nullptr);
    QCOMPARE(serverProfileCount(list), 1);
    QCOMPARE(serverProfileItem(list, 0)->text(0), QStringLiteral("Home NAS"));
    QCOMPARE(homeList->count(), 1);
    QVERIFY(homeList->item(0)->text().startsWith(QStringLiteral("Home NAS\n")));
    homeList->setCurrentRow(0);
    auto* const connectButton = window.findChild<QPushButton*>(QStringLiteral("homeConnectButton"));
    QVERIFY(connectButton != nullptr);
    QVERIFY(connectButton->isEnabled());
    connectButton->click();

    QCOMPARE(requested.size(), 1);
    QVERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);
    const auto requestedProfile =
        qvariant_cast<rfm::core::ConnectionProfile>(requested.constFirst().constFirst());
    QCOMPARE(requestedProfile.id, QStringLiteral("nas-id"));
    QCOMPARE(requestedProfile.host, QStringLiteral("nas.example.test"));

    QVERIFY(QMetaObject::invokeMethod(&window, "showPasswordAuthentication", Qt::DirectConnection));
    auto* const authentication = window.findChild<rfm::app::PasswordAuthenticationDialog*>();
    QVERIFY(authentication != nullptr);
    QCOMPARE(
        authentication->findChild<QLabel*>(QStringLiteral("authenticationServerNameLabel"))->text(),
        QStringLiteral("Home NAS"));
    QCOMPARE(authentication->findChild<QLabel*>(QStringLiteral("authenticationServerIdentityLabel"))
                 ->text(),
             QStringLiteral("alice@nas.example.test"));
    QCOMPARE(
        authentication->findChild<QLabel*>(QStringLiteral("authenticationPromptLabel"))->text(),
        QStringLiteral("SSH key authentication failed. Enter your password to continue."));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "showPasswordAuthenticationForReason", Qt::DirectConnection,
        Q_ARG(rfm::ssh::PasswordAuthenticationReason,
              rfm::ssh::PasswordAuthenticationReason::AdditionalPasswordRequired)));
    QCOMPARE(
        authentication->findChild<QLabel*>(QStringLiteral("authenticationPromptLabel"))->text(),
        QStringLiteral("Additional password authentication is required."));
    QVERIFY(authentication->findChild<QLineEdit*>(QStringLiteral("serverHostEdit")) == nullptr);
}

void MainWindowTest::homeAndNavigationDoubleClickConnectDirectly()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    QString error;
    QVERIFY(store.save(
        {{QStringLiteral("Server"), QStringLiteral("server.example.test"), QStringLiteral("alice"),
          22, QStringLiteral("server-id"), true, QStringLiteral("~/.ssh/rfm_windows_server")}},
        &error));
    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::connectionRequested);

    auto* const homeList = window.findChild<QListWidget*>(QStringLiteral("homeServerList"));
    homeList->setCurrentRow(0);
    QVERIFY(QMetaObject::invokeMethod(homeList, "itemDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(QListWidgetItem*, homeList->item(0))));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(qvariant_cast<rfm::core::ConnectionProfile>(requested.constFirst().constFirst())
                 .privateKeyPath,
             QStringLiteral("~/.ssh/rfm_windows_server"));
    QVERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    auto* const tree = window.findChild<QTreeWidget*>(QStringLiteral("navigationTree"));
    selectServerProfile(tree, 0);
    QTreeWidgetItem* const item = serverProfileItem(tree, 0);
    QVERIFY(QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, item), Q_ARG(int, 0)));
    QCOMPARE(requested.size(), 2);
    QVERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);
}

void MainWindowTest::passwordPromptHandlesRejectionAndCancellation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    QString error;
    QVERIFY(store.save({{QStringLiteral("Server"), QStringLiteral("server.example.test"),
                         QStringLiteral("alice"), 22, QStringLiteral("server-id"), true}},
                       &error));
    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    auto* const homeList = window.findChild<QListWidget*>(QStringLiteral("homeServerList"));
    homeList->setCurrentRow(0);
    window.findChild<QPushButton*>(QStringLiteral("homeConnectButton"))->click();

    QVERIFY(QMetaObject::invokeMethod(&window, "showPasswordAuthentication", Qt::DirectConnection));
    auto* authentication = window.findChild<rfm::app::PasswordAuthenticationDialog*>();
    QVERIFY(authentication != nullptr);
    QCOMPARE(
        authentication->findChild<QLabel*>(QStringLiteral("authenticationPromptLabel"))->text(),
        QStringLiteral("SSH key or agent authentication failed. Enter your password to continue."));
    authentication->findChild<QLineEdit*>(QStringLiteral("authenticationPasswordEdit"))
        ->setText(QStringLiteral("wrong"));
    authentication->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "showPasswordAuthenticationError", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("Incorrect password. Please try again."))));
    QVERIFY(authentication->isVisible());
    QVERIFY(!authentication->isAuthenticating());
    QCOMPARE(
        authentication->findChild<QLabel*>(QStringLiteral("authenticationStatusLabel"))->text(),
        QStringLiteral("Incorrect password. Please try again."));
    QVERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);

    QSignalSpy cancelled(&window, &rfm::app::MainWindow::passwordAuthenticationCancelled);
    authentication->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(cancelled.size(), 1);
    QTRY_VERIFY(window.findChild<rfm::app::PasswordAuthenticationDialog*>() == nullptr);
}

void MainWindowTest::placesButtonTracksActiveAndSelectedProfiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    const QList profiles{rfm::core::ConnectionProfile{
                             QStringLiteral("Active server"), QStringLiteral("active.example.test"),
                             QStringLiteral("alice"), 22, QStringLiteral("active-id")},
                         rfm::core::ConnectionProfile{
                             QStringLiteral("Other server"), QStringLiteral("other.example.test"),
                             QStringLiteral("alice"), 22, QStringLiteral("other-id")}};
    QString error;
    QVERIFY2(store.save(profiles, &error), qPrintable(error));

    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    QSignalSpy connectionRequested(&window, &rfm::app::MainWindow::connectionRequested);
    auto* const list = window.findChild<QTreeWidget*>(QStringLiteral("navigationTree"));
    auto* const disconnect = window.findChild<QAction*>(QStringLiteral("disconnectAction"));
    QVERIFY(list != nullptr);
    QVERIFY(disconnect != nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("connectServerProfileButton")) ==
            nullptr);
    selectServerProfile(list, 0);
    QVERIFY(QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, serverProfileItem(list, 0)),
                                      Q_ARG(int, 0)));
    QCOMPARE(connectionRequested.size(), 1);
    QVERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));

    QVERIFY(disconnect->isEnabled());
    QVERIFY(serverProfileItem(list, 0)->text(0).contains(QStringLiteral("Connected")));
    QVERIFY(serverProfileItem(list, 0)->data(0, Qt::UserRole + 4).toBool());

    QObject::disconnect(&window, &rfm::app::MainWindow::disconnectionRequested, nullptr, nullptr);
    QSignalSpy disconnectRequested(&window, &rfm::app::MainWindow::disconnectionRequested);
    disconnect->trigger();
    QCOMPARE(disconnectRequested.size(), 1);
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QVERIFY(!serverProfileItem(list, 0)->text(0).contains(QStringLiteral("Connected")));

    selectServerProfile(list, 0);
    QVERIFY(QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, serverProfileItem(list, 0)),
                                      Q_ARG(int, 0)));
    QCOMPARE(connectionRequested.size(), 2);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    selectServerProfile(list, 1);
    QSignalSpy secondConnection(&window, &rfm::app::MainWindow::connectionRequested);
    QVERIFY(QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, serverProfileItem(list, 1)),
                                      Q_ARG(int, 0)));
    QCOMPARE(secondConnection.size(), 0);
}

void MainWindowTest::addsEditsAndRemovesSavedServers()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::MainWindow window(nullptr, {}, temporary.path());
    auto* const list = window.findChild<QTreeWidget*>(QStringLiteral("navigationTree"));
    auto* const homeList = window.findChild<QListWidget*>(QStringLiteral("homeServerList"));
    QVERIFY(list != nullptr);
    QVERIFY(homeList != nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("connectServerProfileButton")) ==
            nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("addServerProfileButton")) == nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("editServerProfileButton")) == nullptr);
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("removeServerProfileButton")) == nullptr);

    auto* const homeAdd = window.findChild<QPushButton*>(QStringLiteral("homeNewConnectionButton"));
    auto* const placesAdd =
        qobject_cast<QToolButton*>(list->itemWidget(remoteCategoryItem(list), 1));
    QVERIFY(homeAdd != nullptr);
    QVERIFY(placesAdd != nullptr);
    QCOMPARE(placesAdd->toolTip(), QStringLiteral("New connection"));
    QCOMPARE(placesAdd->accessibleName(), QStringLiteral("New connection"));

    homeAdd->click();
    auto* dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    QCOMPARE(dialog->windowTitle(), QStringLiteral("New SSH connection"));
    QVERIFY(!dialog->findChild<QCheckBox*>(QStringLiteral("saveServerCheck"))->isHidden());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject", Qt::DirectConnection));
    QTRY_VERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);

    QObject::disconnect(&window, &rfm::app::MainWindow::connectionRequested, nullptr, nullptr);
    placesAdd->click();
    dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    QCOMPARE(dialog->windowTitle(), QStringLiteral("New SSH connection"));
    auto* const saveServer = dialog->findChild<QCheckBox*>(QStringLiteral("saveServerCheck"));
    QVERIFY(saveServer != nullptr);
    QVERIFY(!saveServer->isHidden());
    dialog->findChild<QLineEdit*>(QStringLiteral("serverNameEdit"))
        ->setText(QStringLiteral("Test server"));
    dialog->findChild<QLineEdit*>(QStringLiteral("serverHostEdit"))
        ->setText(QStringLiteral("first.example.test"));
    dialog->findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"))
        ->setText(QStringLiteral("alice"));
    dialog->findChild<QSpinBox*>(QStringLiteral("serverPortSpin"))->setValue(2222);
    dialog->findChild<QRadioButton*>(QStringLiteral("passwordOnlyRadio"))->click();
    saveServer->setChecked(true);
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();

    const rfm::core::ServerProfileStore store(temporary.path());
    QString error;
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(error.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QTRY_VERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);
    QCOMPARE(serverProfileCount(list), 1);
    QCOMPARE(serverProfileItem(list, 0)->text(0), QStringLiteral("Test server"));
    QCOMPARE(homeList->count(), 1);
    QVERIFY(homeList->item(0)->text().startsWith(QStringLiteral("Test server\n")));
    const QList passwordOnlySaved = store.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(passwordOnlySaved.constFirst().authenticationMode,
             rfm::core::AuthenticationMode::PasswordOnly);
    QVERIFY(passwordOnlySaved.constFirst().privateKeyPath.isEmpty());

    auto* const edit = qobject_cast<QToolButton*>(list->itemWidget(serverProfileItem(list, 0), 1));
    QVERIFY(edit != nullptr);
    QCOMPARE(edit->toolTip(), QStringLiteral("Edit server"));
    QCOMPARE(edit->accessibleName(), QStringLiteral("Edit server"));
    QSignalSpy connectionRequested(&window, &rfm::app::MainWindow::connectionRequested);
    QTimer::singleShot(0, [] {
        auto* const dialog =
            qobject_cast<rfm::app::ServerProfileDialog*>(QApplication::activeModalWidget());
        QVERIFY(dialog != nullptr);
        dialog->findChild<QLineEdit*>(QStringLiteral("serverNameEdit"))
            ->setText(QStringLiteral("Updated server"));
        dialog->findChild<QLineEdit*>(QStringLiteral("serverHostEdit"))
            ->setText(QStringLiteral("updated.example.test"));
        dialog->findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"))
            ->setText(QStringLiteral("bob"));
        dialog->findChild<QSpinBox*>(QStringLiteral("serverPortSpin"))->setValue(2200);
        dialog->findChild<QRadioButton*>(QStringLiteral("keyOrAgentRadio"))->click();
        dialog->findChild<QLineEdit*>(QStringLiteral("privateKeyPathEdit"))
            ->setText(QStringLiteral("~/.ssh/rfm_windows_server"));
        dialog->findChild<QCheckBox*>(QStringLiteral("passwordAuthenticationCheck"))
            ->setChecked(true);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
    });
    edit->click();
    QCOMPARE(connectionRequested.size(), 0);
    QCOMPARE(serverProfileCount(list), 1);
    QCOMPARE(serverProfileItem(list, 0)->text(0), QStringLiteral("Updated server"));
    QCOMPARE(homeList->count(), 1);
    QVERIFY(homeList->item(0)->text().startsWith(QStringLiteral("Updated server\n")));
    const QList saved = store.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.constFirst().host, QStringLiteral("updated.example.test"));
    QCOMPARE(saved.constFirst().username, QStringLiteral("bob"));
    QCOMPARE(saved.constFirst().port, quint16{2200});
    QCOMPARE(saved.constFirst().privateKeyPath, QStringLiteral("~/.ssh/rfm_windows_server"));
    QCOMPARE(saved.constFirst().authenticationMode, rfm::core::AuthenticationMode::KeyOrAgent);
    QVERIFY(saved.constFirst().allowPasswordAuthentication);

    window.show();
    QTest::qWait(1);
    selectServerProfile(list, 0);
    list->scrollToItem(serverProfileItem(list, 0));
    QTimer::singleShot(0, [] {
        auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu != nullptr);
        QAction* removeAction = nullptr;
        for (QAction* action : menu->actions()) {
            if (action->text() == QStringLiteral("Remove server")) {
                removeAction = action;
                break;
            }
        }
        QVERIFY(removeAction != nullptr);
        acceptNextQuestion();
        removeAction->trigger();
    });
    const QPoint contextPosition = list->visualItemRect(serverProfileItem(list, 0)).center();
    QVERIFY(QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection,
                                      Q_ARG(QPoint, contextPosition)));
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

    const auto openManualConnection = [&window] {
        window.findChild<QAction*>(QStringLiteral("newConnectionAction"))->trigger();
        auto* const dialog = window.findChild<rfm::app::ConnectionDialog*>();
        if (dialog == nullptr) {
            return dialog;
        }
        dialog->findChild<QLineEdit*>(QStringLiteral("serverNameEdit"))
            ->setText(QStringLiteral("Saved server"));
        dialog->findChild<QLineEdit*>(QStringLiteral("serverHostEdit"))
            ->setText(QStringLiteral("saved.example.test"));
        dialog->findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"))
            ->setText(QStringLiteral("alice"));
        dialog->findChild<QLineEdit*>(QStringLiteral("privateKeyPathEdit"))
            ->setText(QStringLiteral("~/.ssh/saved_server"));
        dialog->findChild<QCheckBox*>(QStringLiteral("passwordAuthenticationCheck"))
            ->setChecked(true);
        dialog->findChild<QCheckBox*>(QStringLiteral("saveServerCheck"))->setChecked(true);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        return dialog;
    };

    rfm::app::ConnectionDialog* dialog = openManualConnection();
    QVERIFY(dialog != nullptr);
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(error.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(&window, "showConnectionError", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("Authentication failed"))));
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(error.isEmpty());

    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(QMetaObject::invokeMethod(&window, "showPasswordAuthentication", Qt::DirectConnection));
    auto* const authentication = window.findChild<rfm::app::PasswordAuthenticationDialog*>();
    QVERIFY(authentication != nullptr);
    authentication->findChild<QLineEdit*>(QStringLiteral("authenticationPasswordEdit"))
        ->setText(ephemeralSecret);
    authentication->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QList saved = store.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(saved.size(), 1);
    QVERIFY(saved.constFirst().isValidSavedProfile());
    QCOMPARE(saved.constFirst().host, QStringLiteral("saved.example.test"));
    QVERIFY(saved.constFirst().allowPasswordAuthentication);
    QCOMPARE(saved.constFirst().privateKeyPath, QStringLiteral("~/.ssh/saved_server"));
    QFile serialized(store.filePath());
    QVERIFY(serialized.open(QIODevice::ReadOnly));
    QVERIFY(!serialized.readAll().contains(ephemeralSecret.toUtf8()));

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QTRY_VERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);
    dialog = openManualConnection();
    QVERIFY(dialog != nullptr);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    saved = store.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(saved.size(), 1);

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QTRY_VERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);
    window.findChild<QAction*>(QStringLiteral("newConnectionAction"))->trigger();
    dialog = window.findChild<rfm::app::ConnectionDialog*>();
    QVERIFY(dialog != nullptr);
    dialog->findChild<QLineEdit*>(QStringLiteral("serverNameEdit"))
        ->setText(QStringLiteral("Ephemeral server"));
    dialog->findChild<QLineEdit*>(QStringLiteral("serverHostEdit"))
        ->setText(QStringLiteral("ephemeral.example.test"));
    dialog->findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"))
        ->setText(QStringLiteral("guest"));
    QVERIFY(!dialog->findChild<QCheckBox*>(QStringLiteral("saveServerCheck"))->isChecked());
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(".")),
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
        QStringLiteral("alice"),           22,
        QStringLiteral("disconnect-id"),   false};
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
    auto* const serverList = window.findChild<QTreeWidget*>(QStringLiteral("navigationTree"));
    QVERIFY(serverList != nullptr);
    selectServerProfile(serverList, 0);
    QVERIFY(QMetaObject::invokeMethod(serverList, "itemDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem*, serverProfileItem(serverList, 0)),
                                      Q_ARG(int, 0)));
    QVERIFY(window.findChild<rfm::app::ConnectionDialog*>() == nullptr);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QVERIFY(disconnect->isEnabled());
    QVERIFY(refreshTimer->isActive());
    QCOMPARE(stack->currentWidget(), window.findChild<rfm::app::WorkspaceTabs*>());

    QObject::disconnect(&window, &rfm::app::MainWindow::disconnectionRequested, nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::disconnectionRequested);
    disconnect->trigger();
    QCOMPARE(requested.size(), 1);
    QVERIFY(!disconnect->isEnabled());

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QVERIFY(!disconnect->isEnabled());
    QVERIFY(refreshTimer->isActive());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("newConnectionAction"))->isEnabled());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("uploadAction")) == nullptr);
    QVERIFY(!window.findChild<QTableWidget*>(QStringLiteral("remoteFileTable"))->isEnabled());
    QCOMPARE(window.findChild<QTableWidget*>(QStringLiteral("remoteFileTable"))->rowCount(), 0);
    QCOMPARE(stack->currentWidget(), home);
    QVERIFY(stack->currentWidget() != workspace);
    QCOMPARE(store.load(&error).size(), 1);
    QVERIFY(error.isEmpty());
}

void MainWindowTest::coalescesConnectionErrorsFromOneDisconnect()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    rfm::app::MainWindow window(nullptr, directory.path());
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));

    int simultaneousMessages = 0;
    QTimer::singleShot(0, &window, [&window, &simultaneousMessages] {
        QTimer::singleShot(0, &window, [&simultaneousMessages] {
            for (QWidget* const widget : QApplication::topLevelWidgets()) {
                if (auto* const messageBox = qobject_cast<QMessageBox*>(widget);
                    messageBox != nullptr && messageBox->isVisible()) {
                    ++simultaneousMessages;
                    messageBox->accept();
                }
            }
        });
        QVERIFY(QMetaObject::invokeMethod(
            &window, "showConnectionError", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral("No active SFTP connection."))));
    });
    QVERIFY(QMetaObject::invokeMethod(
        &window, "showConnectionError", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("The SSH/SFTP connection was lost during a transfer."))));
    QCOMPARE(simultaneousMessages, 1);

    QCoreApplication::processEvents();
    bool laterErrorShown = false;
    QTimer::singleShot(0, &window, [&laterErrorShown] {
        auto* const messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        laterErrorShown = messageBox != nullptr && messageBox->isVisible() &&
                          messageBox->text() == QStringLiteral("No active SFTP connection.");
        if (messageBox != nullptr) {
            messageBox->accept();
        }
    });
    QVERIFY(
        QMetaObject::invokeMethod(&window, "showConnectionError", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("No active SFTP connection."))));
    QVERIFY(laterErrorShown);
}

void MainWindowTest::terminalTransferReleasesDisconnectProtection()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    rfm::app::MainWindow window(nullptr, directory.path());
    auto* const disconnect = window.findChild<QAction*>(QStringLiteral("disconnectAction"));
    QVERIFY(disconnect != nullptr);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleConnected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(".")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QVERIFY(disconnect->isEnabled());

    const auto running = progress(61, rfm::core::TransferState::Transferring, 1, 10);
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, running)));
    QVERIFY(!disconnect->isEnabled());

    auto failed = running;
    failed.state = rfm::core::TransferState::Failed;
    failed.error = QStringLiteral("connection lost");
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, failed)));
    QVERIFY(disconnect->isEnabled());
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
    QCOMPARE(table->horizontalHeaderItem(6)->text(), QStringLiteral("Error"));
    QCOMPARE(table->horizontalHeaderItem(7)->text(), QStringLiteral("Actions"));
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

void MainWindowTest::ordersOperationRowsWithoutDuplicates()
{
    rfm::app::OperationPanel panel;
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);

    auto oldTerminal =
        rfm::core::operationProgress(progress(1001, rfm::core::TransferState::Completed, 10, 10));
    oldTerminal.finishedAt = QDateTime::fromSecsSinceEpoch(10);
    panel.updateOperation(oldTerminal);
    QCOMPARE(operationIds(table), QList<quint64>{1001});

    panel.updateOperation(
        rfm::core::operationProgress(progress(1002, rfm::core::TransferState::Queued)));
    panel.updateOperation(
        rfm::core::operationProgress(progress(1003, rfm::core::TransferState::Queued)));
    QCOMPARE(operationIds(table), (QList<quint64>{1002, 1003, 1001}));

    panel.updateOperation(rfm::core::operationProgress(
        progress(1003, rfm::core::TransferState::Transferring, 1, 10)));
    QCOMPARE(operationIds(table), (QList<quint64>{1003, 1002, 1001}));
    panel.updateOperation(rfm::core::operationProgress(
        progress(1003, rfm::core::TransferState::Transferring, 5, 10)));
    QCOMPARE(table->rowCount(), 3);
    QCOMPARE(operationIds(table), (QList<quint64>{1003, 1002, 1001}));

    auto completed =
        rfm::core::operationProgress(progress(1003, rfm::core::TransferState::Completed, 10, 10));
    completed.finishedAt = QDateTime::fromSecsSinceEpoch(30);
    panel.updateOperation(completed);
    QCOMPARE(table->rowCount(), 3);
    QCOMPARE(operationIds(table), (QList<quint64>{1002, 1003, 1001}));

    auto middleTerminal =
        rfm::core::operationProgress(progress(1004, rfm::core::TransferState::Failed, 4, 10));
    middleTerminal.finishedAt = QDateTime::fromSecsSinceEpoch(20);
    panel.updateOperation(middleTerminal);
    QCOMPARE(operationIds(table), (QList<quint64>{1002, 1003, 1004, 1001}));

    panel.updateOperation(
        rfm::core::operationProgress(progress(1005, rfm::core::TransferState::Queued)));
    QCOMPARE(operationIds(table), (QList<quint64>{1002, 1005, 1003, 1004, 1001}));

    panel.updateOperation(
        rfm::core::operationProgress(progress(1006, rfm::core::TransferState::Cancelled)));
    panel.updateOperation(
        rfm::core::operationProgress(progress(1007, rfm::core::TransferState::Cancelled)));
    QVERIFY(rowForId(table, 1007) < rowForId(table, 1006));
}

void MainWindowTest::restoresOperationHistoryInSingleRebuild()
{
    rfm::app::OperationPanel panel;
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);

    auto oldest =
        rfm::core::operationProgress(progress(1101, rfm::core::TransferState::Completed, 10, 10));
    oldest.finishedAt = QDateTime::fromSecsSinceEpoch(10);
    auto newest =
        rfm::core::operationProgress(progress(1102, rfm::core::TransferState::Failed, 4, 10));
    newest.finishedAt = QDateTime::fromSecsSinceEpoch(30);
    auto middle =
        rfm::core::operationProgress(progress(1103, rfm::core::TransferState::Cancelled, 0, 10));
    middle.finishedAt = QDateTime::fromSecsSinceEpoch(20);

    QSignalSpy rowsInserted(table->model(), &QAbstractItemModel::rowsInserted);
    panel.restoreOperations({oldest, newest, middle});

    QCOMPARE(rowsInserted.size(), 3);
    QCOMPARE(operationIds(table), (QList<quint64>{1102, 1103, 1101}));
}

void MainWindowTest::keepsOperationRowsSingleLineAndRetainsExplicitCurrentItem()
{
    rfm::app::OperationPanel panel;
    panel.resize(900, 240);
    panel.show();
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);

    auto longDestination =
        rfm::core::operationProgress(progress(1201, rfm::core::TransferState::Transferring, 1, 10));
    longDestination.destination = QStringLiteral("/mnt/hdd2/Films Science Fiction");
    auto shortDestination =
        rfm::core::operationProgress(progress(1202, rfm::core::TransferState::Transferring, 1, 10));
    shortDestination.destination = QStringLiteral("/srv");
    auto currentItem =
        rfm::core::operationProgress(progress(1203, rfm::core::TransferState::Transferring, 1, 10));
    currentItem.currentItem = QStringLiteral("/tmp/archive/current-item.mkv");
    panel.restoreOperations({longDestination, shortDestination, currentItem});
    QCoreApplication::processEvents();

    const int longRow = rowForId(table, 1201);
    const int shortRow = rowForId(table, 1202);
    const int currentRow = rowForId(table, 1203);
    QVERIFY(longRow >= 0);
    QVERIFY(shortRow >= 0);
    QVERIFY(currentRow >= 0);
    QVERIFY(!table->wordWrap());
    QCOMPARE(table->item(longRow, 2)->text(), longDestination.destination);
    QCOMPARE(table->item(longRow, 2)->toolTip(), longDestination.destination);
    QCOMPARE(table->rowHeight(longRow), table->rowHeight(shortRow));
    QVERIFY(
        table->item(currentRow, 1)->text().contains(QStringLiteral("\nCurrent: current-item.mkv")));
    QVERIFY(table->rowHeight(currentRow) > table->rowHeight(shortRow));
    QVERIFY(table->cellWidget(longRow, 7) != nullptr);
}

void MainWindowTest::preservesOperationPanelContextAcrossReordering()
{
    rfm::app::OperationPanel panel;
    panel.resize(900, 220);
    panel.show();
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);

    for (quint64 id = 2000; id < 2030; ++id) {
        auto terminal =
            rfm::core::operationProgress(progress(id, rfm::core::TransferState::Completed, 10, 10));
        terminal.finishedAt = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(id));
        panel.updateOperation(terminal);
    }
    panel.updateOperation(rfm::core::operationProgress(
        progress(3001, rfm::core::TransferState::Transferring, 1, 10)));
    QCoreApplication::processEvents();

    table->selectRow(rowForId(table, 2010));
    table->scrollToItem(table->item(rowForId(table, 2010), 0), QAbstractItemView::PositionAtTop);
    QCoreApplication::processEvents();
    const quint64 anchorId = topVisibleOperationId(table);
    QVERIFY(anchorId != 0);
    QVERIFY(table->verticalScrollBar()->value() > table->verticalScrollBar()->minimum());

    panel.updateOperation(
        rfm::core::operationProgress(progress(3002, rfm::core::TransferState::Queued)));
    QCoreApplication::processEvents();
    QCOMPARE(topVisibleOperationId(table), anchorId);
    QCOMPARE(table->item(table->selectionModel()->selectedRows().constFirst().row(), 0)
                 ->data(Qt::UserRole)
                 .toULongLong(),
             quint64{2010});
    QVERIFY(table->verticalScrollBar()->value() > table->verticalScrollBar()->minimum());

    QSignalSpy pauses(&panel, &rfm::app::OperationPanel::pauseRequested);
    QSignalSpy resumes(&panel, &rfm::app::OperationPanel::resumeRequested);
    QSignalSpy cancellations(&panel, &rfm::app::OperationPanel::cancelRequested);
    QWidget* actions = table->cellWidget(rowForId(table, 3001), 7);
    QVERIFY(actions != nullptr);
    auto* pauseResume = actions->findChild<QToolButton*>(QStringLiteral("pauseResumeButton"));
    auto* cancel = actions->findChild<QToolButton*>(QStringLiteral("cancelTransferButton"));
    QVERIFY(pauseResume != nullptr);
    QVERIFY(cancel != nullptr);
    QTest::mouseClick(pauseResume, Qt::LeftButton);
    QCOMPARE(pauses.size(), 1);
    QCOMPARE(pauses.constFirst().constFirst().toULongLong(), quint64{3001});

    panel.updateOperation(
        rfm::core::operationProgress(progress(3001, rfm::core::TransferState::Paused, 1, 10)));
    actions = table->cellWidget(rowForId(table, 3001), 7);
    pauseResume = actions->findChild<QToolButton*>(QStringLiteral("pauseResumeButton"));
    cancel = actions->findChild<QToolButton*>(QStringLiteral("cancelTransferButton"));
    QTest::mouseClick(pauseResume, Qt::LeftButton);
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(resumes.size(), 1);
    QCOMPARE(resumes.constFirst().constFirst().toULongLong(), quint64{3001});
    QCOMPARE(cancellations.size(), 1);
    QCOMPARE(cancellations.constFirst().constFirst().toULongLong(), quint64{3001});

    QSignalSpy removals(&panel, &rfm::app::OperationPanel::removeTerminalRequested);
    auto* const remove = table->cellWidget(rowForId(table, 2010), 7)
                             ->findChild<QToolButton*>(QStringLiteral("removeOperationButton"));
    QVERIFY(remove != nullptr);
    QTest::mouseClick(remove, Qt::LeftButton);
    QCOMPARE(removals.size(), 1);
    QCOMPARE(removals.constFirst().constFirst().toULongLong(), quint64{2010});
}

void MainWindowTest::displaysDirectoryTransferDetails()
{
    rfm::app::OperationPanel panel;
    auto directory = progress(1101, rfm::core::TransferState::Transferring, 512, 2048, 1024);
    directory.source = QStringLiteral("/tmp/photos");
    directory.destination = QStringLiteral("/srv/photos");
    directory.directory = true;
    directory.completedFiles = 2;
    directory.totalFiles = 5;
    directory.currentItem = QStringLiteral("/tmp/photos/album/picture.jpg");
    panel.updateOperation(rfm::core::operationProgress(directory));

    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    const int row = rowForId(table, 1101);
    QVERIFY(row >= 0);
    QVERIFY(table->item(row, 1)->text().contains(QStringLiteral("Current: picture.jpg")));
    QVERIFY(table->item(row, 1)->toolTip().contains(directory.currentItem));
    auto* const bar = qobject_cast<QProgressBar*>(table->cellWidget(row, 4));
    QVERIFY(bar != nullptr);
    QCOMPARE(bar->format(), QStringLiteral("512 B / 2.0 KiB · 2 / 5 files"));
    QCOMPARE(table->item(row, 5)->text(), QStringLiteral("1.0 KiB/s"));
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
    auto copy = rfm::core::beginRemoteOperation(701, rfm::core::OperationKind::RemoteCopy,
                                                copySources, QStringLiteral("/destination"));
    copy.serverHost = QStringLiteral("files.example.test");
    copy.serverPort = 2222;
    panel.updateOperation(copy);
    const int copyRow = rowForId(table, 701);
    QVERIFY(copyRow >= 0);
    QCOMPARE(table->item(copyRow, 0)->text(), QStringLiteral("Remote Copy"));
    QCOMPARE(table->item(copyRow, 0)->toolTip(), QStringLiteral("Server: files.example.test:2222"));
    QCOMPARE(table->item(copyRow, 1)->text(), QStringLiteral("first.txt (+1)"));
    QCOMPARE(table->item(copyRow, 2)->text(), QStringLiteral("/destination"));
    QCOMPARE(table->item(copyRow, 3)->text(), QStringLiteral("Running"));
    QCOMPARE(table->item(copyRow, 5)->text(), QStringLiteral("—"));
    auto* const indeterminate = qobject_cast<QProgressBar*>(table->cellWidget(copyRow, 4));
    QVERIFY(indeterminate != nullptr);
    QCOMPARE(indeterminate->minimum(), 0);
    QCOMPARE(indeterminate->maximum(), 0);
    auto* const copyActions = table->cellWidget(copyRow, 7);
    QVERIFY(copyActions != nullptr);
    auto* const cancelCopy =
        copyActions->findChild<QToolButton*>(QStringLiteral("cancelTransferButton"));
    QVERIFY(cancelCopy != nullptr);
    QVERIFY(cancelCopy->isVisible());

    const rfm::core::RemoteOperationResult completedCopy{
        701,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/first.txt"), QStringLiteral("/destination/first.txt"), true, {}},
         {QStringLiteral("/source/second.txt"),
          QStringLiteral("/destination/second.txt"),
          true,
          {}}}};
    panel.updateOperation(rfm::core::finishRemoteOperation(completedCopy, copy));
    QCOMPARE(table->item(copyRow, 3)->text(), QStringLiteral("Completed"));
    QCOMPARE(table->item(copyRow, 4)->text(), QStringLiteral("2 / 2 completed"));

    const QList<rfm::core::RemoteSelection> moveSources{{QStringLiteral("/source/a.txt"), false},
                                                        {QStringLiteral("/source/b.txt"), false}};
    const auto move = rfm::core::beginRemoteOperation(702, rfm::core::OperationKind::RemoteMove,
                                                      moveSources, QStringLiteral("/archive"));
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
    QVERIFY(table->item(moveRow, 6)->text().contains(QStringLiteral("permission denied")));
    QVERIFY(table->cellWidget(moveRow, 4) == nullptr);
    auto* const removeMove = table->cellWidget(moveRow, 7);
    QVERIFY(removeMove != nullptr);
    QVERIFY(removeMove->findChild<QToolButton*>(QStringLiteral("removeOperationButton")) !=
            nullptr);
}

void MainWindowTest::acceptsRemoteMoveProgressFromWorker()
{
    rfm::app::MainWindow window;
    const auto move = rfm::core::beginRemoteOperation(703, rfm::core::OperationKind::RemoteMove,
                                                      {{QStringLiteral("/source/file.txt"), false}},
                                                      QStringLiteral("/archive"));

    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteOperationProgress",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::OperationProgress, move)));
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    const int row = rowForId(table, 703);
    QVERIFY(row >= 0);
    QCOMPARE(table->item(row, 0)->text(), QStringLiteral("Remote Move"));
    QCOMPARE(table->item(row, 3)->text(), QStringLiteral("Running"));
}

void MainWindowTest::removesOnlyTerminalOperationsFromPanel()
{
    rfm::app::OperationPanel panel;
    panel.show();
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    QSignalSpy removals(&panel, &rfm::app::OperationPanel::removeTerminalRequested);

    panel.updateOperation(rfm::core::beginRemoteOperation(
        801, rfm::core::OperationKind::RemoteCopy, {{QStringLiteral("/source/active"), false}},
        QStringLiteral("/destination")));
    auto completed = progress(802, rfm::core::TransferState::Completed, 10, 10);
    panel.updateOperation(rfm::core::operationProgress(completed));
    auto failed = progress(803, rfm::core::TransferState::Failed, 4, 10);
    failed.error = QStringLiteral("failure");
    panel.updateOperation(rfm::core::operationProgress(failed));
    QCOMPARE(table->rowCount(), 3);

    QVERIFY(!panel.removeTerminalOperation(801));
    QCOMPARE(table->rowCount(), 3);

    QVERIFY(table->cellWidget(rowForId(table, 801), 7) != nullptr);
    auto* const remove = table->cellWidget(rowForId(table, 802), 7)
                             ->findChild<QToolButton*>(QStringLiteral("removeOperationButton"));
    QVERIFY(remove != nullptr);
    QTest::mouseClick(remove, Qt::LeftButton);
    QCOMPARE(removals.size(), 1);
    QCOMPARE(removals.constFirst().constFirst().toULongLong(), quint64{802});
    QVERIFY(panel.removeTerminalOperation(802));
    QCOMPARE(table->rowCount(), 2);

    panel.clearTerminalOperations();
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(rowForId(table, 801), 0);
}

void MainWindowTest::exposesPauseResumeAndCancelIntentions()
{
    rfm::app::OperationPanel panel;
    panel.show();
    QSignalSpy pauses(&panel, &rfm::app::OperationPanel::pauseRequested);
    QSignalSpy resumes(&panel, &rfm::app::OperationPanel::resumeRequested);
    QSignalSpy cancellations(&panel, &rfm::app::OperationPanel::cancelRequested);
    panel.updateOperation(
        rfm::core::operationProgress(progress(301, rfm::core::TransferState::Transferring, 1, 10)));
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    const int row = rowForId(table, 301);
    QWidget* const actions = table->cellWidget(row, 7);
    auto* const pauseResume = actions->findChild<QToolButton*>(QStringLiteral("pauseResumeButton"));
    auto* const cancel = actions->findChild<QToolButton*>(QStringLiteral("cancelTransferButton"));
    QVERIFY(pauseResume != nullptr);
    QVERIFY(cancel != nullptr);
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(1), QHeaderView::Stretch);
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(2), QHeaderView::Stretch);
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(7), QHeaderView::ResizeToContents);
    QCOMPARE(table->horizontalHeaderItem(6)->text(), QStringLiteral("Error"));
    QCOMPARE(table->horizontalHeaderItem(7)->text(), QStringLiteral("Actions"));
    QCOMPARE(pauseResume->text(), QString{});
    QCOMPARE(cancel->text(), QString{});
    QVERIFY(!pauseResume->icon().isNull());
    QVERIFY(!cancel->icon().isNull());
    QCOMPARE(pauseResume->toolTip(), QStringLiteral("Pause operation"));
    QCOMPARE(cancel->toolTip(), QStringLiteral("Cancel operation"));
    QCOMPARE(pauseResume->accessibleName(), pauseResume->toolTip());
    QCOMPARE(cancel->accessibleName(), cancel->toolTip());
    QCOMPARE(pauseResume->size(), QSize(28, 28));
    QCOMPARE(cancel->size(), QSize(28, 28));
    for (const int width : {1000, 760, 520}) {
        panel.resize(width, 240);
        QCoreApplication::processEvents();
        QVERIFY(table->columnWidth(7) >= actions->minimumWidth());
        QCOMPARE(pauseResume->size(), QSize(28, 28));
        QCOMPARE(cancel->size(), QSize(28, 28));
    }
    QTest::mouseClick(pauseResume, Qt::LeftButton);
    QCOMPARE(pauses.size(), 1);

    panel.updateOperation(
        rfm::core::operationProgress(progress(301, rfm::core::TransferState::Paused, 1, 10)));
    QCOMPARE(pauseResume->text(), QString{});
    QVERIFY(!pauseResume->icon().isNull());
    QCOMPARE(pauseResume->toolTip(), QStringLiteral("Resume operation"));
    QCOMPARE(pauseResume->accessibleName(), pauseResume->toolTip());
    QTest::mouseClick(pauseResume, Qt::LeftButton);
    QCOMPARE(resumes.size(), 1);
    panel.updateOperation(
        rfm::core::operationProgress(progress(301, rfm::core::TransferState::Transferring, 2, 10)));
    QCOMPARE(pauseResume->toolTip(), QStringLiteral("Pause operation"));
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(cancellations.size(), 1);
    QCOMPARE(cancellations.constFirst().constFirst().toULongLong(), quint64{301});
    QCOMPARE(cancel->text(), QString{});
}

void MainWindowTest::displaysTerminalTransferStatesAndErrors()
{
    rfm::app::OperationPanel panel;
    panel.show();
    panel.updateOperation(
        rfm::core::operationProgress(progress(401, rfm::core::TransferState::Completed, 10, 10)));
    auto failure = progress(402, rfm::core::TransferState::Failed, 4, 10);
    failure.error = QStringLiteral("/srv/archive: permission denied");
    panel.updateOperation(rfm::core::operationProgress(failure));
    panel.updateOperation(
        rfm::core::operationProgress(progress(403, rfm::core::TransferState::Cancelled, 4, 10)));
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    const int completedRow = rowForId(table, 401);
    const int failedRow = rowForId(table, 402);
    const int cancelledRow = rowForId(table, 403);
    QCOMPARE(table->item(completedRow, 3)->text(), QStringLiteral("Completed"));
    QCOMPARE(table->item(failedRow, 3)->text(), QStringLiteral("Failed"));
    QCOMPARE(table->item(failedRow, 6)->text(), failure.error);
    auto* const completedActions = table->cellWidget(completedRow, 7);
    QVERIFY(completedActions != nullptr);
    auto* const remove =
        completedActions->findChild<QToolButton*>(QStringLiteral("removeOperationButton"));
    QVERIFY(remove != nullptr);
    QCOMPARE(remove->text(), QString{});
    QVERIFY(!remove->icon().isNull());
    QCOMPARE(remove->toolTip(), QStringLiteral("Remove from history"));
    QCOMPARE(remove->accessibleName(), remove->toolTip());
    QCOMPARE(remove->size(), QSize(28, 28));
    QVERIFY(remove->isVisible());
    QVERIFY(
        completedActions->findChild<QToolButton*>(QStringLiteral("pauseResumeButton"))->isHidden());
    QVERIFY(completedActions->findChild<QToolButton*>(QStringLiteral("cancelTransferButton"))
                ->isHidden());
    QVERIFY(table->cellWidget(failedRow, 7) != nullptr);
    QVERIFY(table->cellWidget(cancelledRow, 7) != nullptr);
}

void MainWindowTest::formatsTransferSizesAndSpeeds()
{
    QCOMPARE(rfm::app::OperationPanel::formatBytes(0), QStringLiteral("0 B"));
    QCOMPARE(rfm::app::OperationPanel::formatBytes(1536), QStringLiteral("1.5 KiB"));
    QCOMPARE(rfm::app::OperationPanel::formatSpeed(0), QStringLiteral("—"));
    QCOMPARE(rfm::app::OperationPanel::formatSpeed(1024), QStringLiteral("1.0 KiB/s"));
}

void MainWindowTest::displaysSpeedOnlyForRunningTransfers()
{
    rfm::app::OperationPanel panel;
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);

    const auto updateAndVerifySpeed = [&panel, table](quint64 id, rfm::core::TransferState state,
                                                      const QString& expectedSpeed) {
        panel.updateOperation(rfm::core::operationProgress(progress(id, state, 512, 1024, 1024)));
        const int row = rowForId(table, id);
        QVERIFY(row >= 0);
        QCOMPARE(table->item(row, 5)->text(), expectedSpeed);
    };

    updateAndVerifySpeed(501, rfm::core::TransferState::Transferring, QStringLiteral("1.0 KiB/s"));
    updateAndVerifySpeed(502, rfm::core::TransferState::Queued, QStringLiteral("—"));
    updateAndVerifySpeed(503, rfm::core::TransferState::Cancelled, QStringLiteral("—"));
    updateAndVerifySpeed(504, rfm::core::TransferState::Failed, QStringLiteral("—"));
    updateAndVerifySpeed(505, rfm::core::TransferState::Completed, QStringLiteral("—"));
}

void MainWindowTest::displaysQueuedOperationUntilWorkerStarts()
{
    rfm::app::OperationPanel panel;
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);

    rfm::core::OperationProgress operationA;
    operationA.id = 601;
    operationA.kind = rfm::core::OperationKind::LocalCopy;
    operationA.state = rfm::core::OperationState::Running;
    operationA.totalItems = 1;
    rfm::core::OperationProgress operationB = operationA;
    operationB.id = 602;
    operationB.kind = rfm::core::OperationKind::LocalMove;
    operationB.state = rfm::core::OperationState::Queued;
    panel.updateOperation(operationA);
    panel.updateOperation(operationB);
    QCOMPARE(table->item(rowForId(table, 602), 3)->text(), QStringLiteral("Queued"));

    operationB.state = rfm::core::OperationState::Running;
    panel.updateOperation(operationB);
    QCOMPARE(table->item(rowForId(table, 602), 3)->text(), QStringLiteral("Running"));
}

void MainWindowTest::displaysLocalSuccessWarning()
{
    rfm::app::OperationPanel panel;
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    rfm::core::OperationProgress operation;
    operation.id = 603;
    operation.kind = rfm::core::OperationKind::LocalCopy;
    operation.state = rfm::core::OperationState::Completed;
    operation.error = QStringLiteral("The previous destination remains in a temporary backup.");
    panel.updateOperation(operation);
    const int row = rowForId(table, operation.id);
    QCOMPARE(table->item(row, 3)->text(), QStringLiteral("Completed"));
    QCOMPARE(table->item(row, 6)->text(), operation.error);
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

    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().at(1).toString(), QStringLiteral("/srv"));
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);

    const quint64 requestId = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, requestId),
        Q_ARG(QString, QStringLiteral("/srv")), Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
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
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const debounce = window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"));
    QVERIFY(debounce != nullptr);

    auto upload = progress(501, rfm::core::TransferState::Completed, 10, 10);
    upload.destination = QStringLiteral("/srv/uploaded.txt");
    upload.direction = rfm::core::TransferDirection::Upload;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, upload)));
    QVERIFY(!debounce->isActive());
    QCOMPARE(requested.size(), 0);

    auto download = progress(502, rfm::core::TransferState::Completed, 10, 10);
    download.direction = rfm::core::TransferDirection::Download;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, download)));
    QVERIFY(!debounce->isActive());
    QCOMPARE(requested.size(), 0);
}

void MainWindowTest::manualRefreshShowsStatusMessage()
{
    rfm::app::MainWindow window;
    auto* const pane = window.findChild<rfm::app::FileBrowserPane*>();
    QVERIFY(pane != nullptr);
    pane->showDirectory({rfm::core::FileSource::Local,
                         QString::fromLatin1(rfm::core::LocalMachineId), QStringLiteral("/tmp")},
                        QStringLiteral("/tmp"), {});
    QObject::disconnect(&window, &rfm::app::MainWindow::localDirectoryRequested, nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::localDirectoryRequested);
    window.statusBar()->clearMessage();
    window.findChild<QAction*>(QStringLiteral("refreshAction"))->trigger();
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Refreshing /tmp…"));
    QCOMPARE(requested.size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().isEmpty(), 4000);
}

void MainWindowTest::localAndRemoteNavigationShowOpeningStatusMessage()
{
    rfm::app::MainWindow localWindow;
    auto* const localPane = localWindow.findChild<rfm::app::FileBrowserPane*>();
    QVERIFY(localPane != nullptr);
    QObject::disconnect(&localWindow, &rfm::app::MainWindow::localDirectoryRequested, nullptr,
                        nullptr);
    QSignalSpy localRequested(&localWindow, &rfm::app::MainWindow::localDirectoryRequested);
    localPane->locationNavigationRequested({rfm::core::FileSource::Local,
                                            QString::fromLatin1(rfm::core::LocalMachineId),
                                            QStringLiteral("/tmp")},
                                           rfm::app::PaneNavigation::Normal);
    QCOMPARE(localWindow.statusBar()->currentMessage(), QStringLiteral("Opening folder /tmp…"));
    QCOMPARE(localRequested.size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(localWindow.statusBar()->currentMessage().isEmpty(), 4000);

    rfm::app::MainWindow remoteWindow;
    QVERIFY(QMetaObject::invokeMethod(&remoteWindow, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&remoteWindow, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy remoteRequested(&remoteWindow, &rfm::app::MainWindow::directoryRequested);
    auto* const remotePane = remoteWindow.findChild<rfm::app::FileBrowserPane*>();
    QVERIFY(remotePane != nullptr);
    const rfm::core::BrowserLocation remoteLocation = remotePane->currentLocation();
    remotePane->locationNavigationRequested(
        {rfm::core::FileSource::Ssh, remoteLocation.machineId, QStringLiteral("/srv/next")},
        rfm::app::PaneNavigation::Normal);
    QCOMPARE(remoteWindow.statusBar()->currentMessage(),
             QStringLiteral("Opening folder /srv/next…"));
    QVERIFY(!remoteWindow.statusBar()->currentMessage().contains(QStringLiteral("Refreshing")));
    QCOMPARE(remoteRequested.size(), 1);
}

void MainWindowTest::crossSourceMoveUnsupportedReplacesRefreshMessage()
{
    rfm::app::MainWindow window;
    auto* const pane = window.findChild<rfm::app::FileBrowserPane*>();
    QVERIFY(pane != nullptr);
    window.statusBar()->showMessage(QStringLiteral("Refreshing /previous…"));
    QVERIFY(QMetaObject::invokeMethod(pane, "crossSourceMoveUnsupported", Qt::DirectConnection));
    QCOMPARE(window.statusBar()->currentMessage(),
             QStringLiteral("Moving between local and SSH locations is not supported yet."));
}

void MainWindowTest::appliesOnlyExpectedDirectoryResult()
{
    rfm::app::MainWindow window;
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const timer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    const quint64 expectedId = requested.constFirst().constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> wrongEntries{
        {QStringLiteral("wrong.txt"), 1, {}, false, false}};
    const QList<rfm::core::RemoteEntry> expectedEntries{
        {QStringLiteral("expected.txt"), 1, {}, false, false}};

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, expectedId + 100),
                                      Q_ARG(QString, QStringLiteral("/wrong")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, wrongEntries)));
    auto* const pane = window.findChild<rfm::app::FileBrowserPane*>();
    QCOMPARE(pane->currentPath(), QStringLiteral("/srv"));

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, expectedId),
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
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
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
        Q_ARG(QString, QStringLiteral("/srv")), Q_ARG(QList<rfm::core::RemoteEntry>, oldEntries)));
    QCOMPARE(requested.size(), 2);
    const quint64 secondId = requested.at(1).constFirst().toULongLong();
    QVERIFY(firstId != secondId);
    QCOMPARE(requested.at(0).at(1).toString(), requested.at(1).at(1).toString());
    QCOMPARE(pane->fileTable()->rowCount(), 0);

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, secondId),
        Q_ARG(QString, QStringLiteral("/srv")), Q_ARG(QList<rfm::core::RemoteEntry>, newEntries)));
    QCOMPARE(pane->fileTable()->item(0, 0)->text(), QStringLiteral("new.txt"));
}

void MainWindowTest::newNavigationMakesActiveResultObsolete()
{
    rfm::app::MainWindow window;
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const timer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    auto* const pane = window.findChild<rfm::app::FileBrowserPane*>();
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    const quint64 oldId = requested.constFirst().constFirst().toULongLong();
    pane->navigateTo(QStringLiteral("/other"));

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, oldId), Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QCOMPARE(pane->currentPath(), QStringLiteral("/srv"));
    QCOMPARE(requested.size(), 2);
    QCOMPARE(requested.at(1).at(1).toString(), QStringLiteral("/other"));
}

void MainWindowTest::handlesOnlyExpectedListingErrorWithoutDisconnecting()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{{QStringLiteral("kept.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy disconnected(&window, &rfm::app::MainWindow::disconnectionRequested);
    auto* const timer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    auto* const pane = window.findChild<rfm::app::FileBrowserPane*>();
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    const quint64 requestId = requested.constFirst().constFirst().toULongLong();

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListingError", Qt::DirectConnection, Q_ARG(quint64, requestId + 1),
        Q_ARG(QString, QStringLiteral("/wrong")), Q_ARG(QString, QStringLiteral("ignored"))));
    QVERIFY(!window.statusBar()->currentMessage().contains(QStringLiteral("ignored")));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListingError", Qt::DirectConnection,
                                      Q_ARG(quint64, requestId),
                                      Q_ARG(QString, QStringLiteral("/root")),
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
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
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
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListingError", Qt::DirectConnection,
                                      Q_ARG(quint64, requestId),
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
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
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
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, initialSecondaryRequest),
                                      Q_ARG(QString, QStringLiteral("/srv")),
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
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, obsoleteSecondaryId),
                                      Q_ARG(QString, QStringLiteral("/same")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, obsoleteEntries)));
    QCOMPARE(requested.size(), 2);
    QCOMPARE(requested.at(1).at(1).toString(), QStringLiteral("/same"));
    QCOMPARE(secondary->currentPath(), QStringLiteral("/srv"));

    const quint64 primaryId = requested.at(1).constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> primaryEntries{
        {QStringLiteral("primary-new.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, primaryId),
                                      Q_ARG(QString, QStringLiteral("/same")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, primaryEntries)));
    QCOMPARE(requested.size(), 3);
    QCOMPARE(requested.at(2).at(1).toString(), QStringLiteral("/latest"));
    QCOMPARE(primary->currentPath(), QStringLiteral("/same"));
    QCOMPARE(secondary->currentPath(), QStringLiteral("/srv"));

    const quint64 secondaryId = requested.at(2).constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> latestEntries{
        {QStringLiteral("secondary-new.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, secondaryId),
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
    waitForInitialLocalStorageRefresh(window);
    QObject::disconnect(&window, &rfm::app::MainWindow::localVolumesRequested, nullptr, nullptr);
    window.show();
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/one")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy localStorageRequests(&window, &rfm::app::MainWindow::localVolumesRequested);
    QSignalSpy remoteStorageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
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
    QCOMPARE(localStorageRequests.size(), 1);
    QCOMPARE(remoteStorageRequests.size(), 1);
    const quint64 refreshId = listings.constFirst().constFirst().toULongLong();
    const QList<rfm::core::RemoteEntry> refreshedEntries{
        {QStringLiteral("selected.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, refreshId),
                                      Q_ARG(QString, QStringLiteral("/two/child")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, refreshedEntries)));

    listings.clear();
    window.findChild<QAction*>(QStringLiteral("upAction"))->trigger();
    QCOMPARE(listings.size(), 1);
    QCOMPARE(listings.constFirst().at(1).toString(), QStringLiteral("/two"));
    const quint64 parentId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, parentId),
                                      Q_ARG(QString, QStringLiteral("/two")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, refreshedEntries)));
}

void MainWindowTest::navigationKeepsInitiatingPaneActive_data()
{
    QTest::addColumn<bool>("useSecondaryPane");
    QTest::addColumn<QString>("navigation");

    const QStringList navigations{QStringLiteral("open"), QStringLiteral("parent"),
                                  QStringLiteral("back"), QStringLiteral("forward"),
                                  QStringLiteral("refresh")};
    for (const bool useSecondaryPane : {false, true}) {
        const QString paneName =
            useSecondaryPane ? QStringLiteral("right") : QStringLiteral("left");
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
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    rfm::app::FileBrowserPane* const secondary = workspace->otherVisiblePane();
    const quint64 initialRequestId = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, initialRequestId),
        Q_ARG(QString, QStringLiteral("/root")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));

    rfm::app::FileBrowserPane* const pane = useSecondaryPane ? secondary : workspace->primaryPane();
    const QList<rfm::core::RemoteEntry> entries{{QStringLiteral("child"), 0, {}, true, false}};
    pane->showDirectory(QStringLiteral("/root"), QStringLiteral("/root"), {},
                        rfm::app::PaneNavigation::Initial);
    pane->showDirectory(QStringLiteral("/root/current"), QStringLiteral("/root/current"), entries,
                        rfm::app::PaneNavigation::Normal);
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
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, requestId), Q_ARG(QString, resultPath),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
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
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    auto* const secondary = workspace->otherVisiblePane();
    const quint64 requestId = requested.constFirst().constFirst().toULongLong();

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListingError", Qt::DirectConnection,
                                      Q_ARG(quint64, requestId),
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QString, QStringLiteral("Permission denied"))));

    QCOMPARE(workspace->primaryPane()->currentPath(), QStringLiteral("/srv"));
    QCOMPARE(workspace->primaryPane()->fileTable()->item(0, 0)->text(), QStringLiteral("kept.txt"));
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
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const back = window.findChild<QAction*>(QStringLiteral("backAction"));
    auto* const forward = window.findChild<QAction*>(QStringLiteral("forwardAction"));
    auto* const primary = workspace->primaryPane();
    QVERIFY(!back->isEnabled());
    QVERIFY(!forward->isEnabled());

    primary->navigateTo(QStringLiteral("/b"));
    const quint64 toB = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, toB), Q_ARG(QString, QStringLiteral("/b")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QVERIFY(back->isEnabled());
    QVERIFY(!forward->isEnabled());

    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    auto* const secondary = workspace->otherVisiblePane();
    const quint64 secondaryInitial = requested.constLast().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, secondaryInitial),
        Q_ARG(QString, QStringLiteral("/b")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
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
    auto* const remoteCoordinator = detachRemoteOperationExecutor(window);
    QVERIFY(remoteCoordinator != nullptr);
    window.show();
    const QList<rfm::core::RemoteEntry> sourceEntries{
        {QStringLiteral("file.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, sourceEntries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
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
        Q_ARG(QString, QStringLiteral("/destination")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
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
    auto* const operationTable = window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(operationTable != nullptr);
    const int copyRow = rowForId(operationTable, copyId);
    QVERIFY(copyRow >= 0);
    QCOMPARE(operationTable->item(copyRow, 0)->text(), QStringLiteral("Remote Copy"));
    QCOMPARE(operationTable->item(copyRow, 3)->text(), QStringLiteral("Queued"));
    QVERIFY(operationTable->cellWidget(copyRow, 4) != nullptr);
    auto* const copyActions = operationTable->cellWidget(copyRow, 7);
    QVERIFY(copyActions != nullptr);
    auto* const cancelCopy =
        copyActions->findChild<QToolButton*>(QStringLiteral("cancelTransferButton"));
    QVERIFY(cancelCopy != nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::cancelRemoteOperationRequested, nullptr,
                        nullptr);
    QSignalSpy copyCancellations(&window, &rfm::app::MainWindow::cancelRemoteOperationRequested);
    cancelCopy->click();
    QCOMPARE(copyCancellations.size(), 1);
    QCOMPARE(copyCancellations.constFirst().constFirst().toULongLong(), copyId);
    const rfm::core::RemoteOperationResult copyResult{
        copyId,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/file.txt"), QStringLiteral("/destination/file.txt"), true, {}}}};
    remoteCoordinator->handleRemoteExecutorResult(copyResult);
    QCOMPARE(operationTable->item(copyRow, 3)->text(), QStringLiteral("Completed"));

    auto* const debounce = window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"));
    debounce->stop();
    destinationPane->showDirectory(QStringLiteral("/destination"), QStringLiteral("/destination"),
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
    QCOMPARE(operationTable->item(moveRow, 3)->text(), QStringLiteral("Queued"));
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    const rfm::core::RemoteOperationResult moveResult{
        moveId,
        rfm::core::RemoteOperationKind::Move,
        {{QStringLiteral("/destination/back.txt"), QStringLiteral("/source/back.txt"), true, {}}}};
    remoteCoordinator->handleRemoteExecutorResult(moveResult);
    QCOMPARE(operationTable->item(moveRow, 3)->text(), QStringLiteral("Completed"));
    const int operationCount = operationTable->rowCount();
    const rfm::core::RemoteOperationResult createDirectoryResult{
        moveId + 100,
        rfm::core::RemoteOperationKind::CreateDirectory,
        {{QStringLiteral("/source"), QStringLiteral("/source/new"), true, {}}}};
    QVERIFY(
        QMetaObject::invokeMethod(&window, "handleOperationResult", Qt::DirectConnection,
                                  Q_ARG(rfm::core::RemoteOperationResult, createDirectoryResult)));
    QCOMPARE(operationTable->rowCount(), operationCount);
    QVERIFY(!workspace->isSplit());
}

void MainWindowTest::queuesRemoteCopiesWithoutBlockingTheWindow()
{
    rfm::app::MainWindow window;
    auto* const coordinator = detachRemoteOperationExecutor(window);
    QVERIFY(coordinator != nullptr);
    const QList<rfm::core::RemoteEntry> entries{{QStringLiteral("file.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    QSignalSpy copies(&window, &rfm::app::MainWindow::copyRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const copyOther = window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"));
    auto* const disconnect = window.findChild<QAction*>(QStringLiteral("disconnectAction"));
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(workspace != nullptr);
    QVERIFY(copyOther != nullptr);
    QVERIFY(disconnect != nullptr);
    QVERIFY(table != nullptr);

    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    const quint64 listingId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("/destination")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    auto* const sourcePane = workspace->primaryPane();
    sourcePane->fileTable()->selectRow(0);
    acceptNextQuestion();
    copyOther->trigger();
    QCOMPARE(copies.size(), 1);
    const quint64 firstId = copies.constFirst().constFirst().toULongLong();
    auto firstProgress = rfm::core::beginRemoteOperation(
        firstId, rfm::core::OperationKind::RemoteCopy,
        {{QStringLiteral("/source/file.txt"), false}}, QStringLiteral("/destination"));
    firstProgress.state = rfm::core::OperationState::Preparing;
    coordinator->handleRemoteExecutorProgress(firstProgress);

    QVERIFY(sourcePane->fileTable()->isEnabled());
    QVERIFY(copyOther->isEnabled());
    QVERIFY(!disconnect->isEnabled());
    acceptNextQuestion();
    copyOther->trigger();
    QCOMPARE(copies.size(), 2);
    const quint64 secondId = copies.at(1).constFirst().toULongLong();
    QCOMPARE(table->item(rowForId(table, firstId), 3)->text(), QStringLiteral("Preparing"));
    QCOMPARE(table->item(rowForId(table, secondId), 3)->text(), QStringLiteral("Queued"));
    QVERIFY(rowForId(table, firstId) < rowForId(table, secondId));

    QWidget* const queuedActions = table->cellWidget(rowForId(table, secondId), 7);
    QVERIFY(queuedActions != nullptr);
    auto* const cancelQueued =
        queuedActions->findChild<QToolButton*>(QStringLiteral("cancelTransferButton"));
    QVERIFY(cancelQueued != nullptr);
    dismissNextMessageBox();
    cancelQueued->click();
    QCOMPARE(table->item(rowForId(table, secondId), 3)->text(), QStringLiteral("Cancelled"));
    QVERIFY(!disconnect->isEnabled());

    const rfm::core::RemoteOperationResult completed{
        firstId,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/file.txt"), QStringLiteral("/destination/file.txt"), true, {}}}};
    coordinator->handleRemoteExecutorResult(completed);
    QCOMPARE(table->item(rowForId(table, firstId), 3)->text(), QStringLiteral("Completed"));
    QVERIFY(disconnect->isEnabled());
}

void MainWindowTest::remoteExecutorFailureShowsOnlyConnectionDialog()
{
    rfm::app::MainWindow window;
    auto* const coordinator = detachRemoteOperationExecutor(window);
    QVERIFY(coordinator != nullptr);
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    coordinator->enqueueRemoteOperation({901,
                                         rfm::core::RemoteOperationKind::Copy,
                                         {{QStringLiteral("/source/a.txt"), false}},
                                         QStringLiteral("/destination")});
    coordinator->enqueueRemoteOperation({902,
                                         rfm::core::RemoteOperationKind::Move,
                                         {{QStringLiteral("/source/b.txt"), false}},
                                         QStringLiteral("/destination")});

    coordinator->handleRemoteExecutorFailure(QStringLiteral("connection lost"));
    auto failedProgress = rfm::core::operationProgress({901,
                                                        rfm::core::RemoteOperationKind::Copy,
                                                        {{QStringLiteral("/source/a.txt"), false}},
                                                        QStringLiteral("/destination")},
                                                       rfm::core::OperationState::Failed,
                                                       QStringLiteral("connection lost"));
    coordinator->handleRemoteExecutorProgress(failedProgress);
    coordinator->handleRemoteExecutorResult(
        {901,
         rfm::core::RemoteOperationKind::Copy,
         {{QStringLiteral("/source/a.txt"), QStringLiteral("/destination/a.txt"), false,
           QStringLiteral("connection lost")}}});
    QVERIFY(QApplication::activeModalWidget() == nullptr);
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(table != nullptr);
    QCOMPARE(table->item(rowForId(table, 901), 3)->text(), QStringLiteral("Failed"));
    QCOMPARE(table->item(rowForId(table, 902), 3)->text(), QStringLiteral("Failed"));

    int connectionDialogs = 0;
    QTimer::singleShot(0, [&connectionDialogs] {
        auto* const messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(messageBox != nullptr);
        QCOMPARE(messageBox->windowTitle(), QStringLiteral("SSH connection error"));
        ++connectionDialogs;
        messageBox->accept();
    });
    QVERIFY(QMetaObject::invokeMethod(&window, "showConnectionError", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("connection lost"))));
    QCOMPARE(connectionDialogs, 1);
    QVERIFY(QApplication::activeModalWidget() == nullptr);
}

void MainWindowTest::rejectsOtherPaneOperationsForSameDirectory()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{{QStringLiteral("file.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/same")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
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
        Q_ARG(QString, QStringLiteral("/same/./")), Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QTest::mouseClick(workspace->primaryPane()->fileTable()->viewport(), Qt::LeftButton);
    workspace->primaryPane()->fileTable()->selectRow(0);

    QVERIFY(!copyOther->isEnabled());
    QVERIFY(!moveOther->isEnabled());
    copyOther->trigger();
    moveOther->trigger();
    QCOMPARE(copies.size(), 0);
    QCOMPARE(moves.size(), 0);

    workspace->primaryPane()->showDirectory(QStringLiteral("rfm-sprint4"),
                                            QStringLiteral("sftp://host/~/rfm-sprint4"), entries);
    workspace->otherVisiblePane()->showDirectory(
        QStringLiteral("/home/gabriel/rfm-sprint4"),
        QStringLiteral("sftp://host/home/gabriel/rfm-sprint4"), entries);
    QTest::mouseClick(workspace->primaryPane()->fileTable()->viewport(), Qt::LeftButton);
    workspace->primaryPane()->fileTable()->clearSelection();
    workspace->primaryPane()->fileTable()->selectRow(0);
    QVERIFY(!copyOther->isEnabled());
    QVERIFY(!moveOther->isEnabled());
}

void MainWindowTest::fileContextMenuOffersProperties_data()
{
    QTest::addColumn<bool>("local");
    QTest::addColumn<bool>("directory");

    QTest::newRow("local-file") << true << false;
    QTest::newRow("local-folder") << true << true;
    QTest::newRow("remote-file") << false << false;
    QTest::newRow("remote-folder") << false << true;
}

void MainWindowTest::fileContextMenuOffersProperties()
{
    QFETCH(bool, local);
    QFETCH(bool, directory);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::MainWindow window;
    window.show();
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const pane = workspace->primaryPane();
    const QString name = directory ? QStringLiteral("folder") : QStringLiteral("file.txt");
    const rfm::core::BrowserLocation location{
        local ? rfm::core::FileSource::Local : rfm::core::FileSource::Ssh,
        local ? QString::fromLatin1(rfm::core::LocalMachineId) : QStringLiteral("remote-id"),
        local ? temporary.path() : QStringLiteral("/srv")};
    pane->showDirectory(location, location.path,
                        {{name, directory ? quint64{0} : quint64{42},
                          QDateTime::fromSecsSinceEpoch(1'700'000'000), directory, false}});
    const QPoint position =
        pane->fileTable()->visualItemRect(pane->fileTable()->item(0, 0)).center();
    QAction* const properties = window.findChild<QAction*>(QStringLiteral("filePropertiesAction"));
    QVERIFY(properties != nullptr);

    QTimer::singleShot(0, [&window, properties] {
        auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu != nullptr);
        const QList<QAction*> actions = menu->actions();
        QVERIFY(actions.contains(properties));
        QVERIFY(
            actions.contains(window.findChild<QAction*>(QStringLiteral("clipboardCopyAction"))));
        QVERIFY(actions.contains(window.findChild<QAction*>(QStringLiteral("removeAction"))));
        QVERIFY(
            actions.contains(window.findChild<QAction*>(QStringLiteral("createDirectoryAction"))));
        QCOMPARE(actions.constLast(), properties);
        QVERIFY(actions.at(actions.size() - 2)->isSeparator());
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(pane->fileTable(), "customContextMenuRequested",
                                      Qt::DirectConnection, Q_ARG(QPoint, position)));
    const auto clickedProperties = pane->contextEntryProperties();
    QVERIFY(clickedProperties.has_value());
    QCOMPARE(clickedProperties->title, name);
}

void MainWindowTest::contextMenuOffersOpenForSingleLocalFilesOnly()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir directory(temporary.path());
    QVERIFY(directory.mkdir(QStringLiteral("folder")));
    for (const QString& name : {QStringLiteral("first.txt"), QStringLiteral("second.txt")}) {
        QFile file(directory.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    rfm::app::MainWindow window;
    window.show();
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    auto* const pane = workspace->activePane();
    pane->showDirectory({rfm::core::FileSource::Local,
                         QString::fromLatin1(rfm::core::LocalMachineId), temporary.path()},
                        QUrl::fromLocalFile(temporary.path()).toDisplayString(),
                        {{QStringLiteral("first.txt"), 0, {}, false, false},
                         {QStringLiteral("folder"), 0, {}, true, false},
                         {QStringLiteral("second.txt"), 0, {}, false, false}});
    QCoreApplication::processEvents();
    QAction* const openAction = window.findChild<QAction*>(QStringLiteral("openAction"));
    QVERIFY(openAction != nullptr);
    UrlHandler handler;
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &handler, "handleUrl");
    ScopedUrlHandler resetHandler;

    const auto requestMenu = [pane](int row) {
        const QPoint position =
            pane->fileTable()->visualItemRect(pane->fileTable()->item(row, 0)).center();
        QVERIFY(QMetaObject::invokeMethod(pane->fileTable(), "customContextMenuRequested",
                                          Qt::DirectConnection, Q_ARG(QPoint, position)));
    };

    QTimer::singleShot(0, [&] {
        auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu != nullptr);
        QCOMPARE(menu->actions().constFirst(), openAction);
        QVERIFY(openAction->isEnabled());
        openAction->trigger();
        menu->close();
    });
    requestMenu(0);
    QCOMPARE(handler.urls,
             QList<QUrl>{QUrl::fromLocalFile(directory.filePath(QStringLiteral("first.txt")))});

    QTimer::singleShot(0, [&] {
        auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu != nullptr);
        QVERIFY(!menu->actions().contains(openAction));
        menu->close();
    });
    requestMenu(1);

    pane->showDirectory(
        {rfm::core::FileSource::Ssh, QStringLiteral("ssh:fixture"), QStringLiteral("/remote")},
        QStringLiteral("sftp://fixture/remote"),
        {{QStringLiteral("remote.txt"), 0, {}, false, false}});
    QTimer::singleShot(0, [&] {
        auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu != nullptr);
        QVERIFY(!menu->actions().contains(openAction));
        menu->close();
    });
    requestMenu(0);

    pane->showDirectory({rfm::core::FileSource::Local,
                         QString::fromLatin1(rfm::core::LocalMachineId), temporary.path()},
                        QUrl::fromLocalFile(temporary.path()).toDisplayString(),
                        {{QStringLiteral("first.txt"), 0, {}, false, false},
                         {QStringLiteral("second.txt"), 0, {}, false, false}});
    pane->fileTable()->selectRow(0);
    pane->fileTable()->selectionModel()->select(pane->fileTable()->model()->index(1, 0),
                                                QItemSelectionModel::Select |
                                                    QItemSelectionModel::Rows);
    QTimer::singleShot(0, [&] {
        auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu != nullptr);
        QVERIFY(!menu->actions().contains(openAction));
        menu->close();
    });
    requestMenu(0);
    QCOMPARE(handler.urls.size(), 1);
}

void MainWindowTest::contextMenuUsesSharedInterPaneActions()
{
    rfm::app::MainWindow window;
    auto* const remoteCoordinator = detachRemoteOperationExecutor(window);
    QVERIFY(remoteCoordinator != nullptr);
    window.show();
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("deplacement.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("rfm-sprint4/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
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
        QCOMPARE(actions.size(), 11);
        QCOMPARE(actions.at(0), window.findChild<QAction*>(QStringLiteral("clipboardCopyAction")));
        QCOMPARE(actions.at(1), window.findChild<QAction*>(QStringLiteral("clipboardCutAction")));
        QCOMPARE(actions.at(2), window.findChild<QAction*>(QStringLiteral("clipboardPasteAction")));
        QVERIFY(!actions.at(2)->isEnabled());
        QVERIFY(actions.at(3)->isSeparator());
        QCOMPARE(actions.at(4), window.findChild<QAction*>(QStringLiteral("copyAction")));
        QCOMPARE(actions.at(5), window.findChild<QAction*>(QStringLiteral("moveAction")));
        QVERIFY(actions.at(6)->isSeparator());
        QCOMPARE(actions.at(7), window.findChild<QAction*>(QStringLiteral("renameAction")));
        QCOMPARE(actions.at(8), window.findChild<QAction*>(QStringLiteral("removeAction")));
        QVERIFY(actions.at(9)->isSeparator());
        QCOMPARE(actions.at(10),
                 window.findChild<QAction*>(QStringLiteral("createDirectoryAction")));
        QVERIFY(
            !actions.contains(window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"))));
        QVERIFY(
            !actions.contains(window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction"))));
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(
        sourcePane, "contextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, sourcePane->fileTable()->viewport()->mapToGlobal(QPoint{4, 4}))));

    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    const quint64 listingId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, listingId),
                                      Q_ARG(QString, QStringLiteral("rfm-sprint4/dossier-test")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QTest::mouseClick(sourcePane->fileTable()->viewport(), Qt::LeftButton);
    sourcePane->fileTable()->selectRow(0);

    QTimer::singleShot(0, [&window] {
        auto* const menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu != nullptr);
        const QList<QAction*> actions = menu->actions();
        QCOMPARE(actions.size(), 13);
        QCOMPARE(actions.at(0), window.findChild<QAction*>(QStringLiteral("clipboardCopyAction")));
        QCOMPARE(actions.at(1), window.findChild<QAction*>(QStringLiteral("clipboardCutAction")));
        QCOMPARE(actions.at(2), window.findChild<QAction*>(QStringLiteral("clipboardPasteAction")));
        QVERIFY(actions.at(3)->isSeparator());
        QCOMPARE(actions.at(4),
                 window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction")));
        QCOMPARE(actions.at(5),
                 window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction")));
        QCOMPARE(actions.at(6), window.findChild<QAction*>(QStringLiteral("copyAction")));
        QCOMPARE(actions.at(7), window.findChild<QAction*>(QStringLiteral("moveAction")));
        QVERIFY(actions.at(8)->isSeparator());
        QCOMPARE(actions.at(9), window.findChild<QAction*>(QStringLiteral("renameAction")));
        QCOMPARE(actions.at(10), window.findChild<QAction*>(QStringLiteral("removeAction")));
        QVERIFY(actions.at(11)->isSeparator());
        QCOMPARE(actions.at(12),
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
        copies.constFirst().constFirst().toULongLong(),
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("rfm-sprint4/source/deplacement.txt"),
          QStringLiteral("rfm-sprint4/dossier-test/deplacement.txt"),
          true,
          {}}}};
    remoteCoordinator->handleRemoteExecutorResult(copyResult);
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
    auto* const remoteCoordinator = detachRemoteOperationExecutor(window);
    QVERIFY(remoteCoordinator != nullptr);
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
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, listingId),
                                      Q_ARG(QString, QStringLiteral("/destination")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, destinationEntries)));
    auto* const destinationPane = workspace->otherVisiblePane();

    const auto triggerContextAction = [&](rfm::app::FileBrowserPane* pane, const QPoint& position,
                                          const QString& actionName,
                                          const QString& explicitDestination = QString{}) {
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
        QVERIFY(QMetaObject::invokeMethod(pane->fileTable(), "customContextMenuRequested",
                                          Qt::DirectConnection, Q_ARG(QPoint, position)));
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
        firstPasteId,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/first.txt"), QStringLiteral("/destination/first.txt"), true, {}},
         {QStringLiteral("/source/second.txt"),
          QStringLiteral("/destination/second.txt"),
          true,
          {}}}};
    remoteCoordinator->handleRemoteExecutorResult(firstPasteResult);
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
        secondPasteId,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/first.txt"), QStringLiteral("/destination/first.txt"), true, {}},
         {QStringLiteral("/source/second.txt"),
          QStringLiteral("/destination/second.txt"),
          true,
          {}}}};
    remoteCoordinator->handleRemoteExecutorResult(secondPasteResult);
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
        manualCopyId,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/first.txt"),
          QStringLiteral("/manual-copy/first.txt"),
          true,
          {}}}};
    remoteCoordinator->handleRemoteExecutorResult(manualCopyResult);
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
    auto* const remoteCoordinator = detachRemoteOperationExecutor(window);
    QVERIFY(remoteCoordinator != nullptr);
    window.show();
    const QList<rfm::core::RemoteEntry> fileEntry{
        {QStringLiteral("fichier.txt"), 1, {}, false, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("rfm-sprint4/dossier-test/./")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, fileEntry)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
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
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, initialListingId),
                                      Q_ARG(QString, QStringLiteral("rfm-sprint4/./")),
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
    remoteCoordinator->handleRemoteExecutorResult(completedCopy);
    window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"))->stop();

    primary->showDirectory(
        QStringLiteral("/home/gabriel/rfm-sprint4/dossier-test/"),
        QStringLiteral("sftp://gabriel@example.test/home/gabriel/rfm-sprint4/dossier-test"),
        fileEntry, rfm::app::PaneNavigation::Initial);
    secondary->showDirectory(QStringLiteral("/home/gabriel/rfm-sprint4/./"),
                             QStringLiteral("sftp://gabriel@example.test/home/gabriel/rfm-sprint4"),
                             fileEntry, rfm::app::PaneNavigation::Initial);
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
    QObject::disconnect(&window, SIGNAL(directoryRequested(quint64, QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    QVERIFY(workspace->otherVisiblePane() != nullptr);
    const quint64 initialId = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, initialId),
        Q_ARG(QString, QStringLiteral("/destination")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    requested.clear();
    auto* const debounce = window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"));

    const rfm::core::RemoteOperationResult copyResult{
        700,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/a"), QStringLiteral("/destination/a"), true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleOperationResult", Qt::DirectConnection,
                                      Q_ARG(rfm::core::RemoteOperationResult, copyResult)));
    window.statusBar()->showMessage(QStringLiteral("Remote operation completed"));
    debounce->stop();
    QVERIFY(QMetaObject::invokeMethod(debounce, "timeout", Qt::DirectConnection));
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Remote operation completed"));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().at(1).toString(), QStringLiteral("/destination"));
    const quint64 copyRefresh = requested.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, copyRefresh),
        Q_ARG(QString, QStringLiteral("/destination")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));

    requested.clear();
    const rfm::core::RemoteOperationResult moveResult{
        701,
        rfm::core::RemoteOperationKind::Move,
        {{QStringLiteral("/source/a"), QStringLiteral("/destination/a"), true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleOperationResult", Qt::DirectConnection,
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
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, sourceRefresh), Q_ARG(QString, firstMovePath),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QCOMPARE(requested.size(), 2);
    QCOMPARE(requested.at(1).at(1).toString(), secondMovePath);
    const quint64 destinationRefresh = requested.at(1).constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, destinationRefresh),
        Q_ARG(QString, secondMovePath), Q_ARG(QList<rfm::core::RemoteEntry>, {})));

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
        QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                          Q_ARG(rfm::core::TransferProgress, completed)));
        QTest::qWait(5);
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
        auto* const clear =
            window.findChild<QToolButton*>(QStringLiteral("clearOperationHistoryButton"));
        auto* const saveTimer =
            window.findChild<QTimer*>(QStringLiteral("operationHistorySaveTimer"));
        QVERIFY(table != nullptr);
        QVERIFY(clear != nullptr);
        QCOMPARE(clear->text(), QString{});
        QCOMPARE(clear->toolTip(), QStringLiteral("Clear operation history"));
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(operationIds(table), (QList<quint64>{902, 901}));
        QCOMPARE(table->item(rowForId(table, 901), 3)->text(), QStringLiteral("Completed"));
        QCOMPARE(table->item(rowForId(table, 902), 3)->text(), QStringLiteral("Failed"));
        QCOMPARE(table->item(rowForId(table, 901), 0)->toolTip(),
                 QStringLiteral("Server: history.example.test:2222"));

        auto* const remove = table->cellWidget(rowForId(table, 901), 7)
                                 ->findChild<QToolButton*>(QStringLiteral("removeOperationButton"));
        QVERIFY(remove != nullptr);
        QTest::mouseClick(remove, Qt::LeftButton);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(rowForId(table, 901), -1);

        auto active = progress(903, rfm::core::TransferState::Transferring, 1, 10);
        QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                          Q_ARG(rfm::core::TransferProgress, active)));
        QCOMPARE(table->rowCount(), 2);
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
    auto* const remoteCoordinator = detachRemoteOperationExecutor(window);
    QVERIFY(remoteCoordinator != nullptr);
    window.show();
    const QList<rfm::core::RemoteEntry> entries{{QStringLiteral("file.txt"), 1, {}, false, false},
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
        Q_ARG(QString, QStringLiteral("/destination")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));

    auto* const sourcePane = workspace->primaryPane();
    auto* const destinationPane = workspace->otherVisiblePane();
    sourcePane->fileTable()->selectRow(0);
    auto* const copyAction = window.findChild<QAction*>(QStringLiteral("clipboardCopyAction"));
    auto* const cutAction = window.findChild<QAction*>(QStringLiteral("clipboardCutAction"));
    auto* const pasteAction = window.findChild<QAction*>(QStringLiteral("clipboardPasteAction"));
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
        copyId,
        rfm::core::RemoteOperationKind::Copy,
        {{QStringLiteral("/source/file.txt"), QStringLiteral("/destination/file.txt"), true, {}}}};
    remoteCoordinator->handleRemoteExecutorResult(copyResult);
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
        moveId,
        rfm::core::RemoteOperationKind::Move,
        {{QStringLiteral("/source/file.txt"), QStringLiteral("/destination/file.txt"), true, {}}}};
    remoteCoordinator->handleRemoteExecutorResult(moveResult);
    QVERIFY(!pasteAction->isEnabled());
    QVERIFY(!sourcePane->fileTable()->item(0, 0)->font().italic());

    // A completed Move must not clear clipboard content that was replaced while it was running.
    QTest::mouseClick(sourcePane->fileTable()->viewport(), Qt::LeftButton);
    sourcePane->fileTable()->selectRow(0);
    cutAction->trigger();
    QTest::mouseClick(destinationPane->fileTable()->viewport(), Qt::LeftButton);
    pasteAction->trigger();
    QCOMPARE(moves.size(), 2);
    const quint64 olderMoveId = moves.at(1).constFirst().toULongLong();
    QTest::mouseClick(sourcePane->fileTable()->viewport(), Qt::LeftButton);
    sourcePane->fileTable()->selectRow(1);
    copyAction->trigger();
    QTest::mouseClick(destinationPane->fileTable()->viewport(), Qt::LeftButton);
    QVERIFY(pasteAction->isEnabled());
    remoteCoordinator->handleRemoteExecutorResult({olderMoveId,
                                                   rfm::core::RemoteOperationKind::Move,
                                                   {{QStringLiteral("/source/file.txt"),
                                                     QStringLiteral("/destination/file.txt"),
                                                     true,
                                                     {}}}});
    QVERIFY(pasteAction->isEnabled());
    QVERIFY(!sourcePane->fileTable()->item(1, 0)->font().italic());

    // A failed Move keeps the matching Cut available.
    QTest::mouseClick(sourcePane->fileTable()->viewport(), Qt::LeftButton);
    sourcePane->fileTable()->selectRow(0);
    cutAction->trigger();
    QTest::mouseClick(destinationPane->fileTable()->viewport(), Qt::LeftButton);
    pasteAction->trigger();
    QCOMPARE(moves.size(), 3);
    const quint64 failedMoveId = moves.at(2).constFirst().toULongLong();
    dismissNextMessageBox();
    remoteCoordinator->handleRemoteExecutorResult(
        {failedMoveId,
         rfm::core::RemoteOperationKind::Move,
         {{QStringLiteral("/source/file.txt"), QStringLiteral("/destination/file.txt"), false,
           QStringLiteral("Move failed")}}});
    QVERIFY(pasteAction->isEnabled());
    QVERIFY(sourcePane->fileTable()->item(0, 0)->font().italic());
    QVERIFY(cancelCut->isEnabled());
    cancelCut->trigger();
    QVERIFY(!pasteAction->isEnabled());
    QVERIFY(!sourcePane->fileTable()->item(0, 0)->font().italic());

    QTest::mouseClick(sourcePane->fileTable()->viewport(), Qt::LeftButton);
    sourcePane->fileTable()->selectRow(0);
    cutAction->trigger();
    QVERIFY(cancelCut->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QVERIFY(!pasteAction->isEnabled());
    QVERIFY(!cancelCut->isEnabled());
    QCOMPARE(sourcePane->fileTable()->rowCount(), 0);
}

void MainWindowTest::remoteDragDropRoutesModifierIntentionsToExistingRequests()
{
    rfm::app::MainWindow window;
    auto* const remoteCoordinator = detachRemoteOperationExecutor(window);
    QVERIFY(remoteCoordinator != nullptr);
    window.show();
    const QList<rfm::core::RemoteEntry> sourceEntries{
        {QStringLiteral("a.txt"), 1, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, sourceEntries)));
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    const quint64 listingId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("/destination")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    auto* const sourcePane = workspace->primaryPane();
    auto* const destinationPane = workspace->otherVisiblePane();
    sourcePane->fileTable()->selectAll();
    const auto payload = rfm::core::decodeInternalTransfer(sourcePane->createInternalDragData());
    QVERIFY(payload.has_value());
    QCOMPARE(payload->sources.size(), 2);

    QObject::disconnect(&window, &rfm::app::MainWindow::copyRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::moveRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteFilesystemRelationRequested, nullptr,
                        nullptr);
    QSignalSpy copies(&window, &rfm::app::MainWindow::copyRequested);
    QSignalSpy moves(&window, &rfm::app::MainWindow::moveRequested);
    QSignalSpy filesystemRequests(&window,
                                  &rfm::app::MainWindow::remoteFilesystemRelationRequested);
    QSignalSpy remoteRequests(&window, &rfm::app::MainWindow::remoteOperationRequested);
    QMimeData mime;
    mime.setData(rfm::core::InternalTransferMimeType, sourcePane->createInternalDragData());
    const QPoint background(10, destinationPane->fileTable()->viewport()->height() - 2);
    const auto sendDrop = [&](Qt::KeyboardModifiers modifiers,
                              rfm::core::RemoteOperationKind expectedKind) {
        const Qt::DropAction expectedDropAction =
            expectedKind == rfm::core::RemoteOperationKind::Copy ? Qt::CopyAction : Qt::MoveAction;
        QDragEnterEvent enter(background, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                              modifiers);
        QApplication::sendEvent(destinationPane->fileTable()->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QCOMPARE(enter.dropAction(), expectedDropAction);
        QDropEvent drop(QPointF(background), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                        modifiers);
        QApplication::sendEvent(destinationPane->fileTable()->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(drop.dropAction(), expectedDropAction);

        if (modifiers == Qt::NoModifier) {
            QCOMPARE(filesystemRequests.size(), 1);
            const QList<QVariant> filesystemRequest = filesystemRequests.takeFirst();
            QCOMPARE(filesystemRequest.at(1).toString(), QStringLiteral("/source"));
            QCOMPARE(filesystemRequest.at(2).toString(), QStringLiteral("/destination"));
            QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteFilesystemRelation",
                                              Qt::DirectConnection,
                                              Q_ARG(quint64, filesystemRequest.at(0).toULongLong()),
                                              Q_ARG(rfm::core::RemoteFilesystemRelation,
                                                    rfm::core::RemoteFilesystemRelation::Same)));
        } else {
            QCOMPARE(filesystemRequests.size(), 0);
        }

        QCOMPARE(remoteRequests.size(), 1);
        const auto request = qvariant_cast<rfm::core::RemoteOperationRequest>(
            remoteRequests.takeFirst().constFirst());
        QCOMPARE(request.kind, expectedKind);
        QCOMPARE(request.sources.size(), 2);
        QCOMPARE(request.sources.at(0).path, QStringLiteral("/source/a.txt"));
        QVERIFY(!request.sources.at(0).directory);
        QCOMPARE(request.sources.at(1).path, QStringLiteral("/source/folder"));
        QVERIFY(request.sources.at(1).directory);
        QCOMPARE(request.destinationDirectory, QStringLiteral("/destination"));
        if (expectedKind == rfm::core::RemoteOperationKind::Copy) {
            QCOMPARE(copies.size(), 1);
            QCOMPARE(moves.size(), 0);
            copies.clear();
        } else {
            QCOMPARE(copies.size(), 0);
            QCOMPARE(moves.size(), 1);
            moves.clear();
        }

        const rfm::core::RemoteOperationResult result{
            request.id,
            expectedKind,
            {{QStringLiteral("/source/a.txt"), QStringLiteral("/destination/a.txt"), true, {}},
             {QStringLiteral("/source/folder"), QStringLiteral("/destination/folder"), true, {}}}};
        remoteCoordinator->handleRemoteExecutorResult(result);
        QApplication::processEvents();
    };

    sendDrop(Qt::NoModifier, rfm::core::RemoteOperationKind::Copy);
    sendDrop(Qt::ControlModifier, rfm::core::RemoteOperationKind::Copy);
    sendDrop(Qt::ShiftModifier, rfm::core::RemoteOperationKind::Move);
    sendDrop(Qt::ControlModifier | Qt::ShiftModifier, rfm::core::RemoteOperationKind::Copy);
}

void MainWindowTest::remoteCrossFilesystemPreflightRoutesOriginalDrop()
{
    rfm::app::MainWindow window;
    auto* const remoteCoordinator = detachRemoteOperationExecutor(window);
    QVERIFY(remoteCoordinator != nullptr);
    window.show();
    const QList<rfm::core::RemoteEntry> sourceEntries{
        {QStringLiteral("a.txt"), 1, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false}};
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, sourceEntries)));
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    window.findChild<QAction*>(QStringLiteral("splitViewAction"))->trigger();
    const quint64 listingId = listings.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, listingId),
        Q_ARG(QString, QStringLiteral("/destination")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    auto* const sourcePane = workspace->primaryPane();
    auto* const destinationPane = workspace->otherVisiblePane();
    QVERIFY(destinationPane != nullptr);
    sourcePane->fileTable()->selectAll();

    QObject::disconnect(&window, &rfm::app::MainWindow::remoteFilesystemRelationRequested, nullptr,
                        nullptr);
    QSignalSpy filesystemRequests(&window,
                                  &rfm::app::MainWindow::remoteFilesystemRelationRequested);
    QSignalSpy remoteRequests(&window, &rfm::app::MainWindow::remoteOperationRequested);
    QMimeData mime;
    mime.setData(rfm::core::InternalTransferMimeType, sourcePane->createInternalDragData());
    const QPoint background(10, destinationPane->fileTable()->viewport()->height() - 2);
    QList<QVariant> request;
    const auto beginDrop = [&] {
        QDragEnterEvent enter(background, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                              Qt::NoModifier);
        QApplication::sendEvent(destinationPane->fileTable()->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QCOMPARE(enter.dropAction(), Qt::CopyAction);
        QDropEvent drop(QPointF(background), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                        Qt::NoModifier);
        QApplication::sendEvent(destinationPane->fileTable()->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(drop.dropAction(), Qt::CopyAction);
        QCOMPARE(filesystemRequests.size(), 1);
        request = filesystemRequests.takeFirst();
    };
    const auto finishRequest = [&](rfm::core::RemoteOperationKind expectedKind,
                                   const QString& expectedDestination) {
        QCOMPARE(remoteRequests.size(), 1);
        const auto request = qvariant_cast<rfm::core::RemoteOperationRequest>(
            remoteRequests.takeFirst().constFirst());
        QCOMPARE(request.kind, expectedKind);
        QCOMPARE(request.sources.size(), 2);
        QCOMPARE(request.sources.at(0).path, QStringLiteral("/source/a.txt"));
        QCOMPARE(request.sources.at(1).path, QStringLiteral("/source/folder"));
        QCOMPARE(request.destinationDirectory, expectedDestination);
        remoteCoordinator->handleRemoteExecutorResult(
            {request.id,
             expectedKind,
             {{QStringLiteral("/source/a.txt"),
               expectedDestination + QStringLiteral("/a.txt"),
               true,
               {}},
              {QStringLiteral("/source/folder"),
               expectedDestination + QStringLiteral("/folder"),
               true,
               {}}}});
        QApplication::processEvents();
    };

    beginDrop();
    chooseCrossFilesystemDropButton(QStringLiteral("crossFilesystemDropCopyButton"));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteFilesystemRelation",
                                      Qt::DirectConnection,
                                      Q_ARG(quint64, request.at(0).toULongLong()),
                                      Q_ARG(rfm::core::RemoteFilesystemRelation,
                                            rfm::core::RemoteFilesystemRelation::Different)));
    finishRequest(rfm::core::RemoteOperationKind::Copy, QStringLiteral("/destination"));

    beginDrop();
    chooseCrossFilesystemDropButton(QStringLiteral("crossFilesystemDropMoveButton"));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteFilesystemRelation",
                                      Qt::DirectConnection,
                                      Q_ARG(quint64, request.at(0).toULongLong()),
                                      Q_ARG(rfm::core::RemoteFilesystemRelation,
                                            rfm::core::RemoteFilesystemRelation::Different)));
    finishRequest(rfm::core::RemoteOperationKind::Move, QStringLiteral("/destination"));

    beginDrop();
    chooseCrossFilesystemDropButton(QStringLiteral("crossFilesystemDropCancelButton"));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteFilesystemRelation",
                                      Qt::DirectConnection,
                                      Q_ARG(quint64, request.at(0).toULongLong()),
                                      Q_ARG(rfm::core::RemoteFilesystemRelation,
                                            rfm::core::RemoteFilesystemRelation::Different)));
    QCOMPARE(remoteRequests.size(), 0);

    beginDrop();
    sourcePane->fileTable()->clearSelection();
    const QString remoteMachineId = destinationPane->currentLocation().machineId;
    destinationPane->showDirectory(
        {rfm::core::FileSource::Ssh, remoteMachineId, QStringLiteral("/changed")},
        QStringLiteral("/changed"), {});
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteFilesystemRelation", Qt::DirectConnection,
        Q_ARG(quint64, request.at(0).toULongLong()),
        Q_ARG(rfm::core::RemoteFilesystemRelation, rfm::core::RemoteFilesystemRelation::Unknown)));
    finishRequest(rfm::core::RemoteOperationKind::Copy, QStringLiteral("/destination"));
}

void MainWindowTest::localDragDropRoutesModifierIntentionsToExistingRequests_data()
{
    QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
    QTest::addColumn<rfm::core::LocalFileOperationKind>("expectedKind");
    QTest::addColumn<Qt::DropAction>("expectedDropAction");

    QTest::newRow("normal-copy") << Qt::KeyboardModifiers{}
                                 << rfm::core::LocalFileOperationKind::Copy << Qt::CopyAction;
    QTest::newRow("control-copy") << Qt::KeyboardModifiers{Qt::ControlModifier}
                                  << rfm::core::LocalFileOperationKind::Copy << Qt::CopyAction;
    QTest::newRow("shift-move") << Qt::KeyboardModifiers{Qt::ShiftModifier}
                                << rfm::core::LocalFileOperationKind::Move << Qt::MoveAction;
    QTest::newRow("control-priority")
        << Qt::KeyboardModifiers{Qt::ControlModifier | Qt::ShiftModifier}
        << rfm::core::LocalFileOperationKind::Copy << Qt::CopyAction;
}

void MainWindowTest::localDragDropRoutesModifierIntentionsToExistingRequests()
{
    QFETCH(Qt::KeyboardModifiers, modifiers);
    QFETCH(rfm::core::LocalFileOperationKind, expectedKind);
    QFETCH(Qt::DropAction, expectedDropAction);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourceDirectory = QDir(temporary.path()).filePath(QStringLiteral("source"));
    const QString destinationDirectory =
        QDir(temporary.path()).filePath(QStringLiteral("destination"));
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(QDir().mkpath(destinationDirectory));
    for (const QString& name : {QStringLiteral("a.txt"), QStringLiteral("b.txt")}) {
        QFile file(QDir(sourceDirectory).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(name.toUtf8()) == name.size());
    }

    rfm::app::MainWindow window;
    showLocalSplit(window, sourceDirectory, destinationDirectory,
                   {{QStringLiteral("a.txt"), 5, {}, false, false},
                    {QStringLiteral("b.txt"), 5, {}, false, false}},
                   {});
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    auto* const sourcePane = workspace->primaryPane();
    auto* const destinationPane = workspace->otherVisiblePane();
    QVERIFY(destinationPane != nullptr);
    sourcePane->fileTable()->selectAll();
    const auto localPayload =
        rfm::core::decodeInternalTransfer(sourcePane->createInternalDragData());
    QVERIFY(localPayload.has_value());
    QCOMPARE(localPayload->source, rfm::core::FileSource::Local);
    QCOMPARE(localPayload->sources.size(), 2);

    QObject::disconnect(&window, &rfm::app::MainWindow::localFileOperationRequested, nullptr,
                        nullptr);
    QSignalSpy localRequests(&window, &rfm::app::MainWindow::localFileOperationRequested);
    QSignalSpy remoteRequests(&window, &rfm::app::MainWindow::remoteOperationRequested);
    QMimeData mime;
    mime.setData(rfm::core::InternalTransferMimeType, sourcePane->createInternalDragData());
    const QPoint background(10, destinationPane->fileTable()->viewport()->height() - 2);
    QDragEnterEvent enter(background, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                          modifiers);
    QApplication::sendEvent(destinationPane->fileTable()->viewport(), &enter);
    QVERIFY(enter.isAccepted());
    QCOMPARE(enter.dropAction(), expectedDropAction);
    QDropEvent drop(QPointF(background), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                    modifiers);
    QApplication::sendEvent(destinationPane->fileTable()->viewport(), &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(drop.dropAction(), expectedDropAction);
    QCOMPARE(localRequests.size(), 1);
    QCOMPARE(remoteRequests.size(), 0);
    const auto request = qvariant_cast<rfm::core::LocalFileOperationRequest>(
        localRequests.constFirst().constFirst());
    QCOMPARE(request.kind, expectedKind);
    QCOMPARE(request.parentPath, sourceDirectory);
    QCOMPARE(request.sourcePaths,
             QStringList({QDir(sourceDirectory).filePath(QStringLiteral("a.txt")),
                          QDir(sourceDirectory).filePath(QStringLiteral("b.txt"))}));
    QCOMPARE(request.destinationDirectory, destinationDirectory);
    auto* const operationTable = window.findChild<QTableWidget*>(QStringLiteral("operationTable"));
    QVERIFY(operationTable != nullptr);
    const int operationRow = rowForId(operationTable, request.id);
    QVERIFY(operationRow >= 0);
    QCOMPARE(operationTable->item(operationRow, 0)->text(),
             expectedKind == rfm::core::LocalFileOperationKind::Copy ? QStringLiteral("Copy")
                                                                     : QStringLiteral("Move"));
    QCOMPARE(operationTable->item(operationRow, 3)->text(), QStringLiteral("Queued"));
    QVERIFY(QFileInfo(QDir(sourceDirectory).filePath(QStringLiteral("a.txt"))).exists());
    QVERIFY(!QFileInfo(QDir(destinationDirectory).filePath(QStringLiteral("a.txt"))).exists());
}

void MainWindowTest::promptsForConfirmedCrossVolumeLocalDragDrop()
{
#ifndef Q_OS_LINUX
    QSKIP("A portable distinct local filesystem is not available for this UI test.");
#else
    if (!QDir(QStringLiteral("/dev/shm")).exists()) {
        QSKIP("The standard temporary memory filesystem is not available.");
    }
    QTemporaryDir sourceTemporary;
    QTemporaryDir destinationTemporary(QStringLiteral("/dev/shm/rfm-dnd-XXXXXX"));
    if (!sourceTemporary.isValid() || !destinationTemporary.isValid()) {
        QSKIP("Could not create temporary directories on two local filesystems.");
    }
    const QString sourceDirectory = sourceTemporary.filePath(QStringLiteral("source"));
    const QString destinationDirectory =
        destinationTemporary.filePath(QStringLiteral("destination"));
    QVERIFY(QDir().mkpath(sourceDirectory));
    QVERIFY(QDir().mkpath(destinationDirectory));
    const QString sourcePath = QDir(sourceDirectory).filePath(QStringLiteral("a.txt"));
    QFile sourceFile(sourcePath);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    sourceFile.close();
    const QString destinationPath = QDir(destinationDirectory).filePath(QStringLiteral("a.txt"));
    if (!rfm::core::LocalFileSystem::pathsUseDifferentFileSystems(sourcePath, destinationPath)) {
        QSKIP("The temporary directories are not on confirmed distinct filesystems.");
    }

    rfm::app::MainWindow window;
    showLocalSplit(window, sourceDirectory, destinationDirectory,
                   {{QStringLiteral("a.txt"), 0, {}, false, false}}, {});
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    auto* const sourcePane = workspace->primaryPane();
    auto* const destinationPane = workspace->otherVisiblePane();
    QVERIFY(destinationPane != nullptr);
    sourcePane->fileTable()->selectRow(0);

    QObject::disconnect(&window, &rfm::app::MainWindow::localFileOperationRequested, nullptr,
                        nullptr);
    QSignalSpy requests(&window, &rfm::app::MainWindow::localFileOperationRequested);
    QMimeData mime;
    mime.setData(rfm::core::InternalTransferMimeType, sourcePane->createInternalDragData());
    const QPoint background(10, destinationPane->fileTable()->viewport()->height() - 2);
    QTimer::singleShot(0, [] {
        auto* const messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        QVERIFY(messageBox != nullptr);
        auto* const moveButton = messageBox->findChild<QAbstractButton*>(
            QStringLiteral("crossFilesystemDropMoveButton"));
        QVERIFY(moveButton != nullptr);
        moveButton->click();
    });
    QDropEvent drop(QPointF(background), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(destinationPane->fileTable()->viewport(), &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(drop.dropAction(), Qt::CopyAction);
    QCOMPARE(requests.size(), 1);
    const auto request =
        qvariant_cast<rfm::core::LocalFileOperationRequest>(requests.constFirst().constFirst());
    QCOMPARE(request.kind, rfm::core::LocalFileOperationKind::Move);
    QCOMPARE(request.sourcePaths, QStringList{sourcePath});
    QCOMPARE(request.destinationDirectory, destinationDirectory);
#endif
}

void MainWindowTest::localToSshDragDropQueuesExistingUploadsAndRejectsMove()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString sourceDirectory = QDir(temporary.path()).filePath(QStringLiteral("source"));
    QVERIFY(QDir().mkpath(sourceDirectory));
    for (const QString& name : {QStringLiteral("a.txt"), QStringLiteral("folder")}) {
        const QString path = QDir(sourceDirectory).filePath(name);
        if (name == QStringLiteral("folder")) {
            QVERIFY(QDir().mkpath(path));
        } else {
            QFile sourceFile(path);
            QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        }
    }

    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "showRemoteDirectory", Qt::DirectConnection,
        Q_ARG(QString, QStringLiteral("/uploads")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    auto* const sourcePane = workspace->primaryPane();
    const rfm::core::BrowserLocation remoteDestination = sourcePane->currentLocation();
    workspace->setSplit(true);
    auto* const destinationPane = workspace->otherVisiblePane();
    QVERIFY(destinationPane != nullptr);
    sourcePane->showDirectory(
        {rfm::core::FileSource::Local, rfm::core::LocalMachineId, sourceDirectory}, sourceDirectory,
        {{QStringLiteral("a.txt"), 0, {}, false, false},
         {QStringLiteral("folder"), 0, {}, true, false}},
        rfm::app::PaneNavigation::Initial);
    destinationPane->showDirectory(remoteDestination, QStringLiteral("/uploads"), {});
    sourcePane->fileTable()->selectAll();
    const auto localPayload =
        rfm::core::decodeInternalTransfer(sourcePane->createInternalDragData());
    QVERIFY(localPayload.has_value());
    QCOMPARE(localPayload->sources.size(), 2);
    sourcePane->fileTable()->clearSelection();
    const quint64 destinationPaneId = workspace->paneId(destinationPane);
    QObject::disconnect(&window, &rfm::app::MainWindow::transferRequested, nullptr, nullptr);
    QSignalSpy transfers(&window, &rfm::app::MainWindow::transferRequested);
    QSignalSpy removes(&window, &rfm::app::MainWindow::removeRequested);
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleInternalDrop", Qt::DirectConnection,
        Q_ARG(rfm::core::InternalTransferPayload, *localPayload),
        Q_ARG(rfm::core::InternalTransferAction, rfm::core::InternalTransferAction::Copy),
        Q_ARG(quint64, destinationPaneId), Q_ARG(QString, QStringLiteral("/uploads")),
        Q_ARG(bool, false)));
    QVERIFY(QApplication::activeModalWidget() == nullptr);
    QCOMPARE(transfers.size(), 2);
    QHash<QString, rfm::core::TransferRequest> uploads;
    for (const QList<QVariant>& arguments : transfers) {
        const auto request = arguments.constFirst().value<rfm::core::TransferRequest>();
        QCOMPARE(request.direction, rfm::core::TransferDirection::Upload);
        uploads.insert(request.source, request);
    }
    QCOMPARE(uploads.value(QDir(sourceDirectory).filePath(QStringLiteral("a.txt"))).destination,
             QStringLiteral("/uploads/a.txt"));
    QCOMPARE(uploads.value(QDir(sourceDirectory).filePath(QStringLiteral("folder"))).destination,
             QStringLiteral("/uploads/folder"));
    QVERIFY(uploads.value(QDir(sourceDirectory).filePath(QStringLiteral("folder"))).directory);

    auto* const debounce = window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"));
    QVERIFY(debounce != nullptr);
    const auto firstUpload = uploads.value(QDir(sourceDirectory).filePath(QStringLiteral("a.txt")));
    auto uploadCompleted = progress(firstUpload.id, rfm::core::TransferState::Completed, 1, 1);
    uploadCompleted.source = firstUpload.source;
    uploadCompleted.destination = firstUpload.destination;
    uploadCompleted.direction = firstUpload.direction;
    window.statusBar()->showMessage(QStringLiteral("Transfer completed"));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, uploadCompleted)));
    QVERIFY(debounce->isActive());
    debounce->stop();
    QVERIFY(QMetaObject::invokeMethod(debounce, "timeout", Qt::DirectConnection));
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Transfer completed"));

    destinationPane->showDirectory(
        {rfm::core::FileSource::Ssh, remoteDestination.machineId, QStringLiteral("/elsewhere")},
        QStringLiteral("/elsewhere"), {});
    const auto secondUpload =
        uploads.value(QDir(sourceDirectory).filePath(QStringLiteral("folder")));
    auto uploadAfterNavigation =
        progress(secondUpload.id, rfm::core::TransferState::Completed, 1, 1);
    uploadAfterNavigation.source = secondUpload.source;
    uploadAfterNavigation.destination = secondUpload.destination;
    uploadAfterNavigation.direction = secondUpload.direction;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, uploadAfterNavigation)));
    QVERIFY(!debounce->isActive());

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleInternalDrop", Qt::DirectConnection,
        Q_ARG(rfm::core::InternalTransferPayload, *localPayload),
        Q_ARG(rfm::core::InternalTransferAction, rfm::core::InternalTransferAction::Move),
        Q_ARG(quint64, destinationPaneId), Q_ARG(QString, QStringLiteral("/uploads")),
        Q_ARG(bool, true)));
    QCOMPARE(transfers.size(), 2);
    QCOMPARE(removes.size(), 0);
    QVERIFY(QFileInfo(QDir(sourceDirectory).filePath(QStringLiteral("a.txt"))).exists());
    QVERIFY(QFileInfo(QDir(sourceDirectory).filePath(QStringLiteral("folder"))).isDir());
}

void MainWindowTest::sshToLocalDragDropQueuesExistingDownloadsAndRejectsStaleSessions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString destinationDirectory =
        QDir(temporary.path()).filePath(QStringLiteral("downloads"));
    QVERIFY(QDir().mkpath(destinationDirectory));
    const QList<rfm::core::RemoteEntry> sourceEntries{
        {QStringLiteral("a.txt"), 1, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false},
    };

    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, sourceEntries)));
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    auto* const sourcePane = workspace->primaryPane();
    workspace->setSplit(true);
    auto* const destinationPane = workspace->otherVisiblePane();
    QVERIFY(destinationPane != nullptr);
    destinationPane->showDirectory(
        {rfm::core::FileSource::Local, rfm::core::LocalMachineId, destinationDirectory},
        destinationDirectory, {});
    sourcePane->fileTable()->selectAll();
    const auto sshPayload = rfm::core::decodeInternalTransfer(sourcePane->createInternalDragData());
    QVERIFY(sshPayload.has_value());
    QCOMPARE(sshPayload->source, rfm::core::FileSource::Ssh);
    QCOMPARE(sshPayload->sources.size(), 2);
    const quint64 destinationPaneId = workspace->paneId(destinationPane);
    QObject::disconnect(&window, &rfm::app::MainWindow::transferRequested, nullptr, nullptr);
    QSignalSpy transfers(&window, &rfm::app::MainWindow::transferRequested);
    QSignalSpy removes(&window, &rfm::app::MainWindow::removeRequested);

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleInternalDrop", Qt::DirectConnection,
        Q_ARG(rfm::core::InternalTransferPayload, *sshPayload),
        Q_ARG(rfm::core::InternalTransferAction, rfm::core::InternalTransferAction::Move),
        Q_ARG(quint64, destinationPaneId), Q_ARG(QString, destinationDirectory),
        Q_ARG(bool, true)));
    QCOMPARE(transfers.size(), 0);
    QCOMPARE(removes.size(), 0);

    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleInternalDrop", Qt::DirectConnection,
        Q_ARG(rfm::core::InternalTransferPayload, *sshPayload),
        Q_ARG(rfm::core::InternalTransferAction, rfm::core::InternalTransferAction::Copy),
        Q_ARG(quint64, destinationPaneId), Q_ARG(QString, destinationDirectory),
        Q_ARG(bool, false)));
    QVERIFY(QApplication::activeModalWidget() == nullptr);
    QCOMPARE(transfers.size(), 2);
    QHash<QString, rfm::core::TransferRequest> downloads;
    for (const QList<QVariant>& arguments : transfers) {
        const auto request = arguments.constFirst().value<rfm::core::TransferRequest>();
        QCOMPARE(request.direction, rfm::core::TransferDirection::Download);
        downloads.insert(request.source, request);
    }
    QCOMPARE(downloads.value(QStringLiteral("/source/a.txt")).destination,
             QDir(destinationDirectory).filePath(QStringLiteral("a.txt")));
    QCOMPARE(downloads.value(QStringLiteral("/source/folder")).destination,
             QDir(destinationDirectory).filePath(QStringLiteral("folder")));

    QObject::disconnect(&window, &rfm::app::MainWindow::localDirectoryRequested, nullptr, nullptr);
    QSignalSpy localRefreshes(&window, &rfm::app::MainWindow::localDirectoryRequested);
    auto* const debounce = window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"));
    QVERIFY(debounce != nullptr);
    const auto firstDownload = downloads.value(QStringLiteral("/source/a.txt"));
    auto downloadCompleted = progress(firstDownload.id, rfm::core::TransferState::Completed, 1, 1);
    downloadCompleted.source = firstDownload.source;
    downloadCompleted.destination = firstDownload.destination;
    downloadCompleted.direction = firstDownload.direction;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, downloadCompleted)));
    QVERIFY(debounce->isActive());
    debounce->stop();
    QVERIFY(QMetaObject::invokeMethod(debounce, "timeout", Qt::DirectConnection));
    QCOMPARE(localRefreshes.size(), 1);
    QCOMPARE(localRefreshes.constFirst().at(1).toString(), destinationDirectory);

    localRefreshes.clear();
    const QString otherDirectory = QDir(temporary.path()).filePath(QStringLiteral("other"));
    QVERIFY(QDir().mkpath(otherDirectory));
    const auto secondDownload = downloads.value(QStringLiteral("/source/folder"));
    auto downloadAfterNavigation =
        progress(secondDownload.id, rfm::core::TransferState::Completed, 1, 1);
    downloadAfterNavigation.source = secondDownload.source;
    downloadAfterNavigation.destination = secondDownload.destination;
    downloadAfterNavigation.direction = secondDownload.direction;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, downloadAfterNavigation)));
    QVERIFY(debounce->isActive());
    destinationPane->showDirectory(
        {rfm::core::FileSource::Local, rfm::core::LocalMachineId, otherDirectory}, otherDirectory,
        {});
    debounce->stop();
    QVERIFY(QMetaObject::invokeMethod(debounce, "timeout", Qt::DirectConnection));
    QCOMPARE(localRefreshes.size(), 0);

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleInternalDrop", Qt::DirectConnection,
        Q_ARG(rfm::core::InternalTransferPayload, *sshPayload),
        Q_ARG(rfm::core::InternalTransferAction, rfm::core::InternalTransferAction::Copy),
        Q_ARG(quint64, destinationPaneId), Q_ARG(QString, destinationDirectory),
        Q_ARG(bool, true)));
    QCOMPARE(transfers.size(), 2);
    QCOMPARE(removes.size(), 0);
}

void MainWindowTest::copyToOtherPaneQueuesCrossSourceTransfers()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString localDirectory = QDir(temporary.path()).filePath(QStringLiteral("local"));
    QVERIFY(QDir().mkpath(localDirectory));
    const QString localFilePath = QDir(localDirectory).filePath(QStringLiteral("local.txt"));
    QFile localFile(localFilePath);
    QVERIFY(localFile.open(QIODevice::WriteOnly));
    localFile.close();
    const QList<rfm::core::RemoteEntry> remoteEntries{
        {QStringLiteral("remote.txt"), 1, {}, false, false}};

    rfm::app::MainWindow window;
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, remoteEntries)));
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    QVERIFY(workspace != nullptr);
    auto* const sourcePane = workspace->primaryPane();
    const rfm::core::BrowserLocation remoteLocation = sourcePane->currentLocation();
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy listings(&window, &rfm::app::MainWindow::directoryRequested);
    workspace->setSplit(true);
    auto* const destinationPane = workspace->otherVisiblePane();
    QVERIFY(destinationPane != nullptr);
    QVERIFY(!listings.isEmpty());
    const quint64 initialListingId = listings.constLast().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDirectoryListed", Qt::DirectConnection,
                                      Q_ARG(quint64, initialListingId),
                                      Q_ARG(QString, QStringLiteral("/source")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, remoteEntries)));
    destinationPane->showDirectory(
        {rfm::core::FileSource::Local, rfm::core::LocalMachineId, localDirectory}, localDirectory,
        {});
    QVERIFY(QMetaObject::invokeMethod(sourcePane, "activated", Qt::DirectConnection));
    sourcePane->fileTable()->selectRow(0);
    QCOMPARE(workspace->activePane(), sourcePane);
    QCOMPARE(sourcePane->selectedEntries().size(), 1);
    auto* const copyAction = window.findChild<QAction*>(QStringLiteral("copyToOtherPaneAction"));
    auto* const moveAction = window.findChild<QAction*>(QStringLiteral("moveToOtherPaneAction"));
    QVERIFY(copyAction != nullptr);
    QVERIFY(moveAction != nullptr);
    QVERIFY(copyAction->isEnabled());
    QVERIFY(!moveAction->isEnabled());
    QSignalSpy transfers(&window, &rfm::app::MainWindow::transferRequested);
    copyAction->trigger();
    QCOMPARE(transfers.size(), 1);
    const auto download = transfers.constFirst().constFirst().value<rfm::core::TransferRequest>();
    QCOMPARE(download.direction, rfm::core::TransferDirection::Download);
    QCOMPARE(download.source, QStringLiteral("/source/remote.txt"));
    QCOMPARE(download.destination, QDir(localDirectory).filePath(QStringLiteral("remote.txt")));

    sourcePane->showDirectory(
        {rfm::core::FileSource::Local, rfm::core::LocalMachineId, localDirectory}, localDirectory,
        {{QStringLiteral("local.txt"), 1, {}, false, false}});
    destinationPane->showDirectory(
        {rfm::core::FileSource::Ssh, remoteLocation.machineId, QStringLiteral("/uploads")},
        QStringLiteral("/uploads"), {});
    QVERIFY(QMetaObject::invokeMethod(sourcePane, "activated", Qt::DirectConnection));
    sourcePane->fileTable()->selectRow(0);
    QVERIFY(copyAction->isEnabled());
    QVERIFY(!moveAction->isEnabled());
    copyAction->trigger();
    QCOMPARE(transfers.size(), 2);
    const auto upload = transfers.at(1).constFirst().value<rfm::core::TransferRequest>();
    QCOMPARE(upload.direction, rfm::core::TransferDirection::Upload);
    QCOMPARE(upload.source, localFilePath);
    QCOMPARE(upload.destination, QStringLiteral("/uploads/local.txt"));
}

void MainWindowTest::keyboardActionsExposeShortcutsAndTargetTheActivePane()
{
    rfm::app::MainWindow window;
    window.show();
    const QList<rfm::core::RemoteEntry> entries{{QStringLiteral("file.txt"), 1, {}, false, false},
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
    QCOMPARE(action(QStringLiteral("switchPaneAction"))->shortcut(), QKeySequence(Qt::Key_F6));

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
        renameId,
        rfm::core::RemoteOperationKind::Rename,
        {{QStringLiteral("/one/two/file.txt"), QStringLiteral("/one/two/renamed.txt"), true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleOperationResult", Qt::DirectConnection,
                                      Q_ARG(rfm::core::RemoteOperationResult, renameResult)));

    QObject::disconnect(&window, &rfm::app::MainWindow::removeRequested, nullptr, nullptr);
    QSignalSpy removals(&window, &rfm::app::MainWindow::removeRequested);
    acceptNextQuestion();
    action(QStringLiteral("removeAction"))->trigger();
    QCOMPARE(removals.size(), 1);
    const quint64 removeId = removals.constFirst().constFirst().toULongLong();
    const rfm::core::RemoteOperationResult removeResult{
        removeId,
        rfm::core::RemoteOperationKind::Remove,
        {{QStringLiteral("/one/two/file.txt"), {}, true, {}}}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleOperationResult", Qt::DirectConnection,
                                      Q_ARG(rfm::core::RemoteOperationResult, removeResult)));

    QSignalSpy navigation(workspace->activePane(), &rfm::app::FileBrowserPane::navigationRequested);
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
        Q_ARG(QString, QStringLiteral("/other")), Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    auto* const firstPane = workspace->activePane();
    action(QStringLiteral("switchPaneAction"))->trigger();
    QVERIFY(workspace->activePane() != firstPane);
}

void MainWindowTest::volumeOperationSuccessWaitsForSystemRefresh()
{
    rfm::app::MainWindow window(
        nullptr, {}, {},
        std::make_unique<FixedVolumeService>(rfm::core::VolumeOperationError::None));
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const mountButton = window.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    QVERIFY(navigation != nullptr);
    QVERIFY(mountButton != nullptr);
    waitForInitialLocalStorageRefresh(window);
    QObject::disconnect(&window, &rfm::app::MainWindow::localVolumesRequested, nullptr, nullptr);

    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("USB fixture");
    volume.device = QStringLiteral("/dev/sde1");
    volume.fileSystemType = QByteArrayLiteral("vfat");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = false;
    navigation->setStorageVolumes({volume});
    QTreeWidgetItem* const external =
        childNamed(localPlacesRoot(navigation->tree()), QStringLiteral("External devices"));
    QVERIFY(external != nullptr);
    navigation->tree()->setCurrentItem(external->child(0));

    QSignalSpy operations(&window, &rfm::app::MainWindow::volumeOperationRequested);
    QSignalSpy refreshes(&window, &rfm::app::MainWindow::localVolumesRequested);
    mountButton->click();
    QCOMPARE(operations.size(), 1);
    QTRY_COMPARE(refreshes.size(), 1);
    QVERIFY(external->child(0)->text(0).contains(QStringLiteral("Mounting")));
    QVERIFY(!mountButton->isEnabled());
    const auto beforeRefresh = navigation->selectedLocalStorageVolume();
    QVERIFY(beforeRefresh.has_value());
    QVERIFY(!beforeRefresh->mounted);
    QVERIFY(beforeRefresh->rootPath.isEmpty());

    const QList<rfm::core::StorageVolume> observedVolumes{volume};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleLocalStorageVolumes", Qt::DirectConnection,
                                      Q_ARG(QList<rfm::core::StorageVolume>, observedVolumes)));
    QVERIFY(mountButton->isEnabled());
    QVERIFY(!external->child(0)->text(0).contains(QStringLiteral("Mounting")));
    const auto afterRefresh = navigation->selectedLocalStorageVolume();
    QVERIFY(afterRefresh.has_value());
    QVERIFY(!afterRefresh->mounted);
}

void MainWindowTest::volumeOperationErrorsRestoreUi_data()
{
    QTest::addColumn<rfm::core::VolumeOperationError>("error");
    QTest::addColumn<QString>("expectedMessage");
    QTest::addColumn<bool>("refreshExpected");

    QTest::newRow("permission") << rfm::core::VolumeOperationError::PermissionDenied
                                << QStringLiteral("Permission was denied") << false;
    QTest::newRow("busy") << rfm::core::VolumeOperationError::VolumeBusy
                          << QStringLiteral("is busy") << false;
    QTest::newRow("disappeared") << rfm::core::VolumeOperationError::DeviceNotFound
                                 << QStringLiteral("no longer available") << true;
    QTest::newRow("tool") << rfm::core::VolumeOperationError::ToolUnavailable
                          << QStringLiteral("No supported system tool") << false;
    QTest::newRow("system") << rfm::core::VolumeOperationError::SystemError
                            << QStringLiteral("system could not") << false;
}

void MainWindowTest::volumeOperationErrorsRestoreUi()
{
    QFETCH(rfm::core::VolumeOperationError, error);
    QFETCH(QString, expectedMessage);
    QFETCH(bool, refreshExpected);
    rfm::app::MainWindow window(nullptr, {}, {}, std::make_unique<FixedVolumeService>(error));
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const mountButton = window.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    QVERIFY(navigation != nullptr);
    QVERIFY(mountButton != nullptr);
    waitForInitialLocalStorageRefresh(window);
    QObject::disconnect(&window, &rfm::app::MainWindow::localVolumesRequested, nullptr, nullptr);

    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("USB fixture");
    volume.device = QStringLiteral("/dev/sde1");
    volume.fileSystemType = QByteArrayLiteral("vfat");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = false;
    navigation->setStorageVolumes({volume});
    QTreeWidgetItem* const external =
        childNamed(localPlacesRoot(navigation->tree()), QStringLiteral("External devices"));
    QVERIFY(external != nullptr);
    navigation->tree()->setCurrentItem(external->child(0));
    QSignalSpy refreshes(&window, &rfm::app::MainWindow::localVolumesRequested);

    mountButton->click();

    QTRY_VERIFY(window.statusBar()->currentMessage().contains(expectedMessage));
    QVERIFY(!window.statusBar()->currentMessage().contains(QStringLiteral("technical fixture")));
    QVERIFY(mountButton->isEnabled());
    QCOMPARE(refreshes.size(), refreshExpected ? 1 : 0);
}

void MainWindowTest::closingWindowCancelsBusyLocalVolumeWorker()
{
    auto state = std::make_shared<BlockingVolumeServiceState>();
    auto* const window =
        new rfm::app::MainWindow(nullptr, {}, {}, std::make_unique<BlockingVolumeService>(state));
    auto* const navigation = window->findChild<rfm::app::NavigationTree*>();
    auto* const mountButton = window->findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    QVERIFY(navigation != nullptr);
    QVERIFY(mountButton != nullptr);

    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("Blocking fixture");
    volume.device = QStringLiteral("/dev/sde1");
    volume.fileSystemType = QByteArrayLiteral("ext4");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = false;
    navigation->setStorageVolumes({volume});
    QTreeWidgetItem* const external =
        childNamed(localPlacesRoot(navigation->tree()), QStringLiteral("External devices"));
    QVERIFY(external != nullptr);
    navigation->tree()->setCurrentItem(external->child(0));
    mountButton->click();

    {
        std::unique_lock lock(state->mutex);
        QVERIFY(state->condition.wait_for(lock, std::chrono::seconds(2),
                                          [&state] { return state->entered; }));
    }
    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    delete window;

    QVERIFY(shutdownTimer.elapsed() < 1000);
    std::lock_guard lock(state->mutex);
    QVERIFY(state->cancellationRequested);
    QVERIFY(state->completed);
}

void MainWindowTest::successfulUnmountEvacuatesOnlyAffectedPanes_data()
{
    QTest::addColumn<rfm::core::FileSource>("primarySource");
    QTest::addColumn<QString>("primaryPath");
    QTest::addColumn<bool>("split");
    QTest::addColumn<rfm::core::FileSource>("secondarySource");
    QTest::addColumn<QString>("secondaryPath");
    QTest::addColumn<bool>("primaryEvacuated");
    QTest::addColumn<bool>("secondaryEvacuated");

    QTest::newRow("single-exact") << rfm::core::FileSource::Local << QStringLiteral("/mnt/disk")
                                  << false << rfm::core::FileSource::Local << QString{} << true
                                  << false;
    QTest::newRow("left-descendant-right-similar-prefix")
        << rfm::core::FileSource::Local << QStringLiteral("/mnt/disk/Films/Action") << true
        << rfm::core::FileSource::Local << QStringLiteral("/mnt/disk2") << true << false;
    QTest::newRow("right-descendant")
        << rfm::core::FileSource::Local << QStringLiteral("/tmp/elsewhere") << true
        << rfm::core::FileSource::Local << QStringLiteral("/mnt/disk/Films") << false << true;
    QTest::newRow("both") << rfm::core::FileSource::Local << QStringLiteral("/mnt/disk") << true
                          << rfm::core::FileSource::Local << QStringLiteral("/mnt/disk/Films")
                          << true << true;
    QTest::newRow("neither") << rfm::core::FileSource::Local << QStringLiteral("/tmp/elsewhere")
                             << true << rfm::core::FileSource::Local << QStringLiteral("/mnt/disk2")
                             << false << false;
    QTest::newRow("remote-identical-path")
        << rfm::core::FileSource::Ssh << QStringLiteral("/mnt/disk/Films") << true
        << rfm::core::FileSource::Local << QStringLiteral("/tmp/elsewhere") << false << false;
}

void MainWindowTest::successfulUnmountEvacuatesOnlyAffectedPanes()
{
    QFETCH(rfm::core::FileSource, primarySource);
    QFETCH(QString, primaryPath);
    QFETCH(bool, split);
    QFETCH(rfm::core::FileSource, secondarySource);
    QFETCH(QString, secondaryPath);
    QFETCH(bool, primaryEvacuated);
    QFETCH(bool, secondaryEvacuated);

    rfm::app::MainWindow window(
        nullptr, {}, {},
        std::make_unique<FixedVolumeService>(rfm::core::VolumeOperationError::None));
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const unmountButton =
        window.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"));
    QVERIFY(workspace != nullptr);
    QVERIFY(navigation != nullptr);
    QVERIFY(unmountButton != nullptr);
    waitForInitialLocalStorageRefresh(window);
    QObject::disconnect(&window, &rfm::app::MainWindow::localVolumesRequested, nullptr, nullptr);
    if (split) {
        workspace->setSplit(true);
    }
    showPaneLocation(workspace->primaryPane(), primarySource, primaryPath);
    if (split) {
        showPaneLocation(workspace->otherVisiblePane(workspace->paneId(workspace->primaryPane())),
                         secondarySource, secondaryPath);
    }

    QObject::disconnect(&window, &rfm::app::MainWindow::localDirectoryRequested, nullptr, nullptr);
    QSignalSpy directoryRequests(&window, &rfm::app::MainWindow::localDirectoryRequested);
    QSignalSpy volumeRefreshes(&window, &rfm::app::MainWindow::localVolumesRequested);

    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("Unmount fixture");
    volume.device = QStringLiteral("/dev/sde1");
    volume.rootPath = QStringLiteral("/mnt/disk");
    volume.fileSystemType = QByteArrayLiteral("vfat");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = true;
    navigation->setStorageVolumes({volume});
    QTreeWidgetItem* const external =
        childNamed(localPlacesRoot(navigation->tree()), QStringLiteral("External devices"));
    QVERIFY(external != nullptr);
    navigation->tree()->setCurrentItem(external->child(0));

    unmountButton->click();

    const int expectedEvacuations =
        static_cast<int>(primaryEvacuated) + static_cast<int>(secondaryEvacuated);
    QTRY_COMPARE(directoryRequests.size(), expectedEvacuations);
    QTRY_COMPARE(volumeRefreshes.size(), 1);
    for (const QList<QVariant>& arguments : std::as_const(directoryRequests)) {
        const quint64 requestId = arguments.at(0).toULongLong();
        const QString fallbackPath = arguments.at(1).toString();
        QCOMPARE(fallbackPath, QDir::homePath());
        QVERIFY(QMetaObject::invokeMethod(
            &window, "handleLocalDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, requestId),
            Q_ARG(QString, fallbackPath),
            Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    }

    QCOMPARE(workspace->primaryPane()->currentPath(),
             primaryEvacuated ? QDir::homePath() : primaryPath);
    QCOMPARE(workspace->primaryPane()->source(), primarySource);
    if (split) {
        rfm::app::FileBrowserPane* const secondary =
            workspace->otherVisiblePane(workspace->paneId(workspace->primaryPane()));
        QCOMPARE(secondary->currentPath(), secondaryEvacuated ? QDir::homePath() : secondaryPath);
        QCOMPARE(secondary->source(), secondarySource);
    }
}

void MainWindowTest::failedUnmountDoesNotEvacuatePane()
{
    rfm::app::MainWindow window(
        nullptr, {}, {},
        std::make_unique<FixedVolumeService>(rfm::core::VolumeOperationError::PermissionDenied));
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const unmountButton =
        window.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"));
    QVERIFY(workspace != nullptr);
    QVERIFY(navigation != nullptr);
    QVERIFY(unmountButton != nullptr);
    waitForInitialLocalStorageRefresh(window);
    QObject::disconnect(&window, &rfm::app::MainWindow::localVolumesRequested, nullptr, nullptr);
    const QString currentPath = QStringLiteral("/mnt/disk/Films");
    showPaneLocation(workspace->primaryPane(), rfm::core::FileSource::Local, currentPath);
    QObject::disconnect(&window, &rfm::app::MainWindow::localDirectoryRequested, nullptr, nullptr);
    QSignalSpy directoryRequests(&window, &rfm::app::MainWindow::localDirectoryRequested);
    QSignalSpy volumeRefreshes(&window, &rfm::app::MainWindow::localVolumesRequested);

    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("Unmount fixture");
    volume.device = QStringLiteral("/dev/sde1");
    volume.rootPath = QStringLiteral("/mnt/disk");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = true;
    navigation->setStorageVolumes({volume});
    QTreeWidgetItem* const external =
        childNamed(localPlacesRoot(navigation->tree()), QStringLiteral("External devices"));
    navigation->tree()->setCurrentItem(external->child(0));

    unmountButton->click();

    QTRY_VERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Permission")));
    QCOMPARE(workspace->primaryPane()->currentPath(), currentPath);
    QCOMPARE(directoryRequests.size(), 0);
    QCOMPARE(volumeRefreshes.size(), 0);
}

void MainWindowTest::safetyFallbackPurgesUnmountedPathsFromHistory()
{
    rfm::app::MainWindow window(
        nullptr, {}, {},
        std::make_unique<FixedVolumeService>(rfm::core::VolumeOperationError::None));
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const unmountButton =
        window.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"));
    QVERIFY(workspace != nullptr);
    QVERIFY(navigation != nullptr);
    QVERIFY(unmountButton != nullptr);
    waitForInitialLocalStorageRefresh(window);
    QObject::disconnect(&window, &rfm::app::MainWindow::localVolumesRequested, nullptr, nullptr);
    rfm::app::FileBrowserPane* const pane = workspace->primaryPane();
    showPaneLocation(pane, rfm::core::FileSource::Local, QStringLiteral("/mnt/disk/Old"));
    showPaneLocation(pane, rfm::core::FileSource::Local, QStringLiteral("/tmp/safe"),
                     rfm::app::PaneNavigation::Normal);
    showPaneLocation(pane, rfm::core::FileSource::Local, QStringLiteral("/mnt/disk/Films"),
                     rfm::app::PaneNavigation::Normal);
    QObject::disconnect(&window, &rfm::app::MainWindow::localDirectoryRequested, nullptr, nullptr);
    QSignalSpy directoryRequests(&window, &rfm::app::MainWindow::localDirectoryRequested);

    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("Unmount fixture");
    volume.device = QStringLiteral("/dev/sde1");
    volume.rootPath = QStringLiteral("/mnt/disk");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = true;
    navigation->setStorageVolumes({volume});
    QTreeWidgetItem* const external =
        childNamed(localPlacesRoot(navigation->tree()), QStringLiteral("External devices"));
    navigation->tree()->setCurrentItem(external->child(0));

    unmountButton->click();
    QTRY_COMPARE(directoryRequests.size(), 1);
    const quint64 fallbackRequestId = directoryRequests.constFirst().at(0).toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleLocalDirectoryListed", Qt::DirectConnection,
        Q_ARG(quint64, fallbackRequestId), Q_ARG(QString, QDir::homePath()),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QCOMPARE(pane->currentPath(), QDir::homePath());
    QVERIFY(pane->canGoBack());

    pane->requestBack();

    QCOMPARE(directoryRequests.size(), 2);
    QCOMPARE(directoryRequests.constLast().at(1).toString(), QStringLiteral("/tmp/safe"));
    QVERIFY(!rfm::core::localPathIsAtOrBelow(directoryRequests.constLast().at(1).toString(),
                                             QStringLiteral("/mnt/disk")));
}

void MainWindowTest::remoteMountWaitsForRefreshAndOpensObservedMountPoint()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                        nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
    QSignalSpy directoryRequests(&window, &rfm::app::MainWindow::directoryRequested);
    setConnectionIdentity(window, QStringLiteral("volumes.example.test"), QStringLiteral("alice"),
                          QStringLiteral("/home/alice"));
    QCOMPARE(storageRequests.size(), 1);
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const mountButton = window.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    auto* const openButton = window.findChild<QPushButton*>(QStringLiteral("openVolumeButton"));
    QVERIFY(navigation != nullptr);
    QVERIFY(mountButton != nullptr);
    QVERIFY(openButton != nullptr);

    rfm::core::StorageVolume available;
    available.displayName = QStringLiteral("Remote USB");
    available.device = QStringLiteral("/dev/sdb1");
    available.fileSystemType = QByteArrayLiteral("ext4");
    available.kind = rfm::core::StorageKind::External;
    available.mounted = false;
    const quint64 initialRefresh = storageRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, initialRefresh),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{available})));
    QTreeWidgetItem* item =
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), available.device);
    QVERIFY(item != nullptr);
    navigation->tree()->setCurrentItem(item);
    mountButton->click();
    QCOMPARE(operations.size(), 1);
    const rfm::core::VolumeOperationRequest request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    QVERIFY(!openButton->isVisibleTo(&window));
    QVERIFY(item->text(0).contains(QStringLiteral("Mounting")));

    const rfm::core::VolumeOperationResult success{request.id,
                                                   request.operation,
                                                   request.target.device,
                                                   rfm::core::VolumeOperationError::None,
                                                   {}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, success)));
    QCOMPARE(storageRequests.size(), 2);
    QVERIFY(!openButton->isVisibleTo(&window));

    rfm::core::StorageVolume mounted = available;
    mounted.rootPath = QStringLiteral("/run/media/alice/REMOTE_USB");
    mounted.mounted = true;
    const quint64 observedRefresh = storageRequests.constLast().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection,
        Q_ARG(quint64, observedRefresh),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{mounted})));
    item = volumeItemByDevice(remoteCategoryItem(navigation->tree()), mounted.device);
    QVERIFY(item != nullptr);
    navigation->tree()->setCurrentItem(item);
    QVERIFY(openButton->isEnabled());
    openButton->click();
    QCOMPARE(directoryRequests.size(), 1);
    QCOMPARE(directoryRequests.constFirst().at(1).toString(), mounted.rootPath);
}

void MainWindowTest::remoteAuthenticationDialogShowsContextAndCancelReleasesBusy()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                        nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
    QSignalSpy cancellations(&window, &rfm::app::MainWindow::remoteVolumeAuthenticationCancelled);
    setConnectionIdentity(window, QStringLiteral("auth.example.test"), QStringLiteral("alice"),
                          QStringLiteral("/home/alice"));
    QCOMPARE(storageRequests.size(), 1);
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const mountButton = window.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    QVERIFY(navigation != nullptr);

    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("Remote USB");
    volume.device = QStringLiteral("/dev/sdc1");
    volume.fileSystemType = QByteArrayLiteral("ext4");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = false;
    const quint64 refreshId = storageRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, refreshId),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{volume})));
    QTreeWidgetItem* const item =
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), volume.device);
    QVERIFY(item != nullptr);
    navigation->tree()->setCurrentItem(item);
    mountButton->click();
    QCOMPARE(operations.size(), 1);
    const auto request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    const rfm::core::VolumeOperationResult authRequired{
        request.id,
        request.operation,
        request.target.device,
        rfm::core::VolumeOperationError::AuthenticationRequired,
        {},
        41};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, authRequired)));

    auto* const dialog = window.findChild<rfm::app::VolumeAuthenticationDialog*>();
    QVERIFY(dialog != nullptr);
    QCOMPARE(dialog->windowModality(), Qt::WindowModal);
    auto* const password =
        dialog->findChild<QLineEdit*>(QStringLiteral("volumeAuthenticationPasswordEdit"));
    QVERIFY(password != nullptr);
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    QVERIFY(password->inputMethodHints().testFlag(Qt::ImhSensitiveData));
    QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("authenticationServerLabel"))->text(),
             QStringLiteral("auth.example.test"));
    QCOMPARE(dialog->findChild<QLabel*>(QStringLiteral("authenticationDeviceLabel"))->text(),
             volume.device);
    QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("authenticationExplanationLabel"))
                ->text()
                .contains(QStringLiteral("mount")));
    QVERIFY(item->text(0).contains(QStringLiteral("Mounting")));
    mountButton->click();
    QCOMPARE(operations.size(), 1);
    dialog->reject();
    QCOMPARE(cancellations.size(), 1);
    QVERIFY(!item->text(0).contains(QStringLiteral("Mounting")));
    QVERIFY(mountButton->isEnabled());
    QCOMPARE(storageRequests.size(), 1);
    QCOMPARE(operations.size(), 1);
}

void MainWindowTest::remoteAuthenticationSubmitsEphemeralPassword()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                        nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
    setConnectionIdentity(window, QStringLiteral("auth.example.test"), QStringLiteral("alice"),
                          QStringLiteral("/home/alice"));
    QCOMPARE(storageRequests.size(), 1);
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    QVERIFY(navigation != nullptr);
    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("Remote USB");
    volume.device = QStringLiteral("/dev/sdc1");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = false;
    const quint64 refreshId = storageRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, refreshId),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{volume})));
    QTreeWidgetItem* const item =
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), volume.device);
    QVERIFY(item != nullptr);
    navigation->tree()->setCurrentItem(item);
    window.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"))->click();
    QCOMPARE(operations.size(), 1);
    const auto request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    const rfm::core::VolumeOperationResult authRequired{
        request.id,
        request.operation,
        request.target.device,
        rfm::core::VolumeOperationError::AuthenticationRequired,
        {},
        93};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, authRequired)));
    auto* const dialog = window.findChild<rfm::app::VolumeAuthenticationDialog*>();
    QVERIFY(dialog != nullptr);
    auto* const password =
        dialog->findChild<QLineEdit*>(QStringLiteral("volumeAuthenticationPasswordEdit"));
    QVERIFY(password != nullptr);
    password->setText(QStringLiteral("one-use fixture"));
    QTest::keyClick(password, Qt::Key_Return);

    QVERIFY(password->text().isEmpty());
    QCOMPARE(storageRequests.size(), 1);

    const rfm::core::VolumeOperationResult success{request.id,
                                                   request.operation,
                                                   request.target.device,
                                                   rfm::core::VolumeOperationError::None,
                                                   {}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, success)));
    QCOMPARE(storageRequests.size(), 2);
    QVERIFY(item->text(0).contains(QStringLiteral("Mounting")));
}

void MainWindowTest::remoteInteractiveBusinessErrorsReleaseBusy_data()
{
    QTest::addColumn<rfm::core::VolumeOperationError>("error");
    QTest::addColumn<QString>("message");
    QTest::newRow("authentication-failed") << rfm::core::VolumeOperationError::AuthenticationFailed
                                           << QStringLiteral("Authentication failed");
    QTest::newRow("volume-busy") << rfm::core::VolumeOperationError::VolumeBusy
                                 << QStringLiteral("is busy");
}

void MainWindowTest::remoteInteractiveBusinessErrorsReleaseBusy()
{
    QFETCH(rfm::core::VolumeOperationError, error);
    QFETCH(QString, message);
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                        nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
    setConnectionIdentity(window, QStringLiteral("auth.example.test"), QStringLiteral("alice"));
    QCOMPARE(storageRequests.size(), 1);
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const mountButton = window.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    QVERIFY(navigation != nullptr);
    QVERIFY(mountButton != nullptr);
    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("Remote USB");
    volume.device = QStringLiteral("/dev/sdc1");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = false;
    const quint64 refreshId = storageRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, refreshId),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{volume})));
    QTreeWidgetItem* const item =
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), volume.device);
    QVERIFY(item != nullptr);
    navigation->tree()->setCurrentItem(item);
    mountButton->click();
    QCOMPARE(operations.size(), 1);
    const auto request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    const rfm::core::VolumeOperationResult authRequired{
        request.id,
        request.operation,
        request.target.device,
        rfm::core::VolumeOperationError::AuthenticationRequired,
        {},
        13};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, authRequired)));
    auto* const dialog = window.findChild<rfm::app::VolumeAuthenticationDialog*>();
    QVERIFY(dialog != nullptr);
    auto* const password =
        dialog->findChild<QLineEdit*>(QStringLiteral("volumeAuthenticationPasswordEdit"));
    QVERIFY(password != nullptr);
    password->setText(QStringLiteral("wrong fixture"));
    dialog->accept();
    QVERIFY(password->text().isEmpty());
    const rfm::core::VolumeOperationResult failure{
        request.id, request.operation, request.target.device, error, {}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, failure)));
    QVERIFY(!item->text(0).contains(QStringLiteral("Mounting")));
    QVERIFY(mountButton->isEnabled());
    QVERIFY(window.statusBar()->currentMessage().contains(message));
    QCOMPARE(storageRequests.size(), 1);
    mountButton->click();
    QCOMPARE(operations.size(), 2);
    QVERIFY(item->text(0).contains(QStringLiteral("Mounting")));
}

void MainWindowTest::disconnectClosesRemoteAuthenticationDialog()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                        nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
    setConnectionIdentity(window, QStringLiteral("auth.example.test"), QStringLiteral("alice"));
    QCOMPARE(storageRequests.size(), 1);
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    QVERIFY(navigation != nullptr);
    rfm::core::StorageVolume volume;
    volume.displayName = QStringLiteral("Remote USB");
    volume.device = QStringLiteral("/dev/sdc1");
    volume.kind = rfm::core::StorageKind::External;
    volume.mounted = false;
    const quint64 refreshId = storageRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, refreshId),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{volume})));
    QTreeWidgetItem* const item =
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), volume.device);
    QVERIFY(item != nullptr);
    navigation->tree()->setCurrentItem(item);
    window.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"))->click();
    QCOMPARE(operations.size(), 1);
    const auto request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    const rfm::core::VolumeOperationResult authRequired{
        request.id,
        request.operation,
        request.target.device,
        rfm::core::VolumeOperationError::AuthenticationRequired,
        {},
        7};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, authRequired)));
    QPointer<rfm::app::VolumeAuthenticationDialog> dialog =
        window.findChild<rfm::app::VolumeAuthenticationDialog*>();
    QVERIFY(dialog != nullptr);
    dialog->findChild<QLineEdit*>(QStringLiteral("volumeAuthenticationPasswordEdit"))
        ->setText(QStringLiteral("must not escape"));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QTRY_VERIFY(dialog == nullptr);
}

void MainWindowTest::failedRemoteUnmountDoesNotEvacuateOrRefresh()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                        nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
    QSignalSpy directoryRequests(&window, &rfm::app::MainWindow::directoryRequested);
    setConnectionIdentity(window, QStringLiteral("failure.example.test"), QStringLiteral("alice"),
                          QStringLiteral("/home/alice"));
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
    auto* const unmountButton =
        window.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"));
    QVERIFY(navigation != nullptr);
    QVERIFY(workspace != nullptr);
    rfm::core::StorageVolume mounted;
    mounted.displayName = QStringLiteral("Remote USB");
    mounted.device = QStringLiteral("/dev/sdb1");
    mounted.rootPath = QStringLiteral("/mnt/usb");
    mounted.fileSystemType = QByteArrayLiteral("ext4");
    mounted.kind = rfm::core::StorageKind::External;
    const quint64 initialRefresh = storageRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, initialRefresh),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{mounted})));
    const QString machineId =
        serverProfileItem(navigation->tree(), 0)->data(0, Qt::UserRole + 2).toString();
    workspace->primaryPane()->showDirectory(
        {rfm::core::FileSource::Ssh, machineId, QStringLiteral("/mnt/usb/Films")},
        QStringLiteral("/mnt/usb/Films"), {}, rfm::app::PaneNavigation::Initial);
    QTreeWidgetItem* const item =
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), mounted.device);
    QVERIFY(item != nullptr);
    navigation->tree()->setCurrentItem(item);
    unmountButton->click();
    QCOMPARE(operations.size(), 1);
    QVERIFY(item->text(0).contains(QStringLiteral("Unmounting")));
    QVERIFY(!unmountButton->isEnabled());
    const auto request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    const rfm::core::VolumeOperationResult failure{
        request.id, request.operation, request.target.device,
        rfm::core::VolumeOperationError::PermissionDenied, QStringLiteral("private detail")};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, failure)));
    QCOMPARE(directoryRequests.size(), 0);
    QCOMPARE(storageRequests.size(), 1);
    QCOMPARE(workspace->primaryPane()->currentPath(), QStringLiteral("/mnt/usb/Films"));
    QVERIFY(!item->text(0).contains(QStringLiteral("Unmounting")));
    QVERIFY(unmountButton->isEnabled());
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Permission")));
    QVERIFY(!window.statusBar()->currentMessage().contains(QStringLiteral("private detail")));
    QVERIFY(window.findChild<rfm::app::VolumeAuthenticationDialog*>() == nullptr);
}

void MainWindowTest::remoteTimeoutRestoresUiWithoutRefresh()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                        nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
    setConnectionIdentity(window, QStringLiteral("timeout.example.test"), QStringLiteral("alice"),
                          QStringLiteral("/home/alice"));
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const mountButton = window.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"));
    QVERIFY(navigation != nullptr);
    QVERIFY(mountButton != nullptr);

    rfm::core::StorageVolume available;
    available.displayName = QStringLiteral("Remote USB");
    available.device = QStringLiteral("/dev/sdb1");
    available.fileSystemType = QByteArrayLiteral("ext4");
    available.kind = rfm::core::StorageKind::External;
    available.mounted = false;
    const quint64 initialRefresh = storageRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, initialRefresh),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{available})));
    QTreeWidgetItem* const item =
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), available.device);
    QVERIFY(item != nullptr);
    navigation->tree()->setCurrentItem(item);

    mountButton->click();
    QCOMPARE(operations.size(), 1);
    QVERIFY(item->text(0).contains(QStringLiteral("Mounting")));
    QVERIFY(!mountButton->isEnabled());
    const auto request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    const rfm::core::VolumeOperationResult timeout{
        request.id, request.operation, request.target.device,
        rfm::core::VolumeOperationError::SystemError, QStringLiteral("private timeout detail")};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, timeout)));

    QCOMPARE(storageRequests.size(), 1);
    QVERIFY(!item->text(0).contains(QStringLiteral("Mounting")));
    QVERIFY(mountButton->isEnabled());
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("could not mount")));
    QVERIFY(
        !window.statusBar()->currentMessage().contains(QStringLiteral("private timeout detail")));
}

void MainWindowTest::successfulRemoteUnmountEvacuatesOnlyMatchingNamespace()
{
    auto runScenario = [](bool split, rfm::core::FileSource secondarySource,
                          const QString& secondaryMachineId, const QString& secondaryPath,
                          int expectedRequests) {
        rfm::app::MainWindow window;
        QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr,
                            nullptr);
        QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                            nullptr);
        QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
        QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
        QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
        QSignalSpy directoryRequests(&window, &rfm::app::MainWindow::directoryRequested);
        setConnectionIdentity(window, QStringLiteral("unmount.example.test"),
                              QStringLiteral("alice"), QStringLiteral("/home/alice"));
        auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
        auto* const workspace = window.findChild<rfm::app::PaneWorkspace*>();
        auto* const unmountButton =
            window.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"));
        if (split) {
            workspace->setSplit(true);
        }
        const QString machineId =
            serverProfileItem(navigation->tree(), 0)->data(0, Qt::UserRole + 2).toString();
        workspace->primaryPane()->showDirectory(
            {rfm::core::FileSource::Ssh, machineId, QStringLiteral("/mnt/usb/Films/Action")},
            QStringLiteral("/mnt/usb/Films/Action"), {}, rfm::app::PaneNavigation::Initial);
        if (split) {
            auto* const secondary =
                workspace->otherVisiblePane(workspace->paneId(workspace->primaryPane()));
            const QString resolvedMachine =
                secondaryMachineId == QStringLiteral("active") ? machineId : secondaryMachineId;
            secondary->showDirectory({secondarySource, resolvedMachine, secondaryPath},
                                     secondaryPath, {}, rfm::app::PaneNavigation::Initial);
        }
        rfm::core::StorageVolume mounted;
        mounted.displayName = QStringLiteral("Remote USB");
        mounted.device = QStringLiteral("/dev/sdb1");
        mounted.rootPath = QStringLiteral("/mnt/usb");
        mounted.fileSystemType = QByteArrayLiteral("ext4");
        mounted.kind = rfm::core::StorageKind::External;
        const quint64 initialRefresh = storageRequests.constFirst().constFirst().toULongLong();
        QVERIFY(QMetaObject::invokeMethod(
            &window, "handleRemoteStorageVolumes", Qt::DirectConnection,
            Q_ARG(quint64, initialRefresh),
            Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{mounted})));
        navigation->tree()->setCurrentItem(
            volumeItemByDevice(remoteCategoryItem(navigation->tree()), mounted.device));
        unmountButton->click();
        const auto request =
            operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
        const rfm::core::VolumeOperationResult success{request.id,
                                                       request.operation,
                                                       request.target.device,
                                                       rfm::core::VolumeOperationError::None,
                                                       {}};
        QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                          Qt::DirectConnection,
                                          Q_ARG(rfm::core::VolumeOperationResult, success)));
        while (directoryRequests.size() < expectedRequests && !directoryRequests.isEmpty()) {
            const QList<QVariant> arguments = directoryRequests.constLast();
            QVERIFY(QMetaObject::invokeMethod(
                &window, "handleDirectoryListed", Qt::DirectConnection,
                Q_ARG(quint64, arguments.at(0).toULongLong()),
                Q_ARG(QString, arguments.at(1).toString()),
                Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
        }
        QCOMPARE(directoryRequests.size(), expectedRequests);
        QCOMPARE(storageRequests.size(), 2);
        for (const QList<QVariant>& arguments : std::as_const(directoryRequests)) {
            QCOMPARE(arguments.at(1).toString(), QStringLiteral("/home/alice"));
        }
    };

    runScenario(true, rfm::core::FileSource::Ssh, QStringLiteral("active"),
                QStringLiteral("/mnt/usb"), 2);
    runScenario(true, rfm::core::FileSource::Local, QString::fromLatin1(rfm::core::LocalMachineId),
                QStringLiteral("/mnt/usb"), 1);
    runScenario(true, rfm::core::FileSource::Ssh, QStringLiteral("ssh:other-server"),
                QStringLiteral("/mnt/usb"), 1);
    runScenario(true, rfm::core::FileSource::Ssh, QStringLiteral("active"),
                QStringLiteral("/mnt/usb2"), 1);
}

void MainWindowTest::remoteSafetyFallbackPurgesOnlyMatchingHistory()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                        nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::directoryRequested, nullptr, nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
    QSignalSpy directoryRequests(&window, &rfm::app::MainWindow::directoryRequested);
    setConnectionIdentity(window, QStringLiteral("history-volume.example.test"),
                          QStringLiteral("alice"), QStringLiteral("/home/alice"));
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    auto* const pane = window.findChild<rfm::app::PaneWorkspace*>()->primaryPane();
    const QString machineId =
        serverProfileItem(navigation->tree(), 0)->data(0, Qt::UserRole + 2).toString();
    pane->showDirectory({rfm::core::FileSource::Ssh, machineId, QStringLiteral("/mnt/usb/Old")},
                        QStringLiteral("/mnt/usb/Old"), {}, rfm::app::PaneNavigation::Initial);
    pane->showDirectory({rfm::core::FileSource::Ssh, machineId, QStringLiteral("/srv/safe")},
                        QStringLiteral("/srv/safe"), {}, rfm::app::PaneNavigation::Normal);
    pane->showDirectory({rfm::core::FileSource::Ssh, machineId, QStringLiteral("/mnt/usb/Films")},
                        QStringLiteral("/mnt/usb/Films"), {}, rfm::app::PaneNavigation::Normal);
    rfm::core::StorageVolume mounted;
    mounted.displayName = QStringLiteral("Remote USB");
    mounted.device = QStringLiteral("/dev/sdb1");
    mounted.rootPath = QStringLiteral("/mnt/usb");
    mounted.kind = rfm::core::StorageKind::External;
    const quint64 initialRefresh = storageRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, initialRefresh),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{mounted})));
    navigation->tree()->setCurrentItem(
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), mounted.device));
    window.findChild<QPushButton*>(QStringLiteral("unmountVolumeButton"))->click();
    const auto request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    const rfm::core::VolumeOperationResult success{request.id,
                                                   request.operation,
                                                   request.target.device,
                                                   rfm::core::VolumeOperationError::None,
                                                   {}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, success)));
    QCOMPARE(directoryRequests.size(), 1);
    const quint64 fallbackId = directoryRequests.constFirst().at(0).toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleDirectoryListed", Qt::DirectConnection, Q_ARG(quint64, fallbackId),
        Q_ARG(QString, QStringLiteral("/home/alice")),
        Q_ARG(QList<rfm::core::RemoteEntry>, QList<rfm::core::RemoteEntry>{})));
    QCOMPARE(pane->currentPath(), QStringLiteral("/home/alice"));
    pane->requestBack();
    QCOMPARE(directoryRequests.size(), 2);
    QCOMPARE(directoryRequests.constLast().at(1).toString(), QStringLiteral("/srv/safe"));
}

void MainWindowTest::remoteDisconnectClearsPendingVolumeState()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                        nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
    setConnectionIdentity(window, QStringLiteral("disconnect-volume.example.test"));
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    rfm::core::StorageVolume available;
    available.displayName = QStringLiteral("Remote USB");
    available.device = QStringLiteral("/dev/sdb1");
    available.kind = rfm::core::StorageKind::External;
    available.mounted = false;
    const quint64 initialRefresh = storageRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, initialRefresh),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{available})));
    navigation->tree()->setCurrentItem(
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), available.device));
    window.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"))->click();
    QCOMPARE(operations.size(), 1);
    const auto request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    const rfm::core::VolumeOperationResult late{request.id,
                                                request.operation,
                                                request.target.device,
                                                rfm::core::VolumeOperationError::ConnectionLost,
                                                {}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, late)));
    QCOMPARE(storageRequests.size(), 1);
    QCOMPARE(serverProfileCount(navigation->tree()), 0);
}

void MainWindowTest::remoteDisconnectAfterCommandBeforeRefreshKeepsObservedModel()
{
    rfm::app::MainWindow window;
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteStorageRequested, nullptr, nullptr);
    QObject::disconnect(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested, nullptr,
                        nullptr);
    QSignalSpy storageRequests(&window, &rfm::app::MainWindow::remoteStorageRequested);
    QSignalSpy operations(&window, &rfm::app::MainWindow::remoteVolumeOperationRequested);
    setConnectionIdentity(window, QStringLiteral("post-command-loss.example.test"));
    auto* const navigation = window.findChild<rfm::app::NavigationTree*>();
    rfm::core::StorageVolume available;
    available.displayName = QStringLiteral("Remote USB");
    available.device = QStringLiteral("/dev/sdb1");
    available.kind = rfm::core::StorageKind::External;
    available.mounted = false;
    const quint64 initialRefresh = storageRequests.constFirst().constFirst().toULongLong();
    QVERIFY(QMetaObject::invokeMethod(
        &window, "handleRemoteStorageVolumes", Qt::DirectConnection, Q_ARG(quint64, initialRefresh),
        Q_ARG(QList<rfm::core::StorageVolume>, QList<rfm::core::StorageVolume>{available})));
    navigation->tree()->setCurrentItem(
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), available.device));
    window.findChild<QPushButton*>(QStringLiteral("mountVolumeButton"))->click();
    const auto request =
        operations.constFirst().constFirst().value<rfm::core::VolumeOperationRequest>();
    const rfm::core::VolumeOperationResult success{request.id,
                                                   request.operation,
                                                   request.target.device,
                                                   rfm::core::VolumeOperationError::None,
                                                   {}};
    QVERIFY(QMetaObject::invokeMethod(&window, "handleRemoteVolumeOperationResult",
                                      Qt::DirectConnection,
                                      Q_ARG(rfm::core::VolumeOperationResult, success)));
    QCOMPARE(storageRequests.size(), 2);
    QTreeWidgetItem* const stillObserved =
        volumeItemByDevice(remoteCategoryItem(navigation->tree()), available.device);
    QVERIFY(stillObserved != nullptr);
    QCOMPARE(stillObserved->data(0, Qt::UserRole + 6).value<rfm::core::StorageVolume>().mounted,
             false);

    QVERIFY(QMetaObject::invokeMethod(&window, "handleDisconnected", Qt::DirectConnection));
    QCOMPARE(serverProfileCount(navigation->tree()), 0);
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
