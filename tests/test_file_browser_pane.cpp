#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/app/PaneWorkspace.hpp"
#include "remotefilemanager/core/InternalTransfer.hpp"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QLocale>
#include <QMimeData>
#include <QRubberBand>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>

class FileBrowserPaneTest final : public QObject
{
    Q_OBJECT

  private slots:
    void displaysDirectoryAndBuildsRemoteSelection();
    void rubberBandSelectsMultipleLocalRows();
    void controlRubberBandTogglesRemoteRows();
    void dragFromSelectedRowPreservesSelectionAndStartsInternalDrag();
    void dragWithActionModifierPreservesMultipleSelection_data();
    void dragWithActionModifierPreservesMultipleSelection();
    void emitsNavigationIntentions();
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
    void resolvesDropOnCurrentDirectoryAndSubfolder();
    void constructsLocalPayloadAndResolvesLocalDropDestinations();
    void acceptsCrossSourceCopyIntentionsAndPreservesPayload();
    void cutAppearanceSurvivesRefreshAndClearsCleanly();
    void focusesLocationAndSwitchesVisiblePane();
    void navigatesLocalDirectoriesWithSourceAwareHistory();
};

void FileBrowserPaneTest::navigatesLocalDirectoriesWithSourceAwareHistory()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("child")));
    const rfm::core::BrowserLocation rootLocation{
        rfm::core::FileSource::Local, QString::fromLatin1(rfm::core::LocalMachineId),
        temporary.path()};
    const rfm::core::BrowserLocation childLocation{
        rfm::core::FileSource::Local, QString::fromLatin1(rfm::core::LocalMachineId),
        root.filePath(QStringLiteral("child"))};

    rfm::app::FileBrowserPane pane;
    QSignalSpy navigation(&pane,
                          &rfm::app::FileBrowserPane::locationNavigationRequested);
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
    QCOMPARE(qvariant_cast<rfm::core::BrowserLocation>(navigation.constLast().at(0)),
             rootLocation);
    pane.showDirectory(rootLocation, QStringLiteral("file:///fixture"), {},
                       rfm::app::PaneNavigation::Back);
    QVERIFY(pane.canGoForward());
    pane.requestForward();
    QCOMPARE(qvariant_cast<rfm::core::BrowserLocation>(navigation.constLast().at(0)),
             childLocation);
    pane.showDirectory(childLocation, QStringLiteral("file:///fixture/child"), {},
                       rfm::app::PaneNavigation::Forward);
    pane.requestParentDirectory();
    QCOMPARE(qvariant_cast<rfm::core::BrowserLocation>(navigation.constLast().at(0)),
             rootLocation);
    pane.showDirectory(rootLocation, QStringLiteral("file:///fixture"), {},
                       rfm::app::PaneNavigation::Normal);
    pane.requestRefresh();
    QCOMPARE(navigation.constLast().at(1).value<rfm::app::PaneNavigation>(),
             rfm::app::PaneNavigation::Refresh);
}

void FileBrowserPaneTest::displaysDirectoryAndBuildsRemoteSelection()
{
    rfm::app::FileBrowserPane pane;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("file.txt"), 1536, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false},
    };
    pane.showDirectory(QStringLiteral("/srv"),
                       QStringLiteral("sftp://user@example.test:22//srv"), entries);

    QCOMPARE(pane.currentPath(), QStringLiteral("/srv"));
    QCOMPARE(pane.pathEdit()->text(), QStringLiteral("sftp://user@example.test:22//srv"));
    QCOMPARE(pane.fileTable()->rowCount(), 2);
    QCOMPARE(pane.fileTable()->item(0, 0)->text(), QStringLiteral("file.txt"));
    QCOMPARE(pane.fileTable()->item(0, 1)->text(), QLocale{}.formattedDataSize(1536));
    QCOMPARE(pane.fileTable()->selectionMode(), QAbstractItemView::ExtendedSelection);
    QCOMPARE(pane.fileTable()->selectionBehavior(), QAbstractItemView::SelectRows);

    pane.fileTable()->selectionModel()->select(
        pane.fileTable()->model()->index(0, 0),
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
    pane.fileTable()->selectionModel()->select(
        pane.fileTable()->model()->index(1, 0),
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
    const auto selection = pane.selectedEntries();
    QCOMPARE(selection.size(), 2);
    QCOMPARE(selection.at(0).path, QStringLiteral("/srv/file.txt"));
    QVERIFY(!selection.at(0).directory);
    QCOMPARE(selection.at(1).path, QStringLiteral("/srv/folder"));
    QVERIFY(selection.at(1).directory);
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
    pane.showDirectory(
        QStringLiteral("/srv/current"), QStringLiteral("sftp://host/srv/current"),
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
    pane.showDirectory(
        QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"),
        {{QStringLiteral("directory-link"), 0, {}, false, true},
         {QStringLiteral("broken-link"), 0, {}, false, true},
         {QStringLiteral("regular-file"), 1, {}, false, false}});
    QSignalSpy navigation(&pane, &rfm::app::FileBrowserPane::navigationRequested);

    QVERIFY(QMetaObject::invokeMethod(pane.fileTable(), "cellDoubleClicked", Qt::DirectConnection,
                                      Q_ARG(int, 0), Q_ARG(int, 0)));
    QCOMPARE(navigation.takeFirst().constFirst().toString(),
             QStringLiteral("/srv/directory-link"));
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
    pane.showDirectory(
        QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"),
        {{QStringLiteral("first.txt"), 1, {}, false, false},
         {QStringLiteral("second.txt"), 1, {}, false, false}});
    pane.fileTable()->selectRow(0);
    QSignalSpy contextMenus(&pane, &rfm::app::FileBrowserPane::contextMenuRequested);
    const QPoint secondRow = pane.fileTable()->visualItemRect(pane.fileTable()->item(1, 0)).center();

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
    QSignalSpy navigationRequests(&pane,
                                  &rfm::app::FileBrowserPane::locationNavigationRequested);

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

    for (const int row : {4, 5}) {
        clicked = clickRow(row);
        QVERIFY(clicked.has_value());
        QCOMPARE(displayedType(*clicked), QStringLiteral("File"));
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
    QVERIFY(properties.text.contains(QStringLiteral("Type: Folder")));
    QVERIFY(!properties.text.contains(QStringLiteral("Size:")));

    pane.showDirectory({rfm::core::FileSource::Ssh, QStringLiteral("remote-id"),
                        QStringLiteral("/srv")},
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
    QCOMPARE(std::ranges::count_if(unsafeLines, [](const QString& line) {
                 return line.startsWith(QStringLiteral("Type: "));
             }),
             1);
    QCOMPARE(std::ranges::count_if(unsafeLines, [](const QString& line) {
                 return line.startsWith(QStringLiteral("Path: "));
             }),
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
                           static_cast<quint64>(index), {}, false, false});
    }
    pane.showDirectory(QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"), entries);
    pane.fileTable()->selectionModel()->select(
        pane.fileTable()->model()->index(40, 0),
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
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
    pane.showDirectory(
        QStringLiteral("/srv"), QStringLiteral("sftp://host/srv"),
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
    primary->showDirectory(
        QStringLiteral("/one/child"), QStringLiteral("/one/child"),
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
    primary->showDirectory(
        QStringLiteral("/one"), QStringLiteral("sftp://host/one"),
        {{QStringLiteral("first.txt"), 1, {}, false, false}});
    secondary->showDirectory(
        QStringLiteral("/two"), QStringLiteral("sftp://host/two"),
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
    const rfm::core::RemoteConnectionIdentity connection{
        QStringLiteral("server.example.test"), 22, 4};
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
        QCOMPARE(arguments.at(3).toBool(), modifiers.testFlag(Qt::ControlModifier) ||
                                             modifiers.testFlag(Qt::ShiftModifier));
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
    const auto sendDrop = [](rfm::app::FileBrowserPane& destination, const QMimeData& mime,
                             Qt::KeyboardModifiers modifiers, Qt::DropAction expectedAction,
                             rfm::core::InternalTransferAction expectedIntent, bool accepted) {
        const QPoint background(10, destination.fileTable()->viewport()->height() - 2);
        QSignalSpy drops(&destination, &rfm::app::FileBrowserPane::internalDropRequested);
        QDragEnterEvent enter(background, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                              modifiers);
        QApplication::sendEvent(destination.fileTable()->viewport(), &enter);
        QVERIFY(enter.isAccepted());
        QCOMPARE(enter.dropAction(), expectedAction);
        QCOMPARE(destination.fileTable()->property("dropState").toString(),
                 accepted ? QStringLiteral("valid") : QStringLiteral("invalid"));
        QDropEvent drop(QPointF(background), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton,
                        modifiers);
        QApplication::sendEvent(destination.fileTable()->viewport(), &drop);
        QCOMPARE(drop.isAccepted(), accepted);
        QCOMPARE(drop.dropAction(), expectedAction);
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

QTEST_MAIN(FileBrowserPaneTest)

#include "test_file_browser_pane.moc"
