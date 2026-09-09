#include "remotefilemanager/app/WorkspaceTabs.hpp"

#include <QTabWidget>
#include <QToolButton>
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
    auto* const addButton = new QToolButton(m_tabs);
    addButton->setObjectName(QStringLiteral("addWorkspaceButton"));
    addButton->setText(QStringLiteral("+"));
    addButton->setToolTip(tr("New tab"));
    addButton->setAccessibleName(tr("New tab"));
    m_tabs->setCornerWidget(addButton, Qt::TopRightCorner);
    connect(addButton, &QToolButton::clicked, this, [this] { createWorkspace(); });
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        closeWorkspace(qobject_cast<PaneWorkspace*>(m_tabs->widget(index)));
    });
    createWorkspace();
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        emit activeWorkspaceChanged();
        emit activePaneChanged(paneId(activePane()));
    });
}

PaneWorkspace* WorkspaceTabs::createWorkspace()
{
    auto* const workspace = new PaneWorkspace(m_tabs);
    connect(workspace, &PaneWorkspace::activePaneChanged, this, [this, workspace](quint64 id) {
        if (workspace == activeWorkspace()) {
            emit activePaneChanged(id);
        }
    });
    connect(workspace, &PaneWorkspace::paneAdded, this, &WorkspaceTabs::paneAdded);
    connect(workspace, &PaneWorkspace::paneVisibilityChanged, this,
            &WorkspaceTabs::paneVisibilityChanged);
    m_tabs->addTab(workspace, tr("Tab %1").arg(m_nextTabNumber++));
    emit paneAdded(workspace->paneId(workspace->primaryPane()));
    m_tabs->setTabsClosable(m_tabs->count() > 1);
    setActiveWorkspace(workspace);
    return workspace;
}

void WorkspaceTabs::setActiveWorkspace(PaneWorkspace* workspace)
{
    if (workspace != nullptr && m_tabs->indexOf(workspace) >= 0) {
        m_tabs->setCurrentWidget(workspace);
    }
}

void WorkspaceTabs::closeWorkspace(PaneWorkspace* workspace)
{
    const int index = m_tabs->indexOf(workspace);
    if (index < 0 || m_tabs->count() <= 1) {
        return;
    }
    const auto ids = workspace->paneIds();
    m_tabs->removeTab(index);
    for (const PaneId id : ids) {
        emit paneRemoved(id);
    }
    delete workspace;
    m_tabs->setTabsClosable(m_tabs->count() > 1);
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

PaneWorkspace* WorkspaceTabs::workspaceForPane(PaneId id) const
{
    for (int index = 0; index < m_tabs->count(); ++index) {
        auto* const workspace = qobject_cast<PaneWorkspace*>(m_tabs->widget(index));
        if (workspace != nullptr && workspace->pane(id) != nullptr) {
            return workspace;
        }
    }
    return nullptr;
}

FileBrowserPane* WorkspaceTabs::pane(PaneId id) const
{
    const auto* const workspace = workspaceForPane(id);
    return workspace == nullptr ? nullptr : workspace->pane(id);
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

QList<WorkspaceTabs::PaneId> WorkspaceTabs::openPaneIds() const
{
    QList<PaneId> ids;
    for (int index = 0; index < m_tabs->count(); ++index) {
        const auto* const workspace = qobject_cast<PaneWorkspace*>(m_tabs->widget(index));
        if (workspace != nullptr) {
            ids.append(workspace->visiblePaneIds());
        }
    }
    return ids;
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
