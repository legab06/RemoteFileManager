#include "remotefilemanager/app/PaneWorkspace.hpp"

#include "remotefilemanager/app/FileBrowserPane.hpp"

#include <QSplitter>
#include <QStyle>
#include <QTableWidget>
#include <QVBoxLayout>

#include <limits>

namespace rfm::app
{
namespace
{
// Workspaces and their panes are created on the GUI thread. Never recycle IDs,
// including after destruction: asynchronous results may still refer to them.
quint64 allocatePaneId()
{
    static quint64 nextPaneId{1};
    if (nextPaneId == std::numeric_limits<quint64>::max()) {
        qFatal("Pane ID space exhausted");
    }
    return nextPaneId++;
}
} // namespace

PaneWorkspace::PaneWorkspace(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("paneWorkspace"));
    auto* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("paneSplitter"));
    m_splitter->setChildrenCollapsible(false);
    layout->addWidget(m_splitter);

    m_primaryPane = createPane(allocatePaneId());
    m_activePane = m_primaryPane;
    updateActiveAppearance();
}

FileBrowserPane* PaneWorkspace::primaryPane() const { return m_primaryPane; }

FileBrowserPane* PaneWorkspace::activePane() const { return m_activePane; }

FileBrowserPane* PaneWorkspace::otherVisiblePane() const
{
    return otherVisiblePane(paneId(m_activePane));
}

FileBrowserPane* PaneWorkspace::otherVisiblePane(PaneId sourcePaneId) const
{
    if (!isSplit()) {
        return nullptr;
    }
    FileBrowserPane* const sourcePane = pane(sourcePaneId);
    if (sourcePane == nullptr || sourcePane->isHidden()) {
        return nullptr;
    }
    FileBrowserPane* const otherPane =
        sourcePane == m_primaryPane ? m_secondaryPane : m_primaryPane;
    return otherPane != nullptr && !otherPane->isHidden() ? otherPane : nullptr;
}

FileBrowserPane* PaneWorkspace::pane(PaneId id) const { return m_panes.value(id, nullptr); }

PaneWorkspace::PaneId PaneWorkspace::paneId(const FileBrowserPane* pane) const
{
    for (auto iterator = m_panes.cbegin(); iterator != m_panes.cend(); ++iterator) {
        if (iterator.value() == pane) {
            return iterator.key();
        }
    }
    return 0;
}

QList<PaneWorkspace::PaneId> PaneWorkspace::visiblePaneIds() const
{
    QList<PaneId> ids;
    for (auto iterator = m_panes.cbegin(); iterator != m_panes.cend(); ++iterator) {
        if (!iterator.value()->isHidden()) {
            ids.push_back(iterator.key());
        }
    }
    return ids;
}

QList<PaneWorkspace::PaneId> PaneWorkspace::paneIds() const { return m_panes.keys(); }

bool PaneWorkspace::isSplit() const { return m_split; }

void PaneWorkspace::clear()
{
    for (FileBrowserPane* const pane : m_panes) {
        pane->clear();
    }
}

void PaneWorkspace::setSplit(bool enabled)
{
    if (enabled == isSplit()) {
        return;
    }
    if (enabled) {
        if (m_secondaryPane == nullptr) {
            m_secondaryPane = createPane(allocatePaneId());
        }
        FileBrowserPane* const hiddenPane =
            m_activePane == m_primaryPane ? m_secondaryPane : m_primaryPane;
        m_split = true;
        hiddenPane->show();
        emit paneVisibilityChanged(paneId(hiddenPane), true);
        updateActiveAppearance();
        return;
    }

    FileBrowserPane* const paneToHide =
        m_activePane == m_primaryPane ? m_secondaryPane : m_primaryPane;
    const PaneId hiddenId = paneId(paneToHide);
    m_split = false;
    paneToHide->hide();
    emit paneVisibilityChanged(hiddenId, false);
    updateActiveAppearance();
}

void PaneWorkspace::activateOtherPane()
{
    FileBrowserPane* const otherPane = otherVisiblePane();
    if (otherPane == nullptr) {
        return;
    }
    setActivePane(otherPane);
    otherPane->focusFileView();
}

void PaneWorkspace::resetFileView()
{
    if (m_primaryPane != nullptr) {
        m_primaryPane->resetFileView();
    }
}

FileBrowserPane* PaneWorkspace::createPane(PaneId id)
{
    auto* const pane = new FileBrowserPane(m_splitter);
    pane->setProperty("paneId", QVariant::fromValue(id));
    pane->setStyleSheet(QStringLiteral(
        "rfm--app--FileBrowserPane[activePane=\"true\"] {"
        " border: 2px solid palette(highlight);"
        "}"
        "rfm--app--FileBrowserPane[activePane=\"false\"] {"
        " border: 1px solid palette(mid);"
        "}"
        "rfm--app--FileBrowserPane[activePane=\"true\"] QLineEdit#remotePathEdit {"
        " border-bottom: 2px solid palette(highlight);"
        "}"));
    m_splitter->addWidget(pane);
    m_panes.insert(id, pane);
    connect(pane, &FileBrowserPane::activated, this, [this, pane] { setActivePane(pane); });
    emit paneAdded(id);
    return pane;
}

void PaneWorkspace::setActivePane(FileBrowserPane* pane)
{
    if (pane == nullptr || pane == m_activePane || pane->isHidden()) {
        return;
    }
    m_activePane = pane;
    updateActiveAppearance();
    emit activePaneChanged(paneId(pane));
}

void PaneWorkspace::updateActiveAppearance()
{
    for (FileBrowserPane* const pane : m_panes) {
        const bool active = pane == m_activePane;
        pane->setProperty("activePane", active);
        pane->style()->unpolish(pane);
        pane->style()->polish(pane);
        pane->setActiveAppearance(active);
        pane->update();
    }
}

} // namespace rfm::app
