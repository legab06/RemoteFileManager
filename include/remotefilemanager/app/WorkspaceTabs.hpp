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
    [[nodiscard]] FileBrowserPane* activePane() const;
    [[nodiscard]] FileBrowserPane* otherVisiblePane() const;
    [[nodiscard]] FileBrowserPane* otherVisiblePane(PaneId sourcePaneId) const;
    [[nodiscard]] FileBrowserPane* pane(PaneId id) const;
    [[nodiscard]] PaneId paneId(const FileBrowserPane* pane) const;
    // Visible panes belong to the current tab; paneIds includes all workspaces.
    [[nodiscard]] QList<PaneId> visiblePaneIds() const;
    [[nodiscard]] QList<PaneId> paneIds() const;

  signals:
    void activePaneChanged(quint64 paneId);
    void paneVisibilityChanged(quint64 paneId, bool visible);

  private:
    QTabWidget* m_tabs{nullptr};
};

} // namespace rfm::app
