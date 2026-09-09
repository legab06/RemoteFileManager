#pragma once

#include "remotefilemanager/app/PaneWorkspace.hpp"

#include <QWidget>

class QTabWidget;

namespace rfm::app
{

// Owns workspace presentation only; navigation and operations remain in MainWindow.
class WorkspaceTabs final : public QWidget
{
    Q_OBJECT

  public:
    using PaneId = PaneWorkspace::PaneId;

    explicit WorkspaceTabs(QWidget* parent = nullptr);

    [[nodiscard]] PaneWorkspace* activeWorkspace() const;
    [[nodiscard]] PaneWorkspace* workspaceForPane(PaneId id) const;
    PaneWorkspace* createWorkspace();
    void closeWorkspace(PaneWorkspace* workspace);
    void setActiveWorkspace(PaneWorkspace* workspace);
    [[nodiscard]] FileBrowserPane* activePane() const;
    [[nodiscard]] FileBrowserPane* otherVisiblePane() const;
    [[nodiscard]] FileBrowserPane* otherVisiblePane(PaneId sourcePaneId) const;
    [[nodiscard]] FileBrowserPane* pane(PaneId id) const;
    [[nodiscard]] PaneId paneId(const FileBrowserPane* pane) const;
    // Visible panes belong to the current tab; paneIds includes all workspaces.
    [[nodiscard]] QList<PaneId> visiblePaneIds() const;
    [[nodiscard]] QList<PaneId> paneIds() const;
    // Panes exposed by each workspace's split state, including inactive tabs.
    [[nodiscard]] QList<PaneId> openPaneIds() const;

  signals:
    void activeWorkspaceChanged();
    void paneAdded(quint64 paneId);
    void paneRemoved(quint64 paneId);
    void activePaneChanged(quint64 paneId);
    void paneVisibilityChanged(quint64 paneId, bool visible);

  private:
    QTabWidget* m_tabs{nullptr};
    quint64 m_nextTabNumber{1};
};

} // namespace rfm::app
