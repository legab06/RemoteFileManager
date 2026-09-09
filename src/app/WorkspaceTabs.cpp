#include "remotefilemanager/app/WorkspaceTabs.hpp"

#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace rfm::app
{
namespace
{

class WorkspaceTabBar final : public QTabBar
{
  public:
    explicit WorkspaceTabBar(QWidget* parent = nullptr) : QTabBar(parent) { setExpanding(false); }

    void setAddButton(QToolButton* button) { m_addButton = button; }
    [[nodiscard]] QToolButton* addButton() const { return m_addButton; }

    [[nodiscard]] QSize sizeHint() const override { return QTabBar::sizeHint(); }

    [[nodiscard]] QSize minimumSizeHint() const override { return QTabBar::minimumSizeHint(); }

  protected:
    void tabInserted(int index) override
    {
        QTabBar::tabInserted(index);
        updateGeometry();
        updateAddButtonGeometry();
    }

    void tabRemoved(int index) override
    {
        QTabBar::tabRemoved(index);
        updateGeometry();
        updateAddButtonGeometry();
    }

    void tabLayoutChange() override { updateAddButtonGeometry(); }

    void resizeEvent(QResizeEvent* event) override
    {
        QTabBar::resizeEvent(event);
        updateAddButtonGeometry();
    }

    void showEvent(QShowEvent* event) override
    {
        QTabBar::showEvent(event);
        updateAddButtonGeometry();
    }

    void changeEvent(QEvent* event) override
    {
        QTabBar::changeEvent(event);
        if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange ||
            event->type() == QEvent::LayoutRequest) {
            updateGeometry();
            updateAddButtonGeometry();
        }
    }

  private:
    void updateAddButtonGeometry()
    {
        if (m_addButton == nullptr) {
            return;
        }
        if (count() < 2) {
            m_addButton->setVisible(false);
            const QMargins margins = contentsMargins();
            if (margins.right() != 0) {
                setContentsMargins(margins.left(), margins.top(), 0, margins.bottom());
            }
            return;
        }
        const QRect lastTab = tabRect(count() - 1);
        const QSize buttonSize = m_addButton->sizeHint();
        const int gap = style()->pixelMetric(QStyle::PM_TabBarTabHSpace, nullptr, this) / 4;
        const int x = lastTab.right() + gap;
        const int y = lastTab.center().y() - buttonSize.height() / 2;
        const QPoint topLeft = mapTo(parentWidget(), QPoint(x, y));
        m_addButton->setGeometry(topLeft.x(), topLeft.y(), buttonSize.width(), buttonSize.height());
        m_addButton->setVisible(true);
        m_addButton->raise();
    }

    QToolButton* m_addButton{nullptr};
};

class WorkspaceTabWidget final : public QTabWidget
{
  public:
    explicit WorkspaceTabWidget(QWidget* parent = nullptr) : QTabWidget(parent)
    {
        auto* const bar = new WorkspaceTabBar(this);
        setTabBar(bar);
        m_addButton = new QToolButton(this);
        m_addButton->setObjectName(QStringLiteral("addWorkspaceButton"));
        m_addButton->setText(QStringLiteral("+"));
        m_addButton->setToolTip(tr("New tab"));
        m_addButton->setAccessibleName(tr("New tab"));
        m_addButton->setAutoRaise(true);
        m_addButton->setCursor(Qt::ArrowCursor);
        bar->setAddButton(m_addButton);
    }

    [[nodiscard]] QToolButton* addWorkspaceButton() const
    {
        return static_cast<WorkspaceTabBar*>(tabBar())->addButton();
    }

  private:
    QToolButton* m_addButton{nullptr};
};

} // namespace

WorkspaceTabs::WorkspaceTabs(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("workspaceTabs"));
    auto* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_tabs = new WorkspaceTabWidget(this);
    layout->addWidget(m_tabs);
    auto* const addButton = static_cast<WorkspaceTabWidget*>(m_tabs)->addWorkspaceButton();
    connect(addButton, &QToolButton::clicked, this, [this] { createWorkspace(); });
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
        closeWorkspace(qobject_cast<PaneWorkspace*>(m_tabs->widget(index)));
    });
    createWorkspace();
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        emit activeWorkspaceChanged();
        emit activePaneChanged(paneId(activePane()));
    });
    updateTabBarVisibility();
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
    m_tabs->addTab(workspace, {});
    updateTabTitles();
    emit paneAdded(workspace->paneId(workspace->primaryPane()));
    m_tabs->setTabsClosable(m_tabs->count() > 1);
    updateTabBarVisibility();
    m_tabs->updateGeometry();
    if (m_tabs->layout() != nullptr) {
        m_tabs->layout()->activate();
    }
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
    updateTabTitles();
    updateTabBarVisibility();
    m_tabs->updateGeometry();
    if (m_tabs->layout() != nullptr) {
        m_tabs->layout()->activate();
    }
}

QToolButton* WorkspaceTabs::addWorkspaceButton() const
{
    return static_cast<WorkspaceTabWidget*>(m_tabs)->addWorkspaceButton();
}

void WorkspaceTabs::updateTabBarVisibility()
{
    auto* const bar = m_tabs->findChild<QTabBar*>();
    bar->setVisible(m_tabs->count() > 1);
    auto* const addButton = static_cast<WorkspaceTabBar*>(bar)->addButton();
    addButton->setVisible(m_tabs->count() > 1);
    if (m_tabs->count() > 1) {
        addButton->raise();
    }
}

void WorkspaceTabs::updateTabTitles()
{
    for (int index = 0; index < m_tabs->count(); ++index) {
        m_tabs->setTabText(index, tr("Tab %1").arg(index + 1));
    }
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
