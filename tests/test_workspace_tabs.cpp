#include "remotefilemanager/app/FileBrowserPane.hpp"
#include "remotefilemanager/app/WorkspaceTabs.hpp"

#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTest>
#include <QToolButton>

class WorkspaceTabsTest final : public QObject
{
    Q_OBJECT

  private slots:
    void createsSwitchesAndClosesWorkspaces()
    {
        rfm::app::WorkspaceTabs tabs;
        auto* const widget = tabs.findChild<QTabWidget*>();
        auto* const first = tabs.activeWorkspace();
        first->setSplit(true);
        first->activateOtherPane();
        auto* const firstActive = first->activePane();
        QSignalSpy added(&tabs, &rfm::app::WorkspaceTabs::paneAdded);
        QSignalSpy removed(&tabs, &rfm::app::WorkspaceTabs::paneRemoved);
        QSignalSpy changed(&tabs, &rfm::app::WorkspaceTabs::activeWorkspaceChanged);
        tabs.findChild<QToolButton*>(QStringLiteral("addWorkspaceButton"))->click();
        auto* const second = tabs.activeWorkspace();
        QVERIFY(second != first);
        QCOMPARE(widget->count(), 2);
        QVERIFY(widget->tabsClosable());
        QCOMPARE(added.size(), 1);
        QCOMPARE(changed.size(), 1);
        QVERIFY(!second->isSplit());
        QVERIFY(!second->activePane()->hasLocation());
        QVERIFY(!second->activePane()->canGoBack());
        QVERIFY(!second->activePane()->canGoForward());
        const auto secondId = tabs.paneId(second->activePane());
        QVERIFY(!first->paneIds().contains(secondId));
        QCOMPARE(tabs.openPaneIds().size(), 3);
        QCOMPARE(tabs.visiblePaneIds().size(), 1);
        tabs.setActiveWorkspace(first);
        QCOMPARE(tabs.activePane(), firstActive);
        QVERIFY(first->isSplit());
        QVERIFY(!second->isSplit());
        tabs.setActiveWorkspace(second);
        second->setSplit(true);
        QCOMPARE(added.size(), 2);
        second->setSplit(false);
        second->setSplit(true);
        QCOMPARE(added.size(), 2);
        const auto retiredIds = second->paneIds();
        QPointer<rfm::app::PaneWorkspace> retired = second;
        QPointer<rfm::app::FileBrowserPane> retiredPane = second->activePane();
        QVERIFY(QMetaObject::invokeMethod(widget, "tabCloseRequested", Q_ARG(int, 1)));
        QVERIFY(retired.isNull());
        QVERIFY(retiredPane.isNull());
        QCOMPARE(removed.size(), 2);
        QCOMPARE(tabs.activeWorkspace(), first);
        QVERIFY(!widget->tabsClosable());
        for (const auto id : retiredIds) {
            QCOMPARE(tabs.pane(id), nullptr);
        }
        tabs.closeWorkspace(first);
        QCOMPARE(widget->count(), 1);
        auto* const third = tabs.createWorkspace();
        QVERIFY(!retiredIds.contains(tabs.paneId(third->activePane())));
        QCOMPARE(widget->tabText(1), QStringLiteral("Tab 3"));
        // Closing an inactive workspace does not change the current one.
        tabs.closeWorkspace(first);
        QCOMPARE(tabs.activeWorkspace(), third);
    }

    void startsWithOneWorkspace()
    {
        rfm::app::WorkspaceTabs tabs;
        auto* const widget = tabs.findChild<QTabWidget*>();
        QVERIFY(widget != nullptr);
        QCOMPARE(widget->count(), 1);
        QVERIFY(!widget->tabsClosable());
        QCOMPARE(widget->tabText(0), QStringLiteral("Tab 1"));
        QCOMPARE(tabs.findChildren<rfm::app::PaneWorkspace*>().size(), 1);
        auto* const workspace = tabs.activeWorkspace();
        QVERIFY(workspace != nullptr);
        QCOMPARE(widget->widget(0), workspace);
        QCOMPARE(tabs.activePane(), workspace->activePane());
        const auto id = tabs.paneId(tabs.activePane());
        QVERIFY(id != 0);
        QCOMPARE(tabs.pane(id), tabs.activePane());
        QCOMPARE(tabs.paneIds(), QList<quint64>{id});
        QCOMPARE(tabs.visiblePaneIds(), QList<quint64>{id});
        QCOMPARE(tabs.pane(0), nullptr);
        QCOMPARE(tabs.paneId(nullptr), quint64{0});
        QCOMPARE(tabs.otherVisiblePane(), nullptr);
    }

    void splitRelaysVisibilityAndActivation()
    {
        rfm::app::WorkspaceTabs tabs;
        auto* const workspace = tabs.activeWorkspace();
        auto* const primary = tabs.activePane();
        QSignalSpy visibility(&tabs, &rfm::app::WorkspaceTabs::paneVisibilityChanged);
        QSignalSpy activation(&tabs, &rfm::app::WorkspaceTabs::activePaneChanged);
        workspace->setSplit(true);
        auto* const secondary = tabs.otherVisiblePane();
        QVERIFY(secondary != nullptr);
        const auto secondaryId = tabs.paneId(secondary);
        QCOMPARE(tabs.visiblePaneIds().size(), 2);
        QCOMPARE(visibility.size(), 1);
        QCOMPARE(visibility.at(0).at(0).toULongLong(), secondaryId);
        QCOMPARE(visibility.at(0).at(1).toBool(), true);
        workspace->activateOtherPane();
        QCOMPARE(tabs.activePane(), secondary);
        QCOMPARE(tabs.otherVisiblePane(secondaryId), primary);
        QCOMPARE(activation.size(), 1);
        QCOMPARE(activation.at(0).at(0).toULongLong(), secondaryId);
        workspace->setSplit(false);
        QCOMPARE(tabs.activePane(), secondary);
        QCOMPARE(tabs.visiblePaneIds(), QList<quint64>{secondaryId});
        QCOMPARE(tabs.paneIds().size(), 2);
        QCOMPARE(tabs.otherVisiblePane(), nullptr);
        QCOMPARE(visibility.size(), 2);
        QCOMPARE(visibility.at(1).at(1).toBool(), false);
        workspace->setSplit(true);
        QCOMPARE(tabs.otherVisiblePane(), primary);
        QCOMPARE(tabs.paneId(secondary), secondaryId);
        QCOMPARE(tabs.paneIds().size(), 2);
    }

    void paneIdsAreUniqueAcrossWorkspacesAndLifetimes()
    {
        QSet<quint64> seen;
        for (int iteration = 0; iteration < 3; ++iteration) {
            rfm::app::PaneWorkspace first;
            rfm::app::PaneWorkspace second;
            first.setSplit(true);
            second.setSplit(true);
            for (auto* const workspace : {&first, &second}) {
                for (const auto id : workspace->paneIds()) {
                    QVERIFY(id != 0);
                    QVERIFY(!seen.contains(id));
                    seen.insert(id);
                }
            }
        }
        QCOMPARE(seen.size(), 12);
    }
};

QTEST_MAIN(WorkspaceTabsTest)
#include "test_workspace_tabs.moc"
