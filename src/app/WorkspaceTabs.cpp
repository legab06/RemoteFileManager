#include "remotefilemanager/app/WorkspaceTabs.hpp"

#include <QTabWidget>
#include <QVBoxLayout>

namespace rfm::app
{

WorkspaceTabs::WorkspaceTabs(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("workspaceTabs"));
    auto* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs);
    auto* const workspace = new PaneWorkspace(m_tabs);
    connect(workspace, &PaneWorkspace::activePaneChanged, this, &WorkspaceTabs::activePaneChanged);
    connect(workspace, &PaneWorkspace::paneVisibilityChanged, this,
            &WorkspaceTabs::paneVisibilityChanged);
    m_tabs->addTab(workspace, tr("Tab 1"));
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [this](int) { emit activePaneChanged(paneId(activePane())); });
}

PaneWorkspace* WorkspaceTabs::activeWorkspace() const
{
    return qobject_cast<PaneWorkspace*>(m_tabs->currentWidget());
}

FileBrowserPane* WorkspaceTabs::activePane() const
{
    const auto* const workspace = activeWorkspace();
    return workspace == nullptr ? nullptr : workspace->activePane();
}

FileBrowserPane* WorkspaceTabs::otherVisiblePane() const
{
    return otherVisiblePane(paneId(activePane()));
}

FileBrowserPane* WorkspaceTabs::otherVisiblePane(PaneId sourcePaneId) const
{
    const auto* const workspace = activeWorkspace();
    return workspace == nullptr ? nullptr : workspace->otherVisiblePane(sourcePaneId);
}

FileBrowserPane* WorkspaceTabs::pane(PaneId id) const
{
    for (int index = 0; index < m_tabs->count(); ++index) {
        const auto* const workspace = qobject_cast<PaneWorkspace*>(m_tabs->widget(index));
        if (workspace != nullptr) {
            if (auto* const result = workspace->pane(id); result != nullptr) {
                return result;
            }
        }
    }
    return nullptr;
}

WorkspaceTabs::PaneId WorkspaceTabs::paneId(const FileBrowserPane* pane) const
{
    for (int index = 0; index < m_tabs->count(); ++index) {
        const auto* const workspace = qobject_cast<PaneWorkspace*>(m_tabs->widget(index));
        if (workspace != nullptr) {
            const PaneId id = workspace->paneId(pane);
            if (id != 0) {
                return id;
            }
        }
    }
    return 0;
}

QList<WorkspaceTabs::PaneId> WorkspaceTabs::visiblePaneIds() const
{
    const auto* const workspace = activeWorkspace();
    return workspace == nullptr ? QList<PaneId>{} : workspace->visiblePaneIds();
}

QList<WorkspaceTabs::PaneId> WorkspaceTabs::paneIds() const
{
    QList<PaneId> ids;
    for (int index = 0; index < m_tabs->count(); ++index) {
        const auto* const workspace = qobject_cast<PaneWorkspace*>(m_tabs->widget(index));
        if (workspace != nullptr) {
            ids.append(workspace->paneIds());
        }
    }
    return ids;
}

} // namespace rfm::app
