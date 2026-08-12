#pragma once

#include "remotefilemanager/core/BrowserLocation.hpp"
#include "remotefilemanager/core/InternalTransfer.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/RemoteFileOperations.hpp"

#include <QByteArray>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QWidget>

class QLineEdit;
class QTableWidget;

namespace rfm::app
{

enum class PaneNavigation { Initial, Normal, Back, Forward, Refresh, SafetyFallback };

class FileBrowserPane final : public QWidget
{
    Q_OBJECT

  public:
    explicit FileBrowserPane(QWidget* parent = nullptr);

    [[nodiscard]] QString currentPath() const;
    [[nodiscard]] rfm::core::BrowserLocation currentLocation() const;
    [[nodiscard]] rfm::core::FileSource source() const;
    [[nodiscard]] bool hasLocation() const;
    [[nodiscard]] QList<rfm::core::RemoteSelection> selectedEntries() const;
    [[nodiscard]] QLineEdit* pathEdit() const;
    [[nodiscard]] QTableWidget* fileTable() const;
    [[nodiscard]] bool canGoBack() const;
    [[nodiscard]] bool canGoForward() const;
    [[nodiscard]] QByteArray createInternalDragData() const;

    void showDirectory(const QString& path, const QString& displayPath,
                       const QList<rfm::core::RemoteEntry>& entries,
                       PaneNavigation navigation = PaneNavigation::Refresh);
    void showDirectory(const rfm::core::BrowserLocation& location, const QString& displayPath,
                       const QList<rfm::core::RemoteEntry>& entries,
                       PaneNavigation navigation = PaneNavigation::Refresh);
    void clear();
    void removeHistoryForSource(rfm::core::FileSource source);
    void removeLocalHistoryUnderPath(const QString& rootPath);
    void removeHistoryUnderPath(rfm::core::FileSource source, const QString& machineId,
                                const QString& rootPath);
    void setPendingSelectionNames(QStringList names);
    void setInteractionEnabled(bool enabled);
    void setActiveAppearance(bool active);
    void setTransferContext(QString applicationInstanceId,
                            rfm::core::RemoteConnectionIdentity connection, quint64 paneId);
    void clearTransferContext();
    void setCutPaths(QSet<QString> paths);
    void focusLocation();
    void navigateTo(const QString& path);
    void requestParentDirectory();
    void requestBack();
    void requestForward();
    void requestRefresh();

  signals:
    void activated();
    void navigationRequested(QString path, rfm::app::PaneNavigation navigation);
    void locationNavigationRequested(rfm::core::BrowserLocation location,
                                     rfm::app::PaneNavigation navigation);
    void historyChanged();
    void selectionChanged();
    void contextMenuRequested(QPoint globalPosition);
    void internalDropRequested(rfm::core::InternalTransferPayload payload,
                               QString destinationDirectory);

  private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void openEntry(int row);
    void prepareContextMenu(const QPoint& position);
    void startInternalDrag(Qt::DropActions supportedActions);
    [[nodiscard]] QString dropDestinationAt(const QPoint& position, int* folderRow = nullptr) const;
    [[nodiscard]] rfm::core::InternalTransferValidation
    validateDrop(const rfm::core::InternalTransferPayload& payload,
                 const QString& destination) const;
    void updateDropAppearance(bool active, bool valid, int folderRow = -1);
    void updateCutAppearance();
    [[nodiscard]] QString normalizedPath(const rfm::core::BrowserLocation& location) const;
    void requestLocation(const rfm::core::BrowserLocation& location, PaneNavigation navigation);

    QLineEdit* m_pathEdit{nullptr};
    QTableWidget* m_fileTable{nullptr};
    rfm::core::BrowserLocation m_currentLocation;
    QStringList m_pendingSelectionNames;
    QList<rfm::core::BrowserLocation> m_backHistory;
    QList<rfm::core::BrowserLocation> m_forwardHistory;
    QString m_applicationInstanceId;
    rfm::core::RemoteConnectionIdentity m_connectionIdentity;
    quint64 m_paneId{0};
    QSet<QString> m_cutPaths;
    int m_dropHighlightRow{-1};
    bool m_restoreTableFocus{false};
};

} // namespace rfm::app

Q_DECLARE_METATYPE(rfm::app::PaneNavigation)
