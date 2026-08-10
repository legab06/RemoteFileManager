#pragma once

#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/RemoteFileOperations.hpp"

#include <QPoint>
#include <QStringList>
#include <QWidget>

class QLineEdit;
class QTableWidget;

namespace rfm::app
{

enum class PaneNavigation { Initial, Normal, Back, Forward, Refresh };

class FileBrowserPane final : public QWidget
{
    Q_OBJECT

  public:
    explicit FileBrowserPane(QWidget* parent = nullptr);

    [[nodiscard]] QString currentPath() const;
    [[nodiscard]] QList<rfm::core::RemoteSelection> selectedEntries() const;
    [[nodiscard]] QLineEdit* pathEdit() const;
    [[nodiscard]] QTableWidget* fileTable() const;
    [[nodiscard]] bool canGoBack() const;
    [[nodiscard]] bool canGoForward() const;

    void showDirectory(const QString& path, const QString& displayPath,
                       const QList<rfm::core::RemoteEntry>& entries,
                       PaneNavigation navigation = PaneNavigation::Refresh);
    void setPendingSelectionNames(QStringList names);
    void setInteractionEnabled(bool enabled);
    void setActiveAppearance(bool active);
    void navigateTo(const QString& path);
    void requestParentDirectory();
    void requestBack();
    void requestForward();
    void requestRefresh();

  signals:
    void activated();
    void navigationRequested(QString path, rfm::app::PaneNavigation navigation);
    void historyChanged();
    void selectionChanged();
    void contextMenuRequested(QPoint globalPosition);

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void openEntry(int row);
    void prepareContextMenu(const QPoint& position);

    QLineEdit* m_pathEdit{nullptr};
    QTableWidget* m_fileTable{nullptr};
    QString m_currentPath;
    QStringList m_pendingSelectionNames;
    QStringList m_backHistory;
    QStringList m_forwardHistory;
};

} // namespace rfm::app

Q_DECLARE_METATYPE(rfm::app::PaneNavigation)
