#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/app/PaneWorkspace.hpp"
#include "remotefilemanager/app/WorkspaceTabs.hpp"
#include "remotefilemanager/core/InternalTransfer.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QRubberBand>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>

class FileBrowserPaneTest final : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void displaysDirectoryAndBuildsRemoteSelection();
    void switchesBetweenDetailsAndMosaicWithoutRelisting();
    void preservesMosaicSelectionAndUsesSharedModel();
    void preservesMosaicClickSelectionAsDetailsRowSelection();
    void mosaicZoomHandlesWheelAndBounds();
    void mosaicZoomIsIndependentAndPersistsAcrossViewSwitch();
    void keepsViewModeIndependentAcrossPanesAndWorkspaces();
    void mosaicActivationUsesExistingNavigationPath();
    void togglesHiddenEntriesForLocalAndSsh();
    void presentsFileTypesIconsAndModificationTimesConsistently();
    void configuresIndependentMovableColumns();
    void sortsEveryColumnUsingRawValues();
    void loadsDirectoryCountsLazilyAndSortsThem();
    void preservesSshDirectoryCountsAcrossRefresh();
    void rubberBandSelectsMultipleLocalRows();
    void controlRubberBandTogglesRemoteRows();
    void dragFromSelectedRowPreservesSelectionAndStartsInternalDrag();
    void dragWithActionModifierPreservesMultipleSelection_data();
    void dragWithActionModifierPreservesMultipleSelection();
    void emitsNavigationIntentions();
    void requestsOpeningLocalFilesWithoutChangingDirectoryOrSshBehavior();
    void preparesContextSelectionBeforeEmittingIntent();
    void buildsPropertiesForTheEntryUnderTheContextClick();
    void restoresSelectionAndScrollOnRefresh();
    void appliesPendingSelectionAfterOperation();
    void workspaceStartsSingleAndTogglesSplit();
    void workspaceTracksActivePaneFromInteraction();
    void workspaceKeepsPanePathsAndSelectionsIndependent();
    void navigationHistorySupportsBackForwardAndBranching();
    void parentAndRefreshHaveCorrectHistorySemantics();
    void navigatesFromCanonicalLoginDirectoryToRemoteRoot();
    void activatesDirectoryAndBrokenSymbolicLinksForBackendResolution();
    void workspaceHistoriesAreIndependent();
    void constructsAndAcceptsOnlyInternalDragPayloads();
    void mosaicDropUsesTheExistingTransferPipeline();
    void resolvesDropOnCurrentDirectoryAndSubfolder();
    void constructsLocalPayloadAndResolvesLocalDropDestinations();
    void acceptsCrossSourceCopyIntentionsAndPreservesPayload();
    void cutAppearanceSurvivesRefreshAndClearsCleanly();
    void focusesLocationAndSwitchesVisiblePane();
    void navigatesLocalDirectoriesWithSourceAwareHistory();
    void persistsTableHeaderStateAcrossInstances();
    void persistsAscendingTableSortState();
    void persistsNoTableSortState();
    void restoresDefaultTableHeaderStateWhenPreferenceIsInvalid();
    void sharesTableHeaderStateBetweenLocalAndSshPanes();
    void cyclesThroughThreeSortStates();
    void resetFileViewClearsStateAndRestoresAdaptiveLayout();
    void manualLayoutSurvivesResizeAndPersistsMode();
    void responsiveLayoutKeepsColumnsUsableAndOrderStable();
    void adaptiveLayoutChangesContinuouslyAroundMinimum();
    void headerMovesSectionsLiveDuringDrag();
    void headerMovesEachSectionLive_data();
    void headerMovesEachSectionLive();
};

namespace
{

constexpr auto tableHeaderStateKey = "ui/fileBrowserPane/headerState";
constexpr auto tableSortColumnKey = "ui/fileBrowserPane/sortColumn";
constexpr auto tableSortOrderKey = "ui/fileBrowserPane/sortOrder";
constexpr auto tableLayoutModeKey = "ui/fileBrowserPane/layoutMode";

} // namespace

void FileBrowserPaneTest::togglesHiddenEntriesForLocalAndSsh()
{
    rfm::app::FileBrowserPane pane;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("visible"), 0, {}, false, false, false},
        {QStringLiteral(".dotfile"), 0, {}, false, false, false},
        {QStringLiteral("native-hidden"), 0, {}, false, false, true}};
    const rfm::core::BrowserLocation local{rfm::core::FileSource::Local,
                                           QString::fromLatin1(rfm::core::LocalMachineId),
                                           QStringLiteral("/fixture")};
    pane.showDirectory(local, QStringLiteral("/fixture"), entries);
    QVERIFY(!pane.fileTable()->isRowHidden(0));
    QVERIFY(pane.fileTable()->isRowHidden(1));
    QVERIFY(pane.fileTable()->isRowHidden(2));
    pane.setShowHiddenFiles(true);
    QVERIFY(!pane.fileTable()->isRowHidden(1));
    QVERIFY(!pane.fileTable()->isRowHidden(2));
    pane.setShowHiddenFiles(false);
    QVERIFY(pane.fileTable()->isRowHidden(1));
    QVERIFY(pane.fileTable()->isRowHidden(2));

    const rfm::core::BrowserLocation ssh{rfm::core::FileSource::Ssh, QStringLiteral("remote-id"),
                                         QStringLiteral("/home")};
    pane.showDirectory(ssh, QStringLiteral("sftp://remote/home"), entries);
    QVERIFY(pane.fileTable()->isRowHidden(1));
    pane.setShowHiddenFiles(true);
    QVERIFY(!pane.fileTable()->isRowHidden(1));
}

void FileBrowserPaneTest::init()
{
    QSettings settings;
    settings.remove(QString::fromLatin1(tableHeaderStateKey));
    settings.remove(QString::fromLatin1(tableSortColumnKey));
    settings.remove(QString::fromLatin1(tableSortOrderKey));
    settings.remove(QString::fromLatin1(tableLayoutModeKey));
    settings.sync();
}

void FileBrowserPaneTest::switchesBetweenDetailsAndMosaicWithoutRelisting()
{
    rfm::app::FileBrowserPane pane;
    const rfm::core::BrowserLocation location{rfm::core::FileSource::Local,
                                              QString::fromLatin1(rfm::core::LocalMachineId),
                                              QStringLiteral("/fixture")};
    pane.showDirectory(location, QStringLiteral("/fixture"),
                       {{QStringLiteral("photo.jpg"), 42, {}, false, false, false}});
    QSignalSpy navigation(&pane, &rfm::app::FileBrowserPane::locationNavigationRequested);

    QCOMPARE(pane.viewMode(), rfm::app::ViewMode::Details);
    pane.setViewMode(rfm::app::ViewMode::Mosaic);
    QCOMPARE(pane.viewMode(), rfm::app::ViewMode::Mosaic);
    QCOMPARE(pane.currentLocation(), location);
    QCOMPARE(navigation.count(), 0);
    pane.setViewMode(rfm::app::ViewMode::Details);
    QCOMPARE(pane.viewMode(), rfm::app::ViewMode::Details);
    QCOMPARE(pane.currentLocation(), location);
    QCOMPARE(navigation.count(), 0);
}

void FileBrowserPaneTest::preservesMosaicSelectionAndUsesSharedModel()
{
    rfm::app::FileBrowserPane pane;
    pane.showDirectory({rfm::core::FileSource::Local,
                        QString::fromLatin1(rfm::core::LocalMachineId), QStringLiteral("/fixture")},
                       QStringLiteral("/fixture"),
                       {{QStringLiteral("one.txt"), 1, {}, false, false, false},
                        {QStringLiteral("two.txt"), 2, {}, false, false, false}});
    auto* const table = pane.fileTable();
    auto* const mosaic = pane.mosaicView();
    QCOMPARE(mosaic->model(), table->model());
    QCOMPARE(mosaic->selectionModel(), table->selectionModel());

    table->selectionModel()->select(table->model()->index(0, 0),
                                    QItemSelectionModel::ClearAndSelect |
                                        QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(1, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    pane.setViewMode(rfm::app::ViewMode::Mosaic);
    QCOMPARE(mosaic->selectionModel()->selectedRows(0).size(), 2);
    pane.setViewMode(rfm::app::ViewMode::Details);
    QCOMPARE(table->selectionModel()->selectedRows(0).size(), 2);
}

void FileBrowserPaneTest::preservesMosaicClickSelectionAsDetailsRowSelection()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(480, 320);
    pane.showDirectory({rfm::core::FileSource::Local,
                       QString::fromLatin1(rfm::core::LocalMachineId), QStringLiteral("/fixture")},
                       QStringLiteral("/fixture"),
                       {{QStringLiteral("one.txt"), 1, {}, false, false, false},
                        {QStringLiteral("two.txt"), 2, {}, false, false, false},
                        {QStringLiteral("three.txt"), 3, {}, false, false, false}});
    pane.setViewMode(rfm::app::ViewMode::Mosaic);
    pane.show();
    QTest::qWait(1);

    auto* const mosaic = pane.mosaicView();
    const QModelIndex index = mosaic->model()->index(0, 0);
    QTest::mouseClick(mosaic->viewport(), Qt::LeftButton, Qt::NoModifier,
                      mosaic->visualRect(index).center());

    pane.setViewMode(rfm::app::ViewMode::Details);
    QVERIFY(pane.fileTable()->selectionModel()->isRowSelected(0, QModelIndex{}));

    pane.setViewMode(rfm::app::ViewMode::Mosaic);
    QTest::mouseClick(mosaic->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      mosaic->visualRect(mosaic->model()->index(1, 0)).center());
    pane.setViewMode(rfm::app::ViewMode::Details);
    QCOMPARE(pane.fileTable()->selectionModel()->selectedRows(0).size(), 2);

    pane.fileTable()->clearSelection();
    pane.setViewMode(rfm::app::ViewMode::Mosaic);
    QTest::mouseClick(mosaic->viewport(), Qt::LeftButton, Qt::NoModifier,
                      mosaic->visualRect(mosaic->model()->index(0, 0)).center());
    QTest::mouseClick(mosaic->viewport(), Qt::LeftButton, Qt::ShiftModifier,
                      mosaic->visualRect(mosaic->model()->index(2, 0)).center());
    pane.setViewMode(rfm::app::ViewMode::Details);
    QCOMPARE(pane.fileTable()->selectionModel()->selectedRows(0).size(), 3);
}

void FileBrowserPaneTest::mosaicZoomHandlesWheelAndBounds()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(480, 320);
    pane.showDirectory({rfm::core::FileSource::Local,
                        QString::fromLatin1(rfm::core::LocalMachineId), QStringLiteral("/fixture")},
                       QStringLiteral("/fixture"),
                       {{QStringLiteral("one.txt"), 1, {}, false, false, false}});
    pane.setViewMode(rfm::app::ViewMode::Mosaic);
    pane.show();
    QTest::qWait(1);

    auto* const mosaic = pane.mosaicView();
    QCOMPARE(pane.mosaicIconSize(), 48);
    QCOMPARE(mosaic->viewMode(), QListView::IconMode);
    QCOMPARE(mosaic->resizeMode(), QListView::Adjust);
    QVERIFY(mosaic->gridSize().width() >= mosaic->iconSize().width());
    mosaic->selectionModel()->select(mosaic->model()->index(0, 0),
                                     QItemSelectionModel::ClearAndSelect |
                                         QItemSelectionModel::Rows);

    const auto wheel = [mosaic](int delta, Qt::KeyboardModifiers modifiers) {
        QWheelEvent event(mosaic->viewport()->rect().center(),
                          mosaic->viewport()->mapToGlobal(mosaic->viewport()->rect().center()),
                          QPoint{}, QPoint{0, delta}, Qt::NoButton, modifiers,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(mosaic->viewport(), &event);
    };
    wheel(120, Qt::ControlModifier);
    QCOMPARE(pane.mosaicIconSize(), 56);
    QCOMPARE(mosaic->selectionModel()->selectedRows(0).size(), 1);
    pane.resize(900, 320);
    QCoreApplication::processEvents();
    QCOMPARE(pane.mosaicIconSize(), 56);
    wheel(-120, Qt::ControlModifier);
    QCOMPARE(pane.mosaicIconSize(), 48);
    wheel(120, Qt::NoModifier);
    QCOMPARE(pane.mosaicIconSize(), 48);

    for (int step = 0; step < 20; ++step) {
        wheel(-120, Qt::ControlModifier);
    }
    QCOMPARE(pane.mosaicIconSize(), 32);
    for (int step = 0; step < 20; ++step) {
        wheel(120, Qt::ControlModifier);
    }
    QCOMPARE(pane.mosaicIconSize(), 128);
}

void FileBrowserPaneTest::mosaicZoomIsIndependentAndPersistsAcrossViewSwitch()
{
    rfm::app::FileBrowserPane first;
    rfm::app::FileBrowserPane second;
    const auto location = rfm::core::BrowserLocation{
        rfm::core::FileSource::Local, QString::fromLatin1(rfm::core::LocalMachineId),
        QStringLiteral("/fixture")};
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("one.txt"), 1, {}, false, false, false}};
    first.showDirectory(location, QStringLiteral("/fixture"), entries);
    second.showDirectory(location, QStringLiteral("/fixture"), entries);
    first.setViewMode(rfm::app::ViewMode::Mosaic);
    second.setViewMode(rfm::app::ViewMode::Mosaic);

    auto zoomOnce = [](rfm::app::FileBrowserPane& pane) {
        auto* const view = pane.mosaicView();
        QWheelEvent event(view->viewport()->rect().center(),
                          view->viewport()->mapToGlobal(view->viewport()->rect().center()), QPoint{},
                          QPoint{0, 120}, Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase,
                          false);
        QApplication::sendEvent(view->viewport(), &event);
    };
    zoomOnce(first);
    QCOMPARE(first.mosaicIconSize(), 56);
    QCOMPARE(second.mosaicIconSize(), 48);
    first.setViewMode(rfm::app::ViewMode::Details);
    first.setViewMode(rfm::app::ViewMode::Mosaic);
    QCOMPARE(first.mosaicIconSize(), 56);

    rfm::app::WorkspaceTabs tabs;
    auto* const firstTab = tabs.activeWorkspace();
    tabs.activePane()->showDirectory(location, QStringLiteral("/fixture"), entries);
    tabs.activePane()->setViewMode(rfm::app::ViewMode::Mosaic);
    zoomOnce(*tabs.activePane());
    auto* const secondTab = tabs.createWorkspace();
    secondTab->activePane()->showDirectory(location, QStringLiteral("/fixture"), entries);
    secondTab->activePane()->setViewMode(rfm::app::ViewMode::Mosaic);
    QCOMPARE(secondTab->activePane()->mosaicIconSize(), 48);
    tabs.setActiveWorkspace(firstTab);
    QCOMPARE(tabs.activePane()->mosaicIconSize(), 56);
}

void FileBrowserPaneTest::keepsViewModeIndependentAcrossPanesAndWorkspaces()
{
    rfm::app::PaneWorkspace workspace;
    workspace.primaryPane()->setViewMode(rfm::app::ViewMode::Mosaic);
    workspace.setSplit(true);
    QCOMPARE(workspace.primaryPane()->viewMode(), rfm::app::ViewMode::Mosaic);
    QCOMPARE(workspace.otherVisiblePane()->viewMode(), rfm::app::ViewMode::Details);

    rfm::app::WorkspaceTabs tabs;
    auto* const firstWorkspace = tabs.activeWorkspace();
    tabs.activePane()->setViewMode(rfm::app::ViewMode::Mosaic);
    auto* const secondWorkspace = tabs.createWorkspace();
    QCOMPARE(tabs.activePane()->viewMode(), rfm::app::ViewMode::Details);
    tabs.setActiveWorkspace(firstWorkspace);
    QCOMPARE(tabs.activePane()->viewMode(), rfm::app::ViewMode::Mosaic);
    tabs.setActiveWorkspace(secondWorkspace);
    QCOMPARE(tabs.activePane()->viewMode(), rfm::app::ViewMode::Details);
}

void FileBrowserPaneTest::mosaicActivationUsesExistingNavigationPath()
{
    rfm::app::FileBrowserPane pane;
    const rfm::core::BrowserLocation location{rfm::core::FileSource::Local,
                                              QString::fromLatin1(rfm::core::LocalMachineId),
                                              QStringLiteral("/fixture")};
    pane.showDirectory(location, QStringLiteral("/fixture"),
                       {{QStringLiteral("child"), 0, {}, true, false, false}});
    QSignalSpy navigation(&pane, &rfm::app::FileBrowserPane::locationNavigationRequested);
    pane.setViewMode(rfm::app::ViewMode::Mosaic);
    pane.mosaicView()->doubleClicked(pane.mosaicView()->model()->index(0, 0));
    QCOMPARE(navigation.count(), 1);
    const auto request = navigation.constFirst();
    QCOMPARE(request.constFirst().value<rfm::core::BrowserLocation>().path,
             QStringLiteral("/fixture/child"));
}

void FileBrowserPaneTest::navigatesLocalDirectoriesWithSourceAwareHistory()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("child")));
    const rfm::core::BrowserLocation rootLocation{rfm::core::FileSource::Local,
                                                  QString::fromLatin1(rfm::core::LocalMachineId),
                                                  temporary.path()};
    const rfm::core::BrowserLocation childLocation{rfm::core::FileSource::Local,
                                                   QString::fromLatin1(rfm::core::LocalMachineId),
                                                   root.filePath(QStringLiteral("child"))};

    rfm::app::FileBrowserPane pane;
    QSignalSpy navigation(&pane, &rfm::app::FileBrowserPane::locationNavigationRequested);
    pane.showDirectory(rootLocation, QStringLiteral("file:///fixture"),
                       {{QStringLiteral("child"), 0, {}, true, false}},
                       rfm::app::PaneNavigation::Initial);
    QCOMPARE(pane.source(), rfm::core::FileSource::Local);
    QVERIFY(pane.fileTable()->dragEnabled());
    QVERIFY(pane.fileTable()->acceptDrops());
    QVERIFY(pane.createInternalDragData().isEmpty());

    pane.setTransferContext(QStringLiteral("instance"), {}, 5);
    pane.fileTable()->selectRow(0);
    const auto localPayload = rfm::core::decodeInternalTransfer(pane.createInternalDragData());
    QVERIFY(localPayload.has_value());
    QCOMPARE(localPayload->source, rfm::core::FileSource::Local);
    QCOMPARE(localPayload->sourceMachineId, QString::fromLatin1(rfm::core::LocalMachineId));
    QCOMPARE(localPayload->connection, rfm::core::RemoteConnectionIdentity{});

    pane.navigateTo(childLocation.path);
    QCOMPARE(navigation.size(), 1);
    QCOMPARE(qvariant_cast<rfm::core::BrowserLocation>(navigation.constLast().at(0)),
             childLocation);
    pane.showDirectory(childLocation, QStringLiteral("file:///fixture/child"), {},
                       rfm::app::PaneNavigation::Normal);
    QVERIFY(pane.canGoBack());
    pane.requestBack();
    QCOMPARE(qvariant_cast<rfm::core::BrowserLocation>(navigation.constLast().at(0)), rootLocation);
    pane.showDirectory(rootLocation, QStringLiteral("file:///fixture"), {},
                       rfm::app::PaneNavigation::Back);
    QVERIFY(pane.canGoForward());
    pane.requestForward();
    QCOMPARE(qvariant_cast<rfm::core::BrowserLocation>(navigation.constLast().at(0)),
             childLocation);
    pane.showDirectory(childLocation, QStringLiteral("file:///fixture/child"), {},
                       rfm::app::PaneNavigation::Forward);
    pane.requestParentDirectory();
    QCOMPARE(qvariant_cast<rfm::core::BrowserLocation>(navigation.constLast().at(0)), rootLocation);
    pane.showDirectory(rootLocation, QStringLiteral("file:///fixture"), {},
                       rfm::app::PaneNavigation::Normal);
    pane.requestRefresh();
    QCOMPARE(navigation.constLast().at(1).value<rfm::app::PaneNavigation>(),
             rfm::app::PaneNavigation::Refresh);
}

void FileBrowserPaneTest::persistsTableHeaderStateAcrossInstances()
{
    {
        rfm::app::FileBrowserPane pane;
        QHeaderView* const header = pane.fileTable()->horizontalHeader();
        header->resizeSection(1, 205);
        header->moveSection(header->visualIndex(0), 3);
        pane.fileTable()->sortItems(3, Qt::DescendingOrder);
    }

    rfm::app::FileBrowserPane restored;
    QHeaderView* const header = restored.fileTable()->horizontalHeader();
    QCOMPARE(header->sectionSize(1), 205);
    QCOMPARE(header->visualIndex(0), 3);
    QCOMPARE(header->sortIndicatorSection(), 3);
    QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
}

void FileBrowserPaneTest::persistsAscendingTableSortState()
{
    {
        rfm::app::FileBrowserPane pane;
        pane.fileTable()->sortItems(2, Qt::AscendingOrder);
    }

    rfm::app::FileBrowserPane restored;
    const QHeaderView* const header = restored.fileTable()->horizontalHeader();
    QCOMPARE(header->sortIndicatorSection(), 2);
    QCOMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);
    QVERIFY(header->isSortIndicatorShown());
}

void FileBrowserPaneTest::persistsNoTableSortState()
{
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("z.txt"), 2, {}, false, false},
        {QStringLiteral("a.txt"), 1, {}, false, false},
    };
    {
        rfm::app::FileBrowserPane pane;
        pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("/srv"), entries);
        pane.fileTable()->sortItems(1, Qt::DescendingOrder);
        pane.fileTable()->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
        pane.fileTable()->horizontalHeader()->setSortIndicatorShown(false);
    }

    rfm::app::FileBrowserPane restored;
    const QHeaderView* const header = restored.fileTable()->horizontalHeader();
    QCOMPARE(header->sortIndicatorSection(), -1);
    QVERIFY(!header->isSortIndicatorShown());
    restored.showDirectory(QStringLiteral("/srv"), QStringLiteral("/srv"), entries);
    QCOMPARE(restored.fileTable()->item(0, 0)->text(), QStringLiteral("z.txt"));
    QCOMPARE(restored.fileTable()->item(1, 0)->text(), QStringLiteral("a.txt"));
}

void FileBrowserPaneTest::restoresDefaultTableHeaderStateWhenPreferenceIsInvalid()
{
    QSettings settings;
    settings.setValue(QString::fromLatin1(tableHeaderStateKey), QByteArrayLiteral("invalid"));
    settings.sync();

    rfm::app::FileBrowserPane pane;
    QHeaderView* const header = pane.fileTable()->horizontalHeader();
    QCOMPARE(header->sectionSize(0), 280);
    QCOMPARE(header->sectionSize(1), 110);
    QCOMPARE(header->sectionSize(2), 180);
    QCOMPARE(header->sectionSize(3), 170);
    for (int logicalIndex = 0; logicalIndex < header->count(); ++logicalIndex) {
        QCOMPARE(header->visualIndex(logicalIndex), logicalIndex);
    }
    QCOMPARE(header->sortIndicatorSection(), -1);
    QCOMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);
}

void FileBrowserPaneTest::sharesTableHeaderStateBetweenLocalAndSshPanes()
{
    {
        rfm::app::FileBrowserPane pane;
        QHeaderView* const header = pane.fileTable()->horizontalHeader();
        header->resizeSection(2, 240);
        header->moveSection(header->visualIndex(0), 2);
        pane.fileTable()->sortItems(2, Qt::AscendingOrder);
    }

    rfm::app::FileBrowserPane localPane;
    rfm::app::FileBrowserPane sshPane;
    const QHeaderView* const localHeader = localPane.fileTable()->horizontalHeader();
    const QHeaderView* const sshHeader = sshPane.fileTable()->horizontalHeader();
    for (int logicalIndex = 0; logicalIndex < localHeader->count(); ++logicalIndex) {
        QCOMPARE(localHeader->visualIndex(logicalIndex), sshHeader->visualIndex(logicalIndex));
        QCOMPARE(localHeader->sectionSize(logicalIndex), sshHeader->sectionSize(logicalIndex));
    }
    QCOMPARE(localHeader->sortIndicatorSection(), sshHeader->sortIndicatorSection());
    QCOMPARE(localHeader->sortIndicatorOrder(), sshHeader->sortIndicatorOrder());

    localPane.fileTable()->horizontalHeader()->moveSection(localHeader->visualIndex(0), 0);
    QTest::qWait(250);
    QCOMPARE(sshHeader->visualIndex(0), 0);
}

void FileBrowserPaneTest::cyclesThroughThreeSortStates()
{
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("z.txt"), 2, {}, false, false},
        {QStringLiteral("a.txt"), 1, {}, false, false},
        {QStringLiteral("m.txt"), 3, {}, false, false},
    };
    rfm::app::FileBrowserPane pane;
    pane.resize(640, 320);
    pane.show();
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("/srv"), entries);
    QCoreApplication::processEvents();
    QHeaderView* const header = pane.fileTable()->horizontalHeader();
    const QPoint center(header->sectionViewportPosition(0) + header->sectionSize(0) / 2,
                        header->height() / 2);

    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, center);
    QCOMPARE(header->sortIndicatorSection(), 0);
    QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    QVERIFY(header->isSortIndicatorShown());
    QCOMPARE(pane.fileTable()->item(0, 0)->text(), QStringLiteral("z.txt"));

    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, center);
    QCOMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);
    QCOMPARE(pane.fileTable()->item(0, 0)->text(), QStringLiteral("a.txt"));

    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, center);
    QCOMPARE(header->sortIndicatorSection(), -1);
    QVERIFY(!header->isSortIndicatorShown());
    QCOMPARE(pane.fileTable()->item(0, 0)->text(), QStringLiteral("z.txt"));
    QCOMPARE(pane.fileTable()->item(1, 0)->text(), QStringLiteral("a.txt"));
    QCOMPARE(pane.fileTable()->item(2, 0)->text(), QStringLiteral("m.txt"));

    const QList<rfm::core::RemoteEntry> refreshedEntries{
        {QStringLiteral("m.txt"), 3, {}, false, false},
        {QStringLiteral("z.txt"), 2, {}, false, false},
        {QStringLiteral("a.txt"), 1, {}, false, false},
    };
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("/srv"), refreshedEntries);
    QCOMPARE(pane.fileTable()->item(0, 0)->text(), QStringLiteral("m.txt"));
    QCOMPARE(pane.fileTable()->item(1, 0)->text(), QStringLiteral("z.txt"));
    QCOMPARE(pane.fileTable()->item(2, 0)->text(), QStringLiteral("a.txt"));

    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, center);
    QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    QVERIFY(header->isSortIndicatorShown());

    const QPoint sizeCenter(header->sectionViewportPosition(1) + header->sectionSize(1) / 2,
                            header->height() / 2);
    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, sizeCenter);
    QCOMPARE(header->sortIndicatorSection(), 1);
    QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
}

void FileBrowserPaneTest::resetFileViewClearsStateAndRestoresAdaptiveLayout()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(700, 400);
    pane.show();
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("/srv"),
                       {{QStringLiteral("z.txt"), 2, {}, false, false},
                        {QStringLiteral("a-very-long-file-name.txt"), 1, {}, false, false}});
    QCoreApplication::processEvents();
    QHeaderView* const header = pane.fileTable()->horizontalHeader();
    header->resizeSection(0, 190);
    header->resizeSection(1, 240);
    header->resizeSection(2, 145);
    header->resizeSection(3, 210);
    header->moveSection(header->visualIndex(0), 3);
    header->moveSection(header->visualIndex(1), 0);
    header->moveSection(header->visualIndex(2), 1);
    header->setSectionHidden(2, true);
    pane.fileTable()->sortItems(2, Qt::AscendingOrder);
    QTest::qWait(250);
    QVERIFY(QSettings{}.contains(QString::fromLatin1(tableHeaderStateKey)));

    pane.resetFileView();
    QCOMPARE(header->sortIndicatorSection(), -1);
    QVERIFY(!header->isSortIndicatorShown());
    for (int logicalIndex = 0; logicalIndex < header->count(); ++logicalIndex) {
        QCOMPARE(header->visualIndex(logicalIndex), logicalIndex);
        QVERIFY(!header->isSectionHidden(logicalIndex));
    }
    QSettings settings;
    QVERIFY(!settings.contains(QString::fromLatin1(tableHeaderStateKey)));
    QVERIFY(!settings.contains(QString::fromLatin1(tableSortColumnKey)));
    QVERIFY(!settings.contains(QString::fromLatin1(tableSortOrderKey)));
    QVERIFY(!settings.contains(QString::fromLatin1(tableLayoutModeKey)));

    const int initialNameWidth = header->sectionSize(0);
    pane.resize(1400, 400);
    QCoreApplication::processEvents();
    QVERIFY(header->sectionSize(0) != initialNameWidth);
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("/srv"),
                       {{QStringLiteral("natural-first"), 1, {}, false, false},
                        {QStringLiteral("natural-second"), 2, {}, false, false}});
    QCOMPARE(pane.fileTable()->item(0, 0)->text(), QStringLiteral("natural-first"));

    rfm::app::FileBrowserPane restored;
    const QHeaderView* const restoredHeader = restored.fileTable()->horizontalHeader();
    for (int logicalIndex = 0; logicalIndex < restoredHeader->count(); ++logicalIndex) {
        QCOMPARE(restoredHeader->visualIndex(logicalIndex), logicalIndex);
        QVERIFY(!restoredHeader->isSectionHidden(logicalIndex));
    }
    QCOMPARE(restoredHeader->sortIndicatorSection(), -1);
    QVERIFY(!restoredHeader->isSortIndicatorShown());
}

void FileBrowserPaneTest::manualLayoutSurvivesResizeAndPersistsMode()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(900, 400);
    pane.show();
    QCoreApplication::processEvents();
    QHeaderView* const header = pane.fileTable()->horizontalHeader();
    header->resizeSection(1, 235);
    QTest::qWait(250);
    QCOMPARE(header->sectionSize(1), 235);
    QCOMPARE(QSettings{}.value(QString::fromLatin1(tableLayoutModeKey)).toString(),
             QStringLiteral("manual"));
    pane.resize(500, 400);
    QCoreApplication::processEvents();
    QCOMPARE(header->sectionSize(1), 235);
}

void FileBrowserPaneTest::responsiveLayoutKeepsColumnsUsableAndOrderStable()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(1200, 360);
    pane.show();
    pane.showDirectory(
        QStringLiteral("/srv"), QStringLiteral("/srv"),
        {{QStringLiteral("name-with-a-reasonable-length.txt"), 1, {}, false, false}});
    QCoreApplication::processEvents();
    QHeaderView* const header = pane.fileTable()->horizontalHeader();
    const int wideNameWidth = header->sectionSize(0);
    QVERIFY(wideNameWidth > 280);
    const QList<int> visualOrder{header->visualIndex(0), header->visualIndex(1),
                                 header->visualIndex(2), header->visualIndex(3)};

    pane.resize(220, 360);
    QCoreApplication::processEvents();
    QVERIFY(header->sectionSize(0) < wideNameWidth);
    QVERIFY(header->sectionSize(0) >= 160);
    QVERIFY(header->sectionSize(1) >= 72);
    QVERIFY(header->sectionSize(2) >= 100);
    QVERIFY(header->sectionSize(3) >= 130);
    QVERIFY(pane.fileTable()->horizontalScrollBar()->isVisible());
    const QList<int> narrowVisualOrder{header->visualIndex(0), header->visualIndex(1),
                                       header->visualIndex(2), header->visualIndex(3)};
    QCOMPARE(narrowVisualOrder, visualOrder);

    pane.resize(1200, 360);
    QCoreApplication::processEvents();
    QVERIFY(header->sectionSize(1) >= 110);
    QVERIFY(header->sectionSize(2) >= 180);
    QVERIFY(header->sectionSize(3) >= 170);
}

void FileBrowserPaneTest::adaptiveLayoutChangesContinuouslyAroundMinimum()
{
    rfm::app::FileBrowserPane pane;
    pane.show();
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("/srv"),
                       {{QStringLiteral("a-file.txt"), 1, {}, false, false}});
    QHeaderView* const header = pane.fileTable()->horizontalHeader();
    int previousNameWidth = 0;
    for (int paneWidth = 500; paneWidth <= 800; ++paneWidth) {
        pane.resize(paneWidth, 320);
        QCoreApplication::processEvents();
        const int nameWidth = header->sectionSize(0);
        if (previousNameWidth > 0) {
            QVERIFY2(qAbs(nameWidth - previousNameWidth) <= 20,
                     "adaptive Name width changed disproportionately");
        }
        previousNameWidth = nameWidth;
    }

    rfm::app::PaneWorkspace workspace;
    workspace.resize(900, 320);
    workspace.setSplit(true);
    QCoreApplication::processEvents();
    const auto* const primaryHeader = workspace.primaryPane()->fileTable()->horizontalHeader();
    const auto* const secondaryHeader =
        workspace.otherVisiblePane()->fileTable()->horizontalHeader();
    QVERIFY(primaryHeader->sectionSize(0) >= 160);
    QVERIFY(secondaryHeader->sectionSize(0) >= 160);
}

void FileBrowserPaneTest::headerMovesSectionsLiveDuringDrag()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(900, 320);
    pane.show();
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("/srv"),
                       {{QStringLiteral("first.txt"), 12, {}, false, false},
                        {QStringLiteral("second.txt"), 4, {}, false, false}});
    QCoreApplication::processEvents();
    QHeaderView* const header = pane.fileTable()->horizontalHeader();
    pane.fileTable()->sortItems(1, Qt::DescendingOrder);
    const QList<int> widths{header->sectionSize(0), header->sectionSize(1), header->sectionSize(2),
                            header->sectionSize(3)};
    QSignalSpy moved(header, &QHeaderView::sectionMoved);
    const QPoint start(header->sectionViewportPosition(0) + header->sectionSize(0) / 2,
                       header->height() / 2);
    const QPoint target(header->viewport()->width() - 2, header->height() / 2);
    QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(header->viewport(), target, 20);
    QCoreApplication::processEvents();
    const bool movedBeforeRelease = moved.size() > 0;
    QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier, target);
    QVERIFY(movedBeforeRelease);
    QCOMPARE(header->visualIndex(0), 3);
    QCOMPARE(header->sortIndicatorSection(), 1);
    QCOMPARE(header->sortIndicatorOrder(), Qt::DescendingOrder);
    const QList<int> finalWidths{header->sectionSize(0), header->sectionSize(1),
                                 header->sectionSize(2), header->sectionSize(3)};
    QCOMPARE(finalWidths, widths);
    QTest::qWait(250);
    rfm::app::FileBrowserPane restored;
    QCOMPARE(restored.fileTable()->horizontalHeader()->visualIndex(0), 3);
}

void FileBrowserPaneTest::headerMovesEachSectionLive_data()
{
    QTest::addColumn<int>("source");
    QTest::addColumn<int>("target");
    for (int source = 0; source < 4; ++source) {
        QTest::newRow(QByteArray::number(source).constData())
            << source << (source == 0 || source == 2 ? 3 : 0);
    }
}

void FileBrowserPaneTest::headerMovesEachSectionLive()
{
    QFETCH(const int, source);
    QFETCH(const int, target);
    rfm::app::FileBrowserPane pane;
    pane.resize(900, 320);
    pane.show();
    QCoreApplication::processEvents();
    QHeaderView* const header = pane.fileTable()->horizontalHeader();
    QSignalSpy moved(header, &QHeaderView::sectionMoved);
    const QPoint start(header->sectionViewportPosition(source) + header->sectionSize(source) / 2,
                       header->height() / 2);
    const QPoint destination(target == 0 ? 2 : header->viewport()->width() - 2,
                             header->height() / 2);
    QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(header->viewport(), destination, 20);
    QCoreApplication::processEvents();
    QVERIFY(moved.size() > 0);
    QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier, destination);
    QCOMPARE(header->visualIndex(source), target);
}

void FileBrowserPaneTest::displaysDirectoryAndBuildsRemoteSelection()
{
    rfm::app::FileBrowserPane pane;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("file.txt"), 1536, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false},
    };
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://user@example.test:22//srv"),
                       entries);

    QCOMPARE(pane.currentPath(), QStringLiteral("/srv"));
    QCOMPARE(pane.pathEdit()->text(), QStringLiteral("sftp://user@example.test:22//srv"));
    QCOMPARE(pane.fileTable()->rowCount(), 2);
    QCOMPARE(pane.fileTable()->item(0, 0)->text(), QStringLiteral("file.txt"));
    QCOMPARE(pane.fileTable()->item(0, 1)->text(), QLocale{}.formattedDataSize(1536));
    QCOMPARE(pane.fileTable()->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(pane.fileTable()->selectionBehavior(), QAbstractItemView::SelectRows);

    pane.fileTable()->selectionModel()->select(pane.fileTable()->model()->index(0, 0),
                                               QItemSelectionModel::Select |
                                                   QItemSelectionModel::Rows);
    pane.fileTable()->selectionModel()->select(pane.fileTable()->model()->index(1, 0),
                                               QItemSelectionModel::Select |
                                                   QItemSelectionModel::Rows);
    const auto selection = pane.selectedEntries();
    QCOMPARE(selection.size(), 2);
    QCOMPARE(selection.at(0).path, QStringLiteral("/srv/file.txt"));
    QVERIFY(!selection.at(0).directory);
    QCOMPARE(selection.at(1).path, QStringLiteral("/srv/folder"));
    QVERIFY(selection.at(1).directory);
}

void FileBrowserPaneTest::presentsFileTypesIconsAndModificationTimesConsistently()
{
    const QDateTime modified = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("folder"), 0, modified, true, false},
        {QStringLiteral("photo.jpg"), 100, modified, false, false},
        {QStringLiteral("archive.zip"), 200, modified, false, false},
        {QStringLiteral("unknown.rfm_unknown_extension_987"), 300, {}, false, false},
        {QStringLiteral("linked-photo.jpg"), 0, modified, false, true},
    };
    rfm::app::FileBrowserPane pane;

    pane.showDirectory({rfm::core::FileSource::Local,
                        QString::fromLatin1(rfm::core::LocalMachineId), QStringLiteral("/tmp")},
                       QStringLiteral("file:///tmp"), entries);
    QTableWidget* const table = pane.fileTable();
    QCOMPARE(table->columnCount(), 4);
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("Name"));
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("Size"));
    QCOMPARE(table->horizontalHeaderItem(2)->text(), QStringLiteral("Type"));
    QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("Modified"));
    QCOMPARE(
        table->item(0, 2)->text(),
        QMimeDatabase{}.mimeTypeForName(QStringLiteral("inode/directory")).comment().trimmed());
    QVERIFY(table->item(1, 2)->text() != QStringLiteral("File"));
    QVERIFY(table->item(2, 2)->text() != QStringLiteral("File"));
    // Le fichier sans type connu n'est plus identifié comme "File", mais avec une description
    // locale
    QCOMPARE(table->item(4, 2)->text(), QStringLiteral("Symbolic link"));
    QCOMPARE(table->item(1, 3)->text(), QLocale{}.toString(modified, QLocale::ShortFormat));
    QCOMPARE(table->item(3, 3)->text(), QStringLiteral("—"));
    for (int row = 0; row < table->rowCount(); ++row) {
        QVERIFY(!table->item(row, 0)->icon().isNull());
    }

    QStringList localTypes;
    for (int row = 0; row < table->rowCount(); ++row) {
        localTypes.push_back(table->item(row, 2)->text());
    }
    pane.showDirectory(
        {rfm::core::FileSource::Ssh, QStringLiteral("remote-id"), QStringLiteral("/srv")},
        QStringLiteral("sftp://host/srv"), entries);
    for (int row = 0; row < table->rowCount(); ++row) {
        QCOMPARE(table->item(row, 2)->text(), localTypes.at(row));
    }
    QCOMPARE(table->item(1, 3)->text(), QLocale{}.toString(modified, QLocale::ShortFormat));
    QCOMPARE(table->item(3, 3)->text(), QStringLiteral("—"));
}

void FileBrowserPaneTest::configuresIndependentMovableColumns()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(420, 260);
    pane.show();
    QApplication::processEvents();

    QTableWidget* const table = pane.fileTable();
    QHeaderView* const header = table->horizontalHeader();
    QVERIFY(header->sectionsClickable());
    QVERIFY(header->sectionsMovable());
    QVERIFY(header->isFirstSectionMovable());
    QVERIFY(!header->stretchLastSection());
    for (int section = 0; section < table->columnCount(); ++section) {
        QCOMPARE(header->sectionResizeMode(section), QHeaderView::Interactive);
    }

    const QList<int> initialWidths{header->sectionSize(0), header->sectionSize(1),
                                   header->sectionSize(2), header->sectionSize(3)};
    QVERIFY(initialWidths.at(0) >= 160);
    QVERIFY(initialWidths.at(1) >= 48);
    QVERIFY(initialWidths.at(2) >= 48);
    QVERIFY(initialWidths.at(3) >= 48);
    header->resizeSection(1, 60);
    QCOMPARE(header->sectionSize(0), initialWidths.at(0));
    QCOMPARE(header->sectionSize(1), 60);
    QCOMPARE(header->sectionSize(2), initialWidths.at(2));
    QCOMPARE(header->sectionSize(3), initialWidths.at(3));
    header->resizeSection(1, initialWidths.at(1) + 70);
    QCOMPARE(header->sectionSize(0), initialWidths.at(0));
    QCOMPARE(header->sectionSize(1), initialWidths.at(1) + 70);
    QCOMPARE(header->sectionSize(2), initialWidths.at(2));
    QCOMPARE(header->sectionSize(3), initialWidths.at(3));
    QTRY_VERIFY(table->horizontalScrollBar()->maximum() > 0);

    header->moveSection(header->visualIndex(0), 3);
    QCOMPARE(header->visualIndex(0), 3);
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"), {});
    QCOMPARE(header->visualIndex(0), 3);
    QCOMPARE(header->sectionSize(1), initialWidths.at(1) + 70);

    rfm::app::PaneWorkspace workspace;
    workspace.setSplit(true);
    for (rfm::app::FileBrowserPane* const browser :
         {workspace.primaryPane(), workspace.otherVisiblePane()}) {
        QVERIFY(browser->fileTable()->horizontalHeader()->sectionsMovable());
        QVERIFY(browser->fileTable()->horizontalHeader()->isFirstSectionMovable());
        QVERIFY(!browser->fileTable()->horizontalHeader()->stretchLastSection());
    }
}

void FileBrowserPaneTest::sortsEveryColumnUsingRawValues()
{
    const QDateTime early = QDateTime::fromSecsSinceEpoch(1'600'000'000);
    const QDateTime late = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("z-folder"), 0, late, true, false},
        {QStringLiteral("a-folder"), 0, early, true, false},
        {QStringLiteral("b.txt"), 2, late, false, false},
        {QStringLiteral("a.jpg"), 10, early, false, false},
        {QStringLiteral("c.rfm_unknown_extension_987"), 1, {}, false, false},
    };
    rfm::app::FileBrowserPane pane;
    pane.resize(800, 320);
    pane.show();
    pane.showDirectory({rfm::core::FileSource::Local,
                        QString::fromLatin1(rfm::core::LocalMachineId), QStringLiteral("/tmp")},
                       QStringLiteral("file:///tmp"), entries);
    QTableWidget* const table = pane.fileTable();
    QHeaderView* const header = table->horizontalHeader();
    const auto names = [table]() {
        QStringList result;
        for (int row = 0; row < table->rowCount(); ++row) {
            result.push_back(table->item(row, 0)->text());
        }
        return result;
    };

    table->sortItems(0, Qt::AscendingOrder);
    QCOMPARE(names(), QStringList({QStringLiteral("a-folder"), QStringLiteral("z-folder"),
                                   QStringLiteral("a.jpg"), QStringLiteral("b.txt"),
                                   QStringLiteral("c.rfm_unknown_extension_987")}));
    QCOMPARE(header->sortIndicatorSection(), 0);
    QCOMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);
    const QPoint nameHeaderCenter(header->sectionViewportPosition(0) + header->sectionSize(0) / 2,
                                  header->height() / 2);
    QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, nameHeaderCenter);
    QCOMPARE(header->sortIndicatorSection(), -1);
    QVERIFY(!header->isSortIndicatorShown());
    QCOMPARE(names(), QStringList({QStringLiteral("z-folder"), QStringLiteral("a-folder"),
                                   QStringLiteral("b.txt"), QStringLiteral("a.jpg"),
                                   QStringLiteral("c.rfm_unknown_extension_987")}));

    table->sortItems(1, Qt::AscendingOrder);
    QCOMPARE(names().sliced(2), QStringList({QStringLiteral("c.rfm_unknown_extension_987"),
                                             QStringLiteral("b.txt"), QStringLiteral("a.jpg")}));
    table->sortItems(1, Qt::DescendingOrder);
    QCOMPARE(names().sliced(2), QStringList({QStringLiteral("a.jpg"), QStringLiteral("b.txt"),
                                             QStringLiteral("c.rfm_unknown_extension_987")}));

    table->sortItems(2, Qt::AscendingOrder);
    QVERIFY(QString::localeAwareCompare(table->item(2, 2)->text().toCaseFolded(),
                                        table->item(3, 2)->text().toCaseFolded()) <= 0);
    QVERIFY(QString::localeAwareCompare(table->item(3, 2)->text().toCaseFolded(),
                                        table->item(4, 2)->text().toCaseFolded()) <= 0);

    table->sortItems(3, Qt::AscendingOrder);
    QCOMPARE(names().sliced(2), QStringList({QStringLiteral("a.jpg"), QStringLiteral("b.txt"),
                                             QStringLiteral("c.rfm_unknown_extension_987")}));
    table->sortItems(3, Qt::DescendingOrder);
    QCOMPARE(names().sliced(2), QStringList({QStringLiteral("b.txt"), QStringLiteral("a.jpg"),
                                             QStringLiteral("c.rfm_unknown_extension_987")}));
    for (int row = 0; row < 2; ++row) {
        QVERIFY(table->item(row, 0)->data(Qt::UserRole).toBool());
    }
    QCOMPARE(table->item(4, 3)->text(), QStringLiteral("—"));

    const QStringList localOrder = names();
    const int sortedSection = header->sortIndicatorSection();
    const Qt::SortOrder sortedOrder = header->sortIndicatorOrder();
    pane.showDirectory(
        {rfm::core::FileSource::Ssh, QStringLiteral("remote-id"), QStringLiteral("/srv")},
        QStringLiteral("sftp://host/srv"), entries);
    QCOMPARE(names(), localOrder);
    QCOMPARE(header->sortIndicatorSection(), sortedSection);
    QCOMPARE(header->sortIndicatorOrder(), sortedOrder);
}

void FileBrowserPaneTest::loadsDirectoryCountsLazilyAndSortsThem()
{
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("z-folder"), 0, {}, true, false},
        {QStringLiteral("a-folder"), 0, {}, true, false},
        {QStringLiteral("denied"), 0, {}, true, false},
        {QStringLiteral("file.bin"), 2, {}, false, false},
    };
    rfm::app::FileBrowserPane pane;
    QSignalSpy requests(&pane, &rfm::app::FileBrowserPane::directoryItemCountRequested);
    const rfm::core::BrowserLocation localLocation{rfm::core::FileSource::Local,
                                                   QString::fromLatin1(rfm::core::LocalMachineId),
                                                   QStringLiteral("/tmp")};
    pane.showDirectory(localLocation, QStringLiteral("file:///tmp"), entries);
    for (int row = 0; row < 3; ++row) {
        QCOMPARE(pane.fileTable()->item(row, 1)->text(), QStringLiteral("…"));
    }
    QCOMPARE(pane.fileTable()->item(3, 1)->text(), QLocale{}.formattedDataSize(2));

    QTRY_COMPARE(requests.size(), 1);
    const auto finishRequest = [&pane, &requests](int index, std::optional<quint64> count) {
        const QList<QVariant> arguments = requests.at(index);
        pane.setDirectoryItemCount(arguments.at(0).value<rfm::core::BrowserLocation>(),
                                   arguments.at(1).toULongLong(), arguments.at(2).toString(),
                                   count);
    };
    finishRequest(0, 14);
    QTRY_COMPARE(requests.size(), 2);
    finishRequest(1, 1);
    QTRY_COMPARE(requests.size(), 3);
    finishRequest(2, std::nullopt);
    QApplication::processEvents();
    QCOMPARE(requests.size(), 3);

    const auto rowNamed = [&pane](const QString& name) {
        for (int row = 0; row < pane.fileTable()->rowCount(); ++row) {
            if (pane.fileTable()->item(row, 0)->text() == name) {
                return row;
            }
        }
        return -1;
    };
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("z-folder")), 1)->text(),
             QStringLiteral("14 items"));
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("a-folder")), 1)->text(),
             QStringLiteral("1 item"));
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("denied")), 1)->text(),
             QStringLiteral("—"));

    pane.fileTable()->sortItems(1, Qt::AscendingOrder);
    QCOMPARE(pane.fileTable()->item(0, 0)->text(), QStringLiteral("a-folder"));
    QCOMPARE(pane.fileTable()->item(1, 0)->text(), QStringLiteral("z-folder"));
    QCOMPARE(pane.fileTable()->item(2, 0)->text(), QStringLiteral("denied"));
    QCOMPARE(pane.fileTable()->item(3, 0)->text(), QStringLiteral("file.bin"));
    pane.fileTable()->sortItems(1, Qt::DescendingOrder);
    QCOMPARE(pane.fileTable()->item(0, 0)->text(), QStringLiteral("z-folder"));
    QCOMPARE(pane.fileTable()->item(1, 0)->text(), QStringLiteral("a-folder"));
    QCOMPARE(pane.fileTable()->item(2, 0)->text(), QStringLiteral("denied"));
    QCOMPARE(pane.fileTable()->item(3, 0)->text(), QStringLiteral("file.bin"));

    const rfm::core::BrowserLocation remoteLocation{
        rfm::core::FileSource::Ssh, QStringLiteral("remote-id"), QStringLiteral("/srv")};
    pane.showDirectory(remoteLocation, QStringLiteral("sftp://host/srv"), entries);
    for (int row = 0; row < 3; ++row) {
        QCOMPARE(pane.fileTable()->item(row, 1)->text(), QStringLiteral("…"));
    }
    QTRY_COMPARE(requests.size(), 4);
    QCOMPARE(requests.constLast().at(0).value<rfm::core::BrowserLocation>(), remoteLocation);
}

void FileBrowserPaneTest::preservesSshDirectoryCountsAcrossRefresh()
{
    const rfm::core::BrowserLocation remoteLocation{
        rfm::core::FileSource::Ssh, QStringLiteral("remote-id"), QStringLiteral("/srv")};
    const QList<rfm::core::RemoteEntry> initialEntries{
        {QStringLiteral("kept"), 0, {}, true, false},
    };
    const QList<rfm::core::RemoteEntry> refreshedEntries{
        {QStringLiteral("kept"), 0, {}, true, false},
        {QStringLiteral("new"), 0, {}, true, false},
    };
    rfm::app::FileBrowserPane pane;
    QSignalSpy requests(&pane, &rfm::app::FileBrowserPane::directoryItemCountRequested);
    const auto rowNamed = [&pane](const QString& name) {
        for (int row = 0; row < pane.fileTable()->rowCount(); ++row) {
            if (pane.fileTable()->item(row, 0)->text() == name) {
                return row;
            }
        }
        return -1;
    };
    const auto finishRequest = [&pane, &requests](int index, std::optional<quint64> count) {
        const QList<QVariant> arguments = requests.at(index);
        pane.setDirectoryItemCount(arguments.at(0).value<rfm::core::BrowserLocation>(),
                                   arguments.at(1).toULongLong(), arguments.at(2).toString(),
                                   count);
    };

    pane.showDirectory(remoteLocation, QStringLiteral("sftp://host/srv"), initialEntries);
    QTRY_COMPARE(requests.size(), 1);
    finishRequest(0, 3);
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("kept")), 1)->text(),
             QStringLiteral("3 items"));

    pane.fileTable()->sortItems(1, Qt::AscendingOrder);
    pane.showDirectory(remoteLocation, QStringLiteral("sftp://host/srv"), refreshedEntries);
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("kept")), 1)->text(),
             QStringLiteral("3 items"));
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("new")), 1)->text(),
             QStringLiteral("…"));
    QCOMPARE(pane.fileTable()->item(0, 0)->text(), QStringLiteral("kept"));
    QCOMPARE(pane.fileTable()->item(1, 0)->text(), QStringLiteral("new"));

    QTRY_COMPARE(requests.size(), 2);
    QSignalSpy itemChanges(pane.fileTable(), &QTableWidget::itemChanged);
    finishRequest(1, 3);
    QCOMPARE(itemChanges.size(), 0);
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("kept")), 1)->text(),
             QStringLiteral("3 items"));
    QTRY_COMPARE(requests.size(), 3);
    finishRequest(2, 2);
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("new")), 1)->text(),
             QStringLiteral("2 items"));

    pane.showDirectory(remoteLocation, QStringLiteral("sftp://host/srv"), refreshedEntries);
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("kept")), 1)->text(),
             QStringLiteral("3 items"));
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("new")), 1)->text(),
             QStringLiteral("2 items"));
    QTRY_COMPARE(requests.size(), 4);
    finishRequest(3, 4);
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("kept")), 1)->text(),
             QStringLiteral("4 items"));
    QTRY_COMPARE(requests.size(), 5);
    finishRequest(4, std::nullopt);
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("new")), 1)->text(),
             QStringLiteral("2 items"));

    pane.showDirectory(remoteLocation, QStringLiteral("sftp://host/srv"), refreshedEntries);
    QTRY_COMPARE(requests.size(), 6);
    pane.showDirectory(remoteLocation, QStringLiteral("sftp://host/srv"), refreshedEntries);
    finishRequest(5, 99);
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("kept")), 1)->text(),
             QStringLiteral("4 items"));
    QTRY_COMPARE(requests.size(), 7);

    const rfm::core::BrowserLocation localLocation{rfm::core::FileSource::Local,
                                                   QString::fromLatin1(rfm::core::LocalMachineId),
                                                   QStringLiteral("/tmp")};
    pane.showDirectory(localLocation, QStringLiteral("file:///tmp"), refreshedEntries);
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("kept")), 1)->text(),
             QStringLiteral("…"));
    QCOMPARE(pane.fileTable()->item(rowNamed(QStringLiteral("new")), 1)->text(),
             QStringLiteral("…"));
}

void FileBrowserPaneTest::rubberBandSelectsMultipleLocalRows()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    rfm::app::FileBrowserPane pane;
    pane.resize(640, 400);
    pane.show();
    pane.showDirectory({rfm::core::FileSource::Local,
                        QString::fromLatin1(rfm::core::LocalMachineId), temporary.path()},
                       temporary.path(),
                       {{QStringLiteral("zero.txt"), 1, {}, false, false},
                        {QStringLiteral("one.txt"), 1, {}, false, false},
                        {QStringLiteral("two.txt"), 1, {}, false, false},
                        {QStringLiteral("three.txt"), 1, {}, false, false}});
    QApplication::processEvents();

    QTableWidget* const table = pane.fileTable();
    QCOMPARE(table->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(table->selectionBehavior(), QAbstractItemView::SelectRows);
    QVERIFY(table->dragEnabled());
    QVERIFY(table->acceptDrops());
    table->selectionModel()->select(table->model()->index(0, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(1, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(pane.selectedEntries().size(), 2);

    const QRect lastRow = table->visualItemRect(table->item(3, 0));
    const QPoint origin(table->viewport()->width() / 2, lastRow.bottom() + 20);
    const QPoint destination(origin.x(), table->visualItemRect(table->item(2, 0)).center().y());
    QVERIFY(table->viewport()->rect().contains(origin));
    QVERIFY(!table->indexAt(origin).isValid());

    QTest::mousePress(table->viewport(), Qt::LeftButton, Qt::NoModifier, origin);
    QTest::mouseMove(table->viewport(), destination);

    auto* const rubberBand =
        table->viewport()->findChild<QRubberBand*>(QStringLiteral("fileSelectionRubberBand"));
    QVERIFY(rubberBand != nullptr);
    QVERIFY(rubberBand->isVisible());
    QTest::mouseRelease(table->viewport(), Qt::LeftButton, Qt::NoModifier, destination);
    QVERIFY(!rubberBand->isVisible());

    const QList<rfm::core::RemoteSelection> selection = pane.selectedEntries();
    QCOMPARE(selection.size(), 2);
    QCOMPARE(selection.at(0).path, QDir(temporary.path()).filePath(QStringLiteral("two.txt")));
    QCOMPARE(selection.at(1).path, QDir(temporary.path()).filePath(QStringLiteral("three.txt")));
    QCOMPARE(table->selectionModel()->selectedIndexes().size(),
             selection.size() * table->columnCount());
}

void FileBrowserPaneTest::controlRubberBandTogglesRemoteRows()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(640, 400);
    pane.show();
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"),
                       {{QStringLiteral("zero.txt"), 1, {}, false, false},
                        {QStringLiteral("one.txt"), 1, {}, false, false},
                        {QStringLiteral("two.txt"), 1, {}, false, false},
                        {QStringLiteral("three.txt"), 1, {}, false, false}});
    QApplication::processEvents();

    QTableWidget* const table = pane.fileTable();
    table->selectionModel()->select(table->model()->index(0, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(2, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    const QRect lastRow = table->visualItemRect(table->item(3, 0));
    const QPoint origin(table->viewport()->width() / 2, lastRow.bottom() + 20);
    const QPoint destination(origin.x(), table->visualItemRect(table->item(2, 0)).center().y());
    QVERIFY(!table->indexAt(origin).isValid());

    QTest::mousePress(table->viewport(), Qt::LeftButton, Qt::ControlModifier, origin);
    QTest::mouseMove(table->viewport(), destination);
    QTest::mouseRelease(table->viewport(), Qt::LeftButton, Qt::ControlModifier, destination);

    const QList<rfm::core::RemoteSelection> selection = pane.selectedEntries();
    QCOMPARE(selection.size(), 2);
    QCOMPARE(selection.at(0).path, QStringLiteral("/srv/zero.txt"));
    QCOMPARE(selection.at(1).path, QStringLiteral("/srv/three.txt"));
    QCOMPARE(table->selectionModel()->selectedIndexes().size(),
             selection.size() * table->columnCount());
}

void FileBrowserPaneTest::dragFromSelectedRowPreservesSelectionAndStartsInternalDrag()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(640, 400);
    pane.show();
    pane.setTransferContext(QStringLiteral("instance"),
                            {QStringLiteral("server.example.test"), 22, 4}, 7);
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"),
                       {{QStringLiteral("zero.txt"), 1, {}, false, false},
                        {QStringLiteral("one.txt"), 1, {}, false, false},
                        {QStringLiteral("two.txt"), 1, {}, false, false},
                        {QStringLiteral("three.txt"), 1, {}, false, false},
                        {QStringLiteral("four.txt"), 1, {}, false, false}});
    QApplication::processEvents();

    QTableWidget* const table = pane.fileTable();
    table->selectionModel()->select(table->model()->index(0, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(2, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QSignalSpy dragStarts(&pane, &rfm::app::FileBrowserPane::internalDragStarted);
    const QPoint pressPosition = table->visualItemRect(table->item(2, 0)).center();
    const QPoint destination = table->visualItemRect(table->item(4, 0)).center();
    QVERIFY((destination - pressPosition).manhattanLength() >= QApplication::startDragDistance());

    QTest::mousePress(table->viewport(), Qt::LeftButton, Qt::NoModifier, pressPosition);
    QCOMPARE(pane.selectedEntries().size(), 2);
    QTimer::singleShot(0, table->viewport(), [table, destination]() {
        QTest::mouseRelease(table->viewport(), Qt::LeftButton, Qt::NoModifier, destination);
    });
    QTest::mouseMove(table->viewport(), destination);

    QCOMPARE(dragStarts.size(), 1);
    const QList<rfm::core::RemoteSelection> selection = pane.selectedEntries();
    QCOMPARE(selection.size(), 2);
    QCOMPARE(selection.at(0).path, QStringLiteral("/srv/zero.txt"));
    QCOMPARE(selection.at(1).path, QStringLiteral("/srv/two.txt"));
    auto* const rubberBand =
        table->viewport()->findChild<QRubberBand*>(QStringLiteral("fileSelectionRubberBand"));
    QVERIFY(rubberBand == nullptr || !rubberBand->isVisible());

    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, pressPosition);
    QCOMPARE(pane.selectedEntries().size(), 1);
    QCOMPARE(pane.selectedEntries().constFirst().path, QStringLiteral("/srv/two.txt"));
}

void FileBrowserPaneTest::dragWithActionModifierPreservesMultipleSelection_data()
{
    QTest::addColumn<Qt::KeyboardModifiers>("modifiers");

    QTest::newRow("control") << Qt::KeyboardModifiers{Qt::ControlModifier};
    QTest::newRow("shift") << Qt::KeyboardModifiers{Qt::ShiftModifier};
    QTest::newRow("control-priority")
        << Qt::KeyboardModifiers{Qt::ControlModifier | Qt::ShiftModifier};
}

void FileBrowserPaneTest::dragWithActionModifierPreservesMultipleSelection()
{
    QFETCH(Qt::KeyboardModifiers, modifiers);

    rfm::app::FileBrowserPane pane;
    pane.resize(640, 400);
    pane.show();
    pane.setTransferContext(QStringLiteral("instance"),
                            {QStringLiteral("server.example.test"), 22, 4}, 7);
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"),
                       {{QStringLiteral("zero.txt"), 1, {}, false, false},
                        {QStringLiteral("one.txt"), 1, {}, false, false},
                        {QStringLiteral("two.txt"), 1, {}, false, false},
                        {QStringLiteral("three.txt"), 1, {}, false, false}});
    QApplication::processEvents();

    QTableWidget* const table = pane.fileTable();
    table->selectionModel()->select(table->model()->index(0, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(2, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    const QPoint pressPosition = table->visualItemRect(table->item(2, 0)).center();
    const QPoint destination = table->visualItemRect(table->item(3, 0)).center();
    QSignalSpy dragStarts(&pane, &rfm::app::FileBrowserPane::internalDragStarted);

    QTest::mousePress(table->viewport(), Qt::LeftButton, modifiers, pressPosition);
    QCOMPARE(pane.selectedEntries().size(), 2);
    QTimer::singleShot(0, table->viewport(), [table, destination, modifiers]() {
        QTest::mouseRelease(table->viewport(), Qt::LeftButton, modifiers, destination);
    });
    QTest::mouseMove(table->viewport(), destination);

    QCOMPARE(dragStarts.size(), 1);
    const QList<rfm::core::RemoteSelection> selection = pane.selectedEntries();
    QCOMPARE(selection.size(), 2);
    QCOMPARE(selection.at(0).path, QStringLiteral("/srv/zero.txt"));
    QCOMPARE(selection.at(1).path, QStringLiteral("/srv/two.txt"));

    if (modifiers == Qt::KeyboardModifiers{Qt::ControlModifier}) {
        QTest::mouseClick(table->viewport(), Qt::LeftButton, modifiers, pressPosition);
        QCOMPARE(pane.selectedEntries().size(), 1);
        QCOMPARE(pane.selectedEntries().constFirst().path, QStringLiteral("/srv/zero.txt"));
    }
}

void FileBrowserPaneTest::emitsNavigationIntentions()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(640, 320);
    pane.show();
    pane.showDirectory(QStringLiteral("/srv/current"), QStringLiteral("sftp://host/srv/current"),
                       {{QStringLiteral("child"), 0, {}, true, false},
                        {QStringLiteral("file.txt"), 1, {}, false, false}});
    QSignalSpy navigation(&pane, &rfm::app::FileBrowserPane::navigationRequested);

    pane.requestParentDirectory();
    QCOMPARE(navigation.size(), 1);
    QCOMPARE(navigation.takeFirst().constFirst().toString(), QStringLiteral("/srv"));

    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 0), Q_ARG(int, 0)));
    QCOMPARE(navigation.size(), 1);
    QCOMPARE(navigation.takeFirst().constFirst().toString(), QStringLiteral("/srv/current/child"));

    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 1), Q_ARG(int, 0)));
    QCOMPARE(navigation.size(), 0);
}

void FileBrowserPaneTest::requestsOpeningLocalFilesWithoutChangingDirectoryOrSshBehavior()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir directory(temporary.path());
    QVERIFY(directory.mkdir(QStringLiteral("folder")));
    const QString filePath = directory.filePath(QStringLiteral("document.txt"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    rfm::app::FileBrowserPane pane;
    const rfm::core::BrowserLocation localLocation{rfm::core::FileSource::Local,
                                                   QString::fromLatin1(rfm::core::LocalMachineId),
                                                   temporary.path()};
    pane.showDirectory(localLocation, QUrl::fromLocalFile(temporary.path()).toDisplayString(),
                       {{QStringLiteral("folder"), 0, {}, true, false},
                        {QStringLiteral("document.txt"), 0, {}, false, false}});
    QSignalSpy navigation(&pane, &rfm::app::FileBrowserPane::locationNavigationRequested);
    QSignalSpy opens(&pane, &rfm::app::FileBrowserPane::fileOpenRequested);

    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 0), Q_ARG(int, 0)));
    QCOMPARE(navigation.size(), 1);
    QCOMPARE(opens.size(), 0);
    QCOMPARE(qvariant_cast<rfm::core::BrowserLocation>(navigation.constFirst().constFirst()).path,
             directory.filePath(QStringLiteral("folder")));

    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 1), Q_ARG(int, 0)));
    QCOMPARE(navigation.size(), 1);
    QCOMPARE(opens.size(), 1);
    const auto opened = qvariant_cast<rfm::core::BrowserLocation>(opens.constFirst().constFirst());
    QCOMPARE(opened.source, rfm::core::FileSource::Local);
    QCOMPARE(opened.machineId, QString::fromLatin1(rfm::core::LocalMachineId));
    QCOMPARE(opened.path, filePath);

    pane.showDirectory(
        {rfm::core::FileSource::Ssh, QStringLiteral("ssh:fixture"), QStringLiteral("/remote")},
        QStringLiteral("sftp://fixture/remote"),
        {{QStringLiteral("remote.txt"), 0, {}, false, false}});
    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 0), Q_ARG(int, 0)));
    QCOMPARE(opens.size(), 1);
}

void FileBrowserPaneTest::navigatesFromCanonicalLoginDirectoryToRemoteRoot()
{
    rfm::app::FileBrowserPane pane;
    QSignalSpy navigation(&pane, &rfm::app::FileBrowserPane::navigationRequested);
    pane.showDirectory(QStringLiteral("/home/alice"), QStringLiteral("sftp://host/home/alice"), {},
                       rfm::app::PaneNavigation::Initial);

    pane.requestParentDirectory();
    QCOMPARE(navigation.takeFirst().constFirst().toString(), QStringLiteral("/home"));
    pane.showDirectory(QStringLiteral("/home"), QStringLiteral("sftp://host/home"), {},
                       rfm::app::PaneNavigation::Normal);
    pane.requestParentDirectory();
    QCOMPARE(navigation.takeFirst().constFirst().toString(), QStringLiteral("/"));
    pane.showDirectory(QStringLiteral("/"), QStringLiteral("sftp://host/"), {},
                       rfm::app::PaneNavigation::Normal);
    pane.requestParentDirectory();

    QCOMPARE(navigation.size(), 0);
    QVERIFY(pane.canGoBack());
}

void FileBrowserPaneTest::activatesDirectoryAndBrokenSymbolicLinksForBackendResolution()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(640, 240);
    pane.show();
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"),
                       {{QStringLiteral("directory-link"), 0, {}, false, true},
                        {QStringLiteral("broken-link"), 0, {}, false, true},
                        {QStringLiteral("regular-file"), 1, {}, false, false}});
    QSignalSpy navigation(&pane, &rfm::app::FileBrowserPane::navigationRequested);

    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 0), Q_ARG(int, 0)));
    QCOMPARE(navigation.takeFirst().constFirst().toString(), QStringLiteral("/srv/directory-link"));
    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 1), Q_ARG(int, 0)));
    QCOMPARE(navigation.takeFirst().constFirst().toString(), QStringLiteral("/srv/broken-link"));
    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 2), Q_ARG(int, 0)));
    QCOMPARE(navigation.size(), 0);
}

void FileBrowserPaneTest::preparesContextSelectionBeforeEmittingIntent()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(640, 320);
    pane.show();
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"),
                       {{QStringLiteral("first.txt"), 1, {}, false, false},
                        {QStringLiteral("second.txt"), 1, {}, false, false}});
    pane.fileTable()->selectRow(0);
    QSignalSpy contextMenus(&pane, &rfm::app::FileBrowserPane::contextMenuRequested);
    const QPoint secondRow =
        pane.fileTable()->visualItemRect(pane.fileTable()->item(1, 0)).center();

    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "customContextMenuRequested",
                                      Qt::DirectConnection, Q_ARG(QPoint, secondRow)));

    QCOMPARE(contextMenus.size(), 1);
    const auto selection = pane.selectedEntries();
    QCOMPARE(selection.size(), 1);
    QCOMPARE(selection.constFirst().path, QStringLiteral("/srv/second.txt"));

    pane.fileTable()->selectAll();
    const QPoint firstRow = pane.fileTable()->visualItemRect(pane.fileTable()->item(0, 0)).center();
    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "customContextMenuRequested",
                                      Qt::DirectConnection, Q_ARG(QPoint, firstRow)));
    QCOMPARE(pane.selectedEntries().size(), 2);

    const QPoint emptyArea(8, pane.fileTable()->viewport()->height() - 2);
    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "customContextMenuRequested",
                                      Qt::DirectConnection, Q_ARG(QPoint, emptyArea)));
    QVERIFY(pane.selectedEntries().isEmpty());
}

void FileBrowserPaneTest::buildsPropertiesForTheEntryUnderTheContextClick()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime modified = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("folder"), 0, modified, true, false},
        {QStringLiteral("first.txt"), 1536, modified, false, false},
        {QStringLiteral("Avatar.hevc.mkv"), 2048, modified, false, false},
        {QStringLiteral("document.pdf"), 4096, modified, false, false},
        {QStringLiteral("README"), 128, modified, false, false},
        {QStringLiteral("unknown.rfm_unknown_extension_987"), 64, modified, false, false},
        {QStringLiteral("movie.mkv"), 512, modified, false, true},
        {QStringLiteral("link"), 512, modified, false, true}};
    rfm::app::FileBrowserPane pane;
    pane.resize(640, 320);
    pane.show();
    QSignalSpy navigationRequests(&pane, &rfm::app::FileBrowserPane::locationNavigationRequested);

    const auto clickRow = [&pane](int row) -> std::optional<rfm::app::FileEntryProperties> {
        const QPoint position =
            pane.fileTable()->visualItemRect(pane.fileTable()->item(row, 0)).center();
        if (!QMetaObject::invokeMethod(pane.fileTable(), "customContextMenuRequested",
                                       Qt::DirectConnection, Q_ARG(QPoint, position))) {
            return std::nullopt;
        }
        return pane.contextEntryProperties();
    };
    const auto displayedType = [](const rfm::app::FileEntryProperties& properties) {
        for (const QString& line : properties.text.split(QChar{'\n'})) {
            if (line.startsWith(QStringLiteral("Type: "))) {
                return line.sliced(6);
            }
        }
        return QString{};
    };

    pane.showDirectory({rfm::core::FileSource::Local,
                        QString::fromLatin1(rfm::core::LocalMachineId), temporary.path()},
                       temporary.path(), entries);
    pane.fileTable()->selectAll();
    auto clicked = clickRow(2);
    QVERIFY(clicked.has_value());
    rfm::app::FileEntryProperties properties = *clicked;
    QCOMPARE(properties.title, QStringLiteral("Avatar.hevc.mkv"));
    QVERIFY(!displayedType(properties).isEmpty());
    QVERIFY(displayedType(properties) != QStringLiteral("File"));
    QVERIFY(properties.text.contains(QStringLiteral("Extension: mkv")));

    const QString expectedSize = QLocale{}.formattedDataSize(2048);
    QVERIFY(properties.text.contains(QStringLiteral("Size: %1").arg(expectedSize)));

    QVERIFY(properties.text.contains(
        QDir(temporary.path()).filePath(QStringLiteral("Avatar.hevc.mkv"))));

    QVERIFY(!properties.text.contains(QStringLiteral("first.txt")));

    QStringList localTypes;
    for (const int row : {1, 2, 3}) {
        clicked = clickRow(row);
        QVERIFY(clicked.has_value());
        const QString type = displayedType(*clicked);
        QVERIFY(!type.isEmpty());
        QVERIFY(type != QStringLiteral("File"));
        localTypes.push_back(type);
    }

    // Pour les fichiers sans type MIME spécifique, on vérifie qu'ils ne sont pas identifiés comme
    // "File" mais plutôt avec une description locale ou le nom technique
    for (const int row : {4, 5}) {
        clicked = clickRow(row);
        QVERIFY(clicked.has_value());
        const QString actualType = displayedType(*clicked);
        // On ne vérifie plus l'exactitude du texte, mais qu'il n'est pas égal à "File"
        QVERIFY(actualType != QStringLiteral("File"));
        localTypes.push_back(actualType);
    }

    for (const int row : {6, 7}) {
        clicked = clickRow(row);
        QVERIFY(clicked.has_value());
        QCOMPARE(displayedType(*clicked), QStringLiteral("Symbolic link"));
    }
    QVERIFY(!clicked->text.contains(QStringLiteral("Extension: mkv")));
    clicked = clickRow(6);
    QVERIFY(clicked.has_value());
    QVERIFY(clicked->text.contains(QStringLiteral("Extension: mkv")));

    clicked = clickRow(0);
    QVERIFY(clicked.has_value());
    properties = *clicked;
    QCOMPARE(properties.title, QStringLiteral("folder"));

    // Vérifier que le type est correctement identifié (utilisant la description locale)
    const QString expectedFolderType =
        QMimeDatabase{}.mimeTypeForName(QStringLiteral("inode/directory")).comment().trimmed();
    QVERIFY(!expectedFolderType.isEmpty());
    QVERIFY(properties.text.contains(QStringLiteral("Type: ") + expectedFolderType));

    QVERIFY(!properties.text.contains(QStringLiteral("Size:")));

    pane.showDirectory(
        {rfm::core::FileSource::Ssh, QStringLiteral("remote-id"), QStringLiteral("/srv")},
        QStringLiteral("sftp://host/srv"), entries);
    for (int index = 0; index < localTypes.size(); ++index) {
        clicked = clickRow(index + 1);
        QVERIFY(clicked.has_value());
        QCOMPARE(displayedType(*clicked), localTypes.at(index));
        QVERIFY(clicked->text.contains(QStringLiteral("Path: /srv/")));
        QVERIFY(clicked->text.contains(QStringLiteral("Modified:")));
    }
    clicked = clickRow(0);
    QVERIFY(clicked.has_value());
    properties = *clicked;
    QCOMPARE(properties.title, QStringLiteral("folder"));
    QVERIFY(properties.text.contains(QStringLiteral("Path: /srv/folder")));

    const QString unsafeName = QStringLiteral("report\nType: forged.mkv");
    pane.showDirectory({rfm::core::FileSource::Ssh, QStringLiteral("remote-id"),
                        QStringLiteral("/srv\nPath: forged")},
                       QStringLiteral("sftp://host/srv"),
                       {{unsafeName, 1, modified, false, false}});
    clicked = clickRow(0);
    QVERIFY(clicked.has_value());
    QCOMPARE(clicked->title, QStringLiteral("report Type: forged.mkv"));
    QVERIFY(!clicked->title.contains(QChar{'\n'}));
    const QStringList unsafeLines = clicked->text.split(QChar{'\n'});
    QCOMPARE(std::ranges::count_if(
                 unsafeLines,
                 [](const QString& line) { return line.startsWith(QStringLiteral("Type: ")); }),
             1);
    QCOMPARE(std::ranges::count_if(
                 unsafeLines,
                 [](const QString& line) { return line.startsWith(QStringLiteral("Path: ")); }),
             1);
    QVERIFY(!clicked->text.contains(QStringLiteral("\nType: forged")));
    QVERIFY(!clicked->text.contains(QStringLiteral("\nPath: forged")));
    QCOMPARE(navigationRequests.size(), 0);
}

void FileBrowserPaneTest::restoresSelectionAndScrollOnRefresh()
{
    rfm::app::FileBrowserPane pane;
    pane.resize(640, 180);
    pane.show();
    QList<rfm::core::RemoteEntry> entries;
    for (int index = 0; index < 80; ++index) {
        entries.push_back({QStringLiteral("file-%1.txt").arg(index, 2, 10, QChar{'0'}),
                           static_cast<quint64>(index),
                           {},
                           false,
                           false});
    }
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"), entries);
    pane.fileTable()->selectionModel()->select(pane.fileTable()->model()->index(40, 0),
                                               QItemSelectionModel::Select |
                                                   QItemSelectionModel::Rows);
    pane.fileTable()->verticalScrollBar()->setValue(25);
    const int scrollPosition = pane.fileTable()->verticalScrollBar()->value();

    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"), entries);

    const QModelIndexList selected = pane.fileTable()->selectionModel()->selectedRows(0);
    QCOMPARE(selected.size(), 1);
    QCOMPARE(pane.fileTable()->item(selected.constFirst().row(), 0)->text(),
             QStringLiteral("file-40.txt"));
    QCOMPARE(pane.fileTable()->verticalScrollBar()->value(), scrollPosition);
}

void FileBrowserPaneTest::appliesPendingSelectionAfterOperation()
{
    rfm::app::FileBrowserPane pane;
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"), {});
    pane.setPendingSelectionNames({QStringLiteral("created.txt")});
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"),
                       {{QStringLiteral("created.txt"), 10, {}, false, false},
                        {QStringLiteral("other.txt"), 10, {}, false, false}});

    const QModelIndexList selected = pane.fileTable()->selectionModel()->selectedRows(0);
    QCOMPARE(selected.size(), 1);
    QCOMPARE(pane.fileTable()->item(selected.constFirst().row(), 0)->text(),
             QStringLiteral("created.txt"));
}

void FileBrowserPaneTest::workspaceStartsSingleAndTogglesSplit()
{
    rfm::app::PaneWorkspace workspace;
    QCOMPARE(workspace.visiblePaneIds().size(), 1);
    QVERIFY(!workspace.isSplit());
    QCOMPARE(workspace.activePane(), workspace.primaryPane());
    const quint64 primaryId = workspace.paneId(workspace.primaryPane());
    QVERIFY(primaryId != 0);

    QSignalSpy visibility(&workspace, &rfm::app::PaneWorkspace::paneVisibilityChanged);
    workspace.setSplit(true);
    QCOMPARE(workspace.visiblePaneIds().size(), 2);
    QVERIFY(workspace.isSplit());
    QVERIFY(workspace.otherVisiblePane() != nullptr);
    const quint64 secondaryId = workspace.paneId(workspace.otherVisiblePane());
    QVERIFY(secondaryId != 0);
    QVERIFY(secondaryId != primaryId);

    workspace.setSplit(false);
    QCOMPARE(workspace.visiblePaneIds().size(), 1);
    QVERIFY(!workspace.isSplit());
    QCOMPARE(workspace.activePane(), workspace.primaryPane());
    QVERIFY(!workspace.primaryPane()->isHidden());
    QVERIFY(workspace.pane(secondaryId)->isHidden());
    QCOMPARE(workspace.primaryPane(), workspace.pane(primaryId));
    QCOMPARE(visibility.size(), 2);

    workspace.setSplit(true);
    QCOMPARE(workspace.paneId(workspace.otherVisiblePane()), secondaryId);
    QCOMPARE(workspace.visiblePaneIds().size(), 2);
    QCOMPARE(workspace.paneId(workspace.primaryPane()), primaryId);
}

void FileBrowserPaneTest::workspaceTracksActivePaneFromInteraction()
{
    rfm::app::PaneWorkspace workspace;
    workspace.resize(900, 400);
    workspace.setSplit(true);
    workspace.show();
    rfm::app::FileBrowserPane* const secondary = workspace.otherVisiblePane();
    rfm::app::FileBrowserPane* const primary = workspace.primaryPane();
    const quint64 primaryId = workspace.paneId(primary);
    const quint64 secondaryId = workspace.paneId(secondary);
    const QColor normalWindow = workspace.palette().color(QPalette::Window);
    primary->showDirectory(QStringLiteral("/one"), QStringLiteral("/one"), {},
                           rfm::app::PaneNavigation::Initial);
    primary->showDirectory(QStringLiteral("/one/child"), QStringLiteral("/one/child"),
                           {{QStringLiteral("kept.txt"), 1, {}, false, false}},
                           rfm::app::PaneNavigation::Normal);
    primary->fileTable()->selectRow(0);
    QSignalSpy activeChanges(&workspace, &rfm::app::PaneWorkspace::activePaneChanged);

    QTest::mouseClick(secondary->fileTable()->viewport(), Qt::LeftButton);

    QCOMPARE(workspace.activePane(), secondary);
    QCOMPARE(activeChanges.size(), 1);
    QCOMPARE(workspace.otherVisiblePane(secondaryId), primary);
    QCOMPARE(workspace.otherVisiblePane(primaryId), secondary);
    QVERIFY(secondary->property("activePane").toBool());
    QVERIFY(!primary->property("activePane").toBool());
    QVERIFY(secondary->styleSheet().contains(QStringLiteral("palette(highlight)")));
    QVERIFY(secondary->palette().color(QPalette::Window) != normalWindow);
    QCOMPARE(primary->palette().color(QPalette::Window), normalWindow);
    QVERIFY(secondary->pathEdit()->palette().color(QPalette::Base) !=
            primary->pathEdit()->palette().color(QPalette::Base));

    workspace.setSplit(false);
    QCOMPARE(workspace.activePane(), secondary);
    QVERIFY(!secondary->isHidden());
    QVERIFY(primary->isHidden());
    QCOMPARE(workspace.visiblePaneIds(), QList<quint64>{secondaryId});
    QCOMPARE(primary->currentPath(), QStringLiteral("/one/child"));
    QCOMPARE(primary->selectedEntries().size(), 1);
    QVERIFY(primary->canGoBack());

    workspace.setSplit(true);
    QCOMPARE(workspace.activePane(), secondary);
    QCOMPARE(workspace.visiblePaneIds().size(), 2);
    QCOMPARE(workspace.paneId(primary), primaryId);
    QCOMPARE(workspace.paneId(secondary), secondaryId);
    QCOMPARE(primary->currentPath(), QStringLiteral("/one/child"));
    QCOMPARE(primary->selectedEntries().size(), 1);
    QVERIFY(primary->canGoBack());

    QTest::mouseClick(primary->fileTable()->viewport(), Qt::LeftButton);
    QCOMPARE(workspace.activePane(), primary);
    QVERIFY(primary->property("activePane").toBool());
    QVERIFY(!secondary->property("activePane").toBool());
    QVERIFY(primary->palette().color(QPalette::Window) != normalWindow);
    QCOMPARE(secondary->palette().color(QPalette::Window), normalWindow);
    QVERIFY(primary->pathEdit()->palette().color(QPalette::Base) !=
            secondary->pathEdit()->palette().color(QPalette::Base));
}

void FileBrowserPaneTest::workspaceKeepsPanePathsAndSelectionsIndependent()
{
    rfm::app::PaneWorkspace workspace;
    workspace.setSplit(true);
    rfm::app::FileBrowserPane* const primary = workspace.primaryPane();
    rfm::app::FileBrowserPane* const secondary = workspace.otherVisiblePane();
    primary->showDirectory(QStringLiteral("/one"), QStringLiteral("sftp://host/one"),
                           {{QStringLiteral("first.txt"), 1, {}, false, false}});
    secondary->showDirectory(QStringLiteral("/two"), QStringLiteral("sftp://host/two"),
                             {{QStringLiteral("second.txt"), 1, {}, false, false}});
    primary->fileTable()->selectRow(0);

    QCOMPARE(primary->currentPath(), QStringLiteral("/one"));
    QCOMPARE(secondary->currentPath(), QStringLiteral("/two"));
    QCOMPARE(primary->selectedEntries().size(), 1);
    QCOMPARE(secondary->selectedEntries().size(), 0);
}

void FileBrowserPaneTest::navigationHistorySupportsBackForwardAndBranching()
{
    rfm::app::FileBrowserPane pane;
    QSignalSpy navigation(&pane, &rfm::app::FileBrowserPane::navigationRequested);
    pane.showDirectory(QStringLiteral("/a"), QStringLiteral("/a"), {},
                       rfm::app::PaneNavigation::Initial);
    pane.navigateTo(QStringLiteral("/b"));
    QCOMPARE(navigation.takeFirst().constFirst().toString(), QStringLiteral("/b"));
    pane.showDirectory(QStringLiteral("/b"), QStringLiteral("/b"), {},
                       rfm::app::PaneNavigation::Normal);
    pane.navigateTo(QStringLiteral("/c"));
    pane.showDirectory(QStringLiteral("/c"), QStringLiteral("/c"), {},
                       rfm::app::PaneNavigation::Normal);
    QVERIFY(pane.canGoBack());
    QVERIFY(!pane.canGoForward());

    navigation.clear();
    pane.requestBack();
    QCOMPARE(navigation.constFirst().constFirst().toString(), QStringLiteral("/b"));
    pane.showDirectory(QStringLiteral("/b"), QStringLiteral("/b"), {},
                       rfm::app::PaneNavigation::Back);
    QVERIFY(pane.canGoForward());
    pane.requestForward();
    QCOMPARE(navigation.constLast().constFirst().toString(), QStringLiteral("/c"));
    pane.showDirectory(QStringLiteral("/c"), QStringLiteral("/c"), {},
                       rfm::app::PaneNavigation::Forward);

    pane.requestBack();
    pane.showDirectory(QStringLiteral("/b"), QStringLiteral("/b"), {},
                       rfm::app::PaneNavigation::Back);
    pane.navigateTo(QStringLiteral("/d"));
    pane.showDirectory(QStringLiteral("/d"), QStringLiteral("/d"), {},
                       rfm::app::PaneNavigation::Normal);
    QVERIFY(!pane.canGoForward());
}

void FileBrowserPaneTest::parentAndRefreshHaveCorrectHistorySemantics()
{
    rfm::app::FileBrowserPane pane;
    QSignalSpy navigation(&pane, &rfm::app::FileBrowserPane::navigationRequested);
    pane.showDirectory(QStringLiteral("/one/two"), QStringLiteral("/one/two"), {},
                       rfm::app::PaneNavigation::Initial);
    pane.requestParentDirectory();
    QCOMPARE(navigation.constFirst().constFirst().toString(), QStringLiteral("/one"));
    pane.showDirectory(QStringLiteral("/one"), QStringLiteral("/one"), {},
                       rfm::app::PaneNavigation::Normal);
    QVERIFY(pane.canGoBack());

    navigation.clear();
    pane.requestRefresh();
    QCOMPARE(navigation.size(), 1);
    pane.showDirectory(QStringLiteral("/one"), QStringLiteral("/one"), {},
                       rfm::app::PaneNavigation::Refresh);
    pane.requestBack();
    QCOMPARE(navigation.constLast().constFirst().toString(), QStringLiteral("/one/two"));

    const qsizetype beforeFailedNavigation = navigation.size();
    pane.navigateTo(QStringLiteral("/missing"));
    QCOMPARE(navigation.size(), beforeFailedNavigation + 1);
    pane.requestBack();
    QCOMPARE(navigation.constLast().constFirst().toString(), QStringLiteral("/one/two"));
}

void FileBrowserPaneTest::workspaceHistoriesAreIndependent()
{
    rfm::app::PaneWorkspace workspace;
    workspace.setSplit(true);
    auto* const primary = workspace.primaryPane();
    auto* const secondary = workspace.otherVisiblePane();
    primary->showDirectory(QStringLiteral("/a"), QStringLiteral("/a"), {},
                           rfm::app::PaneNavigation::Initial);
    secondary->showDirectory(QStringLiteral("/x"), QStringLiteral("/x"), {},
                             rfm::app::PaneNavigation::Initial);
    primary->showDirectory(QStringLiteral("/b"), QStringLiteral("/b"), {},
                           rfm::app::PaneNavigation::Normal);

    QVERIFY(primary->canGoBack());
    QVERIFY(!secondary->canGoBack());
}

void FileBrowserPaneTest::mosaicDropUsesTheExistingTransferPipeline()
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir destinationDirectory;
    QVERIFY(sourceDirectory.isValid());
    QVERIFY(destinationDirectory.isValid());
    QFile sourceFile(QDir(sourceDirectory.path()).filePath(QStringLiteral("item.txt")));
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    sourceFile.close();
    QVERIFY(QDir(destinationDirectory.path()).mkdir(QStringLiteral("child")));

    rfm::app::FileBrowserPane source;
    source.setTransferContext(QStringLiteral("instance"), {}, 1);
    source.showDirectory({rfm::core::FileSource::Local,
                          QString::fromLatin1(rfm::core::LocalMachineId), sourceDirectory.path()},
                         sourceDirectory.path(),
                         {{QStringLiteral("item.txt"), 1, {}, false, false, false}});
    source.fileTable()->selectRow(0);

    rfm::app::FileBrowserPane destination;
    destination.setTransferContext(QStringLiteral("instance"), {}, 2);
    destination.showDirectory(
        {rfm::core::FileSource::Local, QString::fromLatin1(rfm::core::LocalMachineId),
         destinationDirectory.path()},
        destinationDirectory.path(), {{QStringLiteral("child"), 0, {}, true, false, false}});
    destination.setViewMode(rfm::app::ViewMode::Mosaic);
    destination.resize(480, 320);
    destination.show();
    QTest::qWait(1);

    QMimeData mime;
    mime.setData(rfm::core::InternalTransferMimeType, source.createInternalDragData());
    QSignalSpy drops(&destination, &rfm::app::FileBrowserPane::internalDropRequested);
    auto* const mosaic = destination.mosaicView();
    const QPoint position = mosaic->visualRect(mosaic->model()->index(0, 0)).center();
    QDragEnterEvent enter(position, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(mosaic->viewport(), &enter);
    QVERIFY(enter.isAccepted());
    QCOMPARE(enter.dropAction(), Qt::CopyAction);
    QCOMPARE(mosaic->property("dropState").toString(), QStringLiteral("valid"));
    QDropEvent drop(QPointF(position), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(mosaic->viewport(), &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(drops.size(), 1);
    QCOMPARE(drops.constFirst().at(2).toString(),
             QDir(destinationDirectory.path()).filePath(QStringLiteral("child")));
}

void FileBrowserPaneTest::constructsAndAcceptsOnlyInternalDragPayloads()
{
    rfm::app::FileBrowserPane pane;
    pane.setTransferContext(QStringLiteral("instance"),
                            {QStringLiteral("server.example.test"), 22, 4}, 9);
    pane.showDirectory(QStringLiteral("/source"), QStringLiteral("/source"),
                       {{QStringLiteral("a.txt"), 1, {}, false, false},
                        {QStringLiteral("folder"), 0, {}, true, false}});
    QCOMPARE(pane.fileTable()->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(pane.fileTable()->selectionBehavior(), QAbstractItemView::SelectRows);
    QCOMPARE(pane.fileTable()->dragDropMode(), QAbstractItemView::DragDrop);
    QVERIFY(pane.fileTable()->dragEnabled());
    QVERIFY(pane.fileTable()->acceptDrops());
    QVERIFY(pane.fileTable()->viewport()->acceptDrops());
    pane.fileTable()->selectAll();

    const QByteArray data = pane.createInternalDragData();
    const auto decoded = rfm::core::decodeInternalTransfer(data);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->source, rfm::core::FileSource::Ssh);
    QCOMPARE(decoded->sourceMachineId, QStringLiteral("ssh"));
    QCOMPARE(decoded->sourcePaneId, quint64{9});
    QCOMPARE(decoded->sources.size(), 2);

    QSignalSpy drops(&pane, &rfm::app::FileBrowserPane::internalDropRequested);
    QMimeData external;
    external.setText(QStringLiteral("file:///tmp/external.txt"));
    QDropEvent externalDrop(QPointF(10, 10), Qt::CopyAction, &external, Qt::LeftButton,
                            Qt::NoModifier);
    QApplication::sendEvent(pane.fileTable()->viewport(), &externalDrop);
    QCOMPARE(drops.size(), 0);
    QVERIFY(!externalDrop.isAccepted());
}

void FileBrowserPaneTest::resolvesDropOnCurrentDirectoryAndSubfolder()
{
    const rfm::core::RemoteConnectionIdentity connection{QStringLiteral("server.example.test"), 22,
                                                         4};
    rfm::app::FileBrowserPane source;
    source.setTransferContext(QStringLiteral("instance"), connection, 1);
    source.showDirectory(QStringLiteral("/source"), QStringLiteral("/source"),
                         {{QStringLiteral("a.txt"), 1, {}, false, false}});
    source.fileTable()->selectRow(0);

    rfm::app::FileBrowserPane destination;
    destination.resize(640, 320);
    destination.show();
    destination.setTransferContext(QStringLiteral("instance"), connection, 2);
    destination.showDirectory(QStringLiteral("/target"), QStringLiteral("/target"),
                              {{QStringLiteral("child"), 0, {}, true, false},
                               {QStringLiteral("plain.txt"), 1, {}, false, false}});
    QApplication::processEvents();

    QMimeData mime;
    mime.setData(rfm::core::InternalTransferMimeType, source.createInternalDragData());
    QSignalSpy drops(&destination, &rfm::app::FileBrowserPane::internalDropRequested);

    const QPoint childPosition =
        destination.fileTable()->visualItemRect(destination.fileTable()->item(0, 0)).center();
    QDragEnterEvent childEnter(childPosition, Qt::CopyAction | Qt::MoveAction, &mime,
                               Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(destination.fileTable()->viewport(), &childEnter);
    QVERIFY(childEnter.isAccepted());
    QCOMPARE(childEnter.dropAction(), Qt::CopyAction);
    QCOMPARE(destination.fileTable()->property("dropState").toString(), QStringLiteral("valid"));
    QDropEvent childDrop(QPointF(childPosition), Qt::CopyAction | Qt::MoveAction, &mime,
                         Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(destination.fileTable()->viewport(), &childDrop);
    QVERIFY(childDrop.isAccepted());
    QCOMPARE(childDrop.dropAction(), Qt::CopyAction);
    QCOMPARE(destination.fileTable()->property("dropState").toString(), QStringLiteral("none"));
    QCOMPARE(drops.size(), 1);
    const QList<QVariant> childArguments = drops.takeFirst();
    QCOMPARE(childArguments.at(1).value<rfm::core::InternalTransferAction>(),
             rfm::core::InternalTransferAction::Copy);
    QCOMPARE(childArguments.at(2).toString(), QStringLiteral("/target/child"));
    QVERIFY(!childArguments.at(3).toBool());

    const QPoint filePosition =
        destination.fileTable()->visualItemRect(destination.fileTable()->item(1, 0)).center();
    QDragEnterEvent fileEnter(filePosition, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                              Qt::ControlModifier);
    QApplication::sendEvent(destination.fileTable()->viewport(), &fileEnter);
    QVERIFY(fileEnter.isAccepted());
    QCOMPARE(fileEnter.dropAction(), Qt::CopyAction);
    QDropEvent fileDrop(QPointF(filePosition), Qt::CopyAction | Qt::MoveAction, &mime,
                        Qt::LeftButton, Qt::ControlModifier);
    QApplication::sendEvent(destination.fileTable()->viewport(), &fileDrop);
    QVERIFY(fileDrop.isAccepted());
    QCOMPARE(fileDrop.dropAction(), Qt::CopyAction);
    QCOMPARE(drops.size(), 1);
    const QList<QVariant> fileArguments = drops.takeFirst();
    QCOMPARE(fileArguments.at(1).value<rfm::core::InternalTransferAction>(),
             rfm::core::InternalTransferAction::Copy);
    QCOMPARE(fileArguments.at(2).toString(), QStringLiteral("/target"));
    QVERIFY(fileArguments.at(3).toBool());

    const QPoint emptyPosition(10, destination.fileTable()->viewport()->height() - 2);
    QDragEnterEvent shiftedEmptyEnter(emptyPosition, Qt::CopyAction | Qt::MoveAction, &mime,
                                      Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(destination.fileTable()->viewport(), &shiftedEmptyEnter);
    QVERIFY(shiftedEmptyEnter.isAccepted());
    QCOMPARE(shiftedEmptyEnter.dropAction(), Qt::MoveAction);
    QDropEvent emptyDrop(QPointF(emptyPosition), Qt::CopyAction | Qt::MoveAction, &mime,
                         Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(destination.fileTable()->viewport(), &emptyDrop);
    QVERIFY(emptyDrop.isAccepted());
    QCOMPARE(emptyDrop.dropAction(), Qt::MoveAction);
    QCOMPARE(drops.size(), 1);
    const QList<QVariant> emptyArguments = drops.takeFirst();
    QCOMPARE(emptyArguments.at(1).value<rfm::core::InternalTransferAction>(),
             rfm::core::InternalTransferAction::Move);
    QCOMPARE(emptyArguments.at(2).toString(), QStringLiteral("/target"));
    QVERIFY(emptyArguments.at(3).toBool());

    QDragEnterEvent priorityEnter(emptyPosition, Qt::CopyAction | Qt::MoveAction, &mime,
                                  Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::sendEvent(destination.fileTable()->viewport(), &priorityEnter);
    QVERIFY(priorityEnter.isAccepted());
    QCOMPARE(priorityEnter.dropAction(), Qt::CopyAction);
    QDropEvent priorityDrop(QPointF(emptyPosition), Qt::CopyAction | Qt::MoveAction, &mime,
                            Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::sendEvent(destination.fileTable()->viewport(), &priorityDrop);
    QVERIFY(priorityDrop.isAccepted());
    QCOMPARE(priorityDrop.dropAction(), Qt::CopyAction);
    QCOMPARE(drops.size(), 1);
    const QList<QVariant> priorityArguments = drops.takeFirst();
    QCOMPARE(priorityArguments.at(1).value<rfm::core::InternalTransferAction>(),
             rfm::core::InternalTransferAction::Copy);
    QCOMPARE(priorityArguments.at(2).toString(), QStringLiteral("/target"));
    QVERIFY(priorityArguments.at(3).toBool());

    destination.showDirectory(QStringLiteral("/source"), QStringLiteral("/source"), {});
    QDragEnterEvent invalidEnter(emptyPosition, Qt::CopyAction | Qt::MoveAction, &mime,
                                 Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(destination.fileTable()->viewport(), &invalidEnter);
    QVERIFY(invalidEnter.isAccepted());
    QCOMPARE(invalidEnter.dropAction(), Qt::IgnoreAction);
    QCOMPARE(destination.fileTable()->property("dropState").toString(), QStringLiteral("invalid"));
    QDropEvent invalidDrop(QPointF(emptyPosition), Qt::CopyAction | Qt::MoveAction, &mime,
                           Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(destination.fileTable()->viewport(), &invalidDrop);
    QVERIFY(!invalidDrop.isAccepted());
    QCOMPARE(invalidDrop.dropAction(), Qt::IgnoreAction);
    QCOMPARE(drops.size(), 0);
}

void FileBrowserPaneTest::constructsLocalPayloadAndResolvesLocalDropDestinations()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkpath(QStringLiteral("target/child")));
    QFile sourceFile(root.filePath(QStringLiteral("source/a.txt")));
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    sourceFile.close();
    QVERIFY(QDir(root.filePath(QStringLiteral("source"))).mkdir(QStringLiteral("folder")));
    QFile targetFile(root.filePath(QStringLiteral("target/plain.txt")));
    QVERIFY(targetFile.open(QIODevice::WriteOnly));
    targetFile.close();

    const auto localLocation = [](const QString& path) {
        return rfm::core::BrowserLocation{rfm::core::FileSource::Local,
                                          QString::fromLatin1(rfm::core::LocalMachineId), path};
    };
    rfm::app::FileBrowserPane source;
    source.setTransferContext(QStringLiteral("instance"), {}, 1);
    source.showDirectory(localLocation(root.filePath(QStringLiteral("source"))),
                         QStringLiteral("source"),
                         {{QStringLiteral("a.txt"), 1, {}, false, false},
                          {QStringLiteral("folder"), 0, {}, true, false}});
    QVERIFY(source.fileTable()->dragEnabled());
    source.fileTable()->selectAll();
    const QByteArray data = source.createInternalDragData();
    const auto decoded = rfm::core::decodeInternalTransfer(data);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->source, rfm::core::FileSource::Local);
    QCOMPARE(decoded->sources.size(), 2);
    QCOMPARE(decoded->sources.at(0).path, sourceFile.fileName());
    QCOMPARE(decoded->sources.at(1).path, root.filePath(QStringLiteral("source/folder")));

    rfm::app::FileBrowserPane destination;
    destination.resize(640, 320);
    destination.show();
    destination.setTransferContext(QStringLiteral("instance"), {}, 2);
    destination.showDirectory(localLocation(root.filePath(QStringLiteral("target"))),
                              QStringLiteral("target"),
                              {{QStringLiteral("child"), 0, {}, true, false},
                               {QStringLiteral("plain.txt"), 1, {}, false, false}});
    QApplication::processEvents();
    QVERIFY(destination.fileTable()->dragEnabled());
    QVERIFY(destination.fileTable()->acceptDrops());

    QMimeData mime;
    mime.setData(rfm::core::InternalTransferMimeType, data);
    QSignalSpy drops(&destination, &rfm::app::FileBrowserPane::internalDropRequested);
    const auto sendDrop = [&destination, &mime,
                           &drops](const QPoint& position, Qt::KeyboardModifiers modifiers,
                                   rfm::core::InternalTransferAction expectedAction,
                                   const QString& expectedDestination) {
        const Qt::DropAction expectedDropAction =
            expectedAction == rfm::core::InternalTransferAction::Copy ? Qt::CopyAction
                                                                      : Qt::MoveAction;
        QDragEnterEvent enter(position, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                              modifiers);
        QApplication::sendEvent(destination.fileTable()->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QCOMPARE(enter.dropAction(), expectedDropAction);
        QCOMPARE(destination.fileTable()->property("dropState").toString(),
                 QStringLiteral("valid"));
        QDropEvent drop(QPointF(position), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                        modifiers);
        QApplication::sendEvent(destination.fileTable()->viewport(), &drop);
        QVERIFY(drop.isAccepted());
        QCOMPARE(drop.dropAction(), expectedDropAction);
        QCOMPARE(drops.size(), 1);
        const QList<QVariant> arguments = drops.takeFirst();
        QCOMPARE(arguments.at(1).value<rfm::core::InternalTransferAction>(), expectedAction);
        QCOMPARE(arguments.at(2).toString(), expectedDestination);
        QCOMPARE(arguments.at(3).toBool(),
                 modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::ShiftModifier));
    };

    const QPoint childPosition =
        destination.fileTable()->visualItemRect(destination.fileTable()->item(0, 0)).center();
    sendDrop(childPosition, Qt::NoModifier, rfm::core::InternalTransferAction::Copy,
             root.filePath(QStringLiteral("target/child")));
    const QPoint filePosition =
        destination.fileTable()->visualItemRect(destination.fileTable()->item(1, 0)).center();
    sendDrop(filePosition, Qt::ControlModifier, rfm::core::InternalTransferAction::Copy,
             root.filePath(QStringLiteral("target")));
    const QPoint backgroundPosition(10, destination.fileTable()->viewport()->height() - 2);
    QVERIFY(!destination.fileTable()->indexAt(backgroundPosition).isValid());
    sendDrop(backgroundPosition, Qt::ShiftModifier, rfm::core::InternalTransferAction::Move,
             root.filePath(QStringLiteral("target")));
    sendDrop(backgroundPosition, Qt::ControlModifier | Qt::ShiftModifier,
             rfm::core::InternalTransferAction::Copy, root.filePath(QStringLiteral("target")));
}

void FileBrowserPaneTest::acceptsCrossSourceCopyIntentionsAndPreservesPayload()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("target")));
    for (const QString& name : {QStringLiteral("a.txt"), QStringLiteral("b.txt")}) {
        QFile sourceFile(root.filePath(QStringLiteral("source/") + name));
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    }

    rfm::app::FileBrowserPane local;
    local.setTransferContext(QStringLiteral("instance"), {}, 1);
    local.showDirectory({rfm::core::FileSource::Local,
                         QString::fromLatin1(rfm::core::LocalMachineId),
                         root.filePath(QStringLiteral("source"))},
                        QStringLiteral("source"),
                        {{QStringLiteral("a.txt"), 1, {}, false, false},
                         {QStringLiteral("b.txt"), 1, {}, false, false}});
    local.fileTable()->selectAll();
    const QByteArray localData = local.createInternalDragData();
    const auto localPayload = rfm::core::decodeInternalTransfer(localData);
    QVERIFY(localPayload.has_value());
    QCOMPARE(localPayload->sources.size(), 2);

    const rfm::core::RemoteConnectionIdentity connection{QStringLiteral("server.example.test"), 22,
                                                         4};
    rfm::app::FileBrowserPane ssh;
    ssh.resize(640, 320);
    ssh.show();
    ssh.setTransferContext(QStringLiteral("instance"), connection, 2);
    ssh.showDirectory(QStringLiteral("/target"), QStringLiteral("/target"), {});
    QApplication::processEvents();

    QMimeData localMime;
    localMime.setData(rfm::core::InternalTransferMimeType, localData);
    const QPoint background(10, ssh.fileTable()->viewport()->height() - 2);
    QDragEnterEvent normalEnter(background, Qt::CopyAction | Qt::MoveAction, &localMime,
                                Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(ssh.fileTable()->viewport(), &normalEnter);
    QVERIFY(normalEnter.isAccepted());
    QCOMPARE(normalEnter.dropAction(), Qt::CopyAction);
    QDragMoveEvent shiftedMove(background, Qt::CopyAction | Qt::MoveAction, &localMime,
                               Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(ssh.fileTable()->viewport(), &shiftedMove);
    QVERIFY(!shiftedMove.isAccepted());
    QCOMPARE(shiftedMove.dropAction(), Qt::IgnoreAction);
    QDragMoveEvent restoredMove(background, Qt::CopyAction | Qt::MoveAction, &localMime,
                                Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(ssh.fileTable()->viewport(), &restoredMove);
    QVERIFY(restoredMove.isAccepted());
    QCOMPARE(restoredMove.dropAction(), Qt::CopyAction);
    QDragMoveEvent controlShiftMove(background, Qt::CopyAction | Qt::MoveAction, &localMime,
                                    Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier);
    QApplication::sendEvent(ssh.fileTable()->viewport(), &controlShiftMove);
    QVERIFY(controlShiftMove.isAccepted());
    QCOMPARE(controlShiftMove.dropAction(), Qt::CopyAction);
    const auto sendDrop = [](rfm::app::FileBrowserPane& destination, const QMimeData& mime,
                             Qt::KeyboardModifiers modifiers, Qt::DropAction expectedAction,
                             rfm::core::InternalTransferAction expectedIntent, bool accepted) {
        const QPoint background(10, destination.fileTable()->viewport()->height() - 2);
        QSignalSpy drops(&destination, &rfm::app::FileBrowserPane::internalDropRequested);
        QDragEnterEvent enter(background, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                              modifiers);
        QApplication::sendEvent(destination.fileTable()->viewport(), &enter);
        QCOMPARE(enter.isAccepted(), accepted);
        QCOMPARE(enter.dropAction(), expectedAction);
        QCOMPARE(destination.fileTable()->property("dropState").toString(),
                 accepted ? QStringLiteral("valid") : QStringLiteral("invalid"));
        QDropEvent drop(QPointF(background), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                        modifiers);
        QApplication::sendEvent(destination.fileTable()->viewport(), &drop);
        QCOMPARE(drop.isAccepted(), accepted);
        if (accepted) {
            QCOMPARE(drop.dropAction(), expectedAction);
        }
        QCOMPARE(drops.size(), accepted ? 1 : 0);
        if (accepted) {
            QCOMPARE(drops.constFirst().at(1).value<rfm::core::InternalTransferAction>(),
                     expectedIntent);
        }
    };

    sendDrop(ssh, localMime, Qt::NoModifier, Qt::CopyAction,
             rfm::core::InternalTransferAction::Copy, true);
    sendDrop(ssh, localMime, Qt::ControlModifier, Qt::CopyAction,
             rfm::core::InternalTransferAction::Copy, true);
    sendDrop(ssh, localMime, Qt::ShiftModifier, Qt::IgnoreAction,
             rfm::core::InternalTransferAction::Move, false);
    sendDrop(ssh, localMime, Qt::ControlModifier | Qt::ShiftModifier, Qt::CopyAction,
             rfm::core::InternalTransferAction::Copy, true);

    ssh.showDirectory(QStringLiteral("/source"), QStringLiteral("/source"),
                      {{QStringLiteral("remote.txt"), 1, {}, false, false},
                       {QStringLiteral("remote-folder"), 0, {}, true, false}});
    ssh.fileTable()->selectAll();
    const QByteArray sshData = ssh.createInternalDragData();
    const auto sshPayload = rfm::core::decodeInternalTransfer(sshData);
    QVERIFY(sshPayload.has_value());
    QCOMPARE(sshPayload->sources.size(), 2);
    local.showDirectory({rfm::core::FileSource::Local,
                         QString::fromLatin1(rfm::core::LocalMachineId),
                         root.filePath(QStringLiteral("target"))},
                        QStringLiteral("target"), {});
    local.setTransferContext(QStringLiteral("instance"), connection, 1);
    local.resize(640, 320);
    local.show();
    QApplication::processEvents();
    QMimeData sshMime;
    sshMime.setData(rfm::core::InternalTransferMimeType, sshData);
    sendDrop(local, sshMime, Qt::NoModifier, Qt::CopyAction,
             rfm::core::InternalTransferAction::Copy, true);
    sendDrop(local, sshMime, Qt::ControlModifier, Qt::CopyAction,
             rfm::core::InternalTransferAction::Copy, true);
    sendDrop(local, sshMime, Qt::ShiftModifier, Qt::IgnoreAction,
             rfm::core::InternalTransferAction::Move, false);
    sendDrop(local, sshMime, Qt::ControlModifier | Qt::ShiftModifier, Qt::CopyAction,
             rfm::core::InternalTransferAction::Copy, true);

    local.setTransferContext(QStringLiteral("instance"), {}, 1);
    QDragEnterEvent staleConnection(QPoint(10, local.fileTable()->viewport()->height() - 2),
                                    Qt::CopyAction | Qt::MoveAction, &sshMime, Qt::LeftButton,
                                    Qt::NoModifier);
    QApplication::sendEvent(local.fileTable()->viewport(), &staleConnection);
    QVERIFY(staleConnection.isAccepted());
    QCOMPARE(staleConnection.dropAction(), Qt::IgnoreAction);
    QCOMPARE(local.fileTable()->property("dropState").toString(), QStringLiteral("invalid"));
}

void FileBrowserPaneTest::cutAppearanceSurvivesRefreshAndClearsCleanly()
{
    rfm::app::FileBrowserPane pane;
    const QList<rfm::core::RemoteEntry> entries{{QStringLiteral("file.txt"), 1, {}, false, false}};
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("/srv"), entries);
    pane.setCutPaths({QStringLiteral("/srv/file.txt")});
    QVERIFY(pane.fileTable()->item(0, 0)->font().italic());
    QVERIFY(pane.fileTable()->item(0, 0)->foreground().style() != Qt::NoBrush);

    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("/srv"), entries);
    QVERIFY(pane.fileTable()->item(0, 0)->font().italic());

    pane.setCutPaths({});
    QVERIFY(!pane.fileTable()->item(0, 0)->font().italic());
    QCOMPARE(pane.fileTable()->item(0, 0)->foreground().style(), Qt::NoBrush);
}

void FileBrowserPaneTest::focusesLocationAndSwitchesVisiblePane()
{
    rfm::app::PaneWorkspace workspace;
    workspace.setSplit(true);
    rfm::app::FileBrowserPane* const primary = workspace.primaryPane();
    rfm::app::FileBrowserPane* const secondary = workspace.otherVisiblePane();
    primary->showDirectory(QStringLiteral("/one"), QStringLiteral("sftp://host/one"), {});
    primary->focusLocation();
    QCOMPARE(primary->pathEdit()->selectedText(), QStringLiteral("sftp://host/one"));

    QCOMPARE(workspace.activePane(), primary);
    workspace.activateOtherPane();
    QCOMPARE(workspace.activePane(), secondary);
    workspace.activateOtherPane();
    QCOMPARE(workspace.activePane(), primary);

    workspace.setSplit(false);
    workspace.activateOtherPane();
    QCOMPARE(workspace.activePane(), primary);
}

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("RemoteFileManagerTests"));
    QCoreApplication::setApplicationName(QStringLiteral("rfm_file_browser_pane_tests"));
    QTemporaryDir settingsDirectory;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDirectory.path());
    FileBrowserPaneTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_file_browser_pane.moc"
