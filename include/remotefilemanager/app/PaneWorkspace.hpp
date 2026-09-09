#pragma once

#include <QHash>
#include <QWidget>

class QSplitter;

namespace rfm::app
{

class FileBrowserPane;

class PaneWorkspace final : public QWidget
{
    Q_OBJECT

  public:
    using PaneId = quint64;

    explicit PaneWorkspace(QWidget* parent = nullptr);

    [[nodiscard]] FileBrowserPane* primaryPane() const;
    [[nodiscard]] FileBrowserPane* activePane() const;
    [[nodiscard]] FileBrowserPane* otherVisiblePane() const;
    [[nodiscard]] FileBrowserPane* otherVisiblePane(PaneId sourcePaneId) const;
    [[nodiscard]] FileBrowserPane* pane(PaneId id) const;
    [[nodiscard]] PaneId paneId(const FileBrowserPane* pane) const;
    [[nodiscard]] QList<PaneId> visiblePaneIds() const;
    [[nodiscard]] QList<PaneId> paneIds() const;
    [[nodiscard]] bool isSplit() const;

  public slots:
    void clear();
    void setSplit(bool enabled);
    void activateOtherPane();
    void resetFileView();

  signals:
    void activePaneChanged(quint64 paneId);
    void paneVisibilityChanged(quint64 paneId, bool visible);

  private:
    FileBrowserPane* createPane(PaneId id);
    void setActivePane(FileBrowserPane* pane);
    void updateActiveAppearance();

    QSplitter* m_splitter{nullptr};
    FileBrowserPane* m_primaryPane{nullptr};
    FileBrowserPane* m_secondaryPane{nullptr};
    FileBrowserPane* m_activePane{nullptr};
    QHash<PaneId, FileBrowserPane*> m_panes;
    bool m_split{false};
};

} // namespace rfm::app
