#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/RemoteFileOperations.hpp"

#include <QMainWindow>
#include <QPoint>
#include <QStringList>

class QAction;
class QLineEdit;
class QTableWidget;
class QThread;

namespace rfm::ssh {
class SshSession;
}

namespace rfm::app {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    void connectionRequested(rfm::core::ConnectionProfile profile, QString password);
    void hostKeyDecision(bool accepted);
    void directoryRequested(QString path);
    void createDirectoryRequested(quint64 id, QString parent, QString name);
    void renameRequested(quint64 id, QString source, QString newName);
    void moveRequested(quint64 id,
                       QList<rfm::core::RemoteSelection> sources,
                       QString destinationDirectory);
    void copyRequested(quint64 id,
                       QList<rfm::core::RemoteSelection> sources,
                       QString destinationDirectory);
    void removeRequested(quint64 id,
                         QList<rfm::core::RemoteSelection> sources,
                         bool recursive);
    void disconnectionRequested();

private:
    void createActions();
    void createMenus();
    void createNavigationBar();
    void createPlacesDock();
    void createEmptyState();
    void showConnectionDialog();
    void showAboutDialog();
    void showHostKeyConfirmation(const QString& host, const QString& fingerprint);
    Q_INVOKABLE void showRemoteDirectory(
        const QString& path, const QList<rfm::core::RemoteEntry>& entries);
    void showConnectionError(const QString& message);
    void openSelectedEntry(int row, int column);
    void requestParentDirectory();
    void showFileContextMenu(const QPoint& position);
    void createRemoteDirectory();
    void renameSelectedEntry();
    void moveSelectedEntries();
    void copySelectedEntries();
    void removeSelectedEntries();
    void handleOperationResult(const rfm::core::RemoteOperationResult& result);
    void updateOperationActions();
    [[nodiscard]] QList<rfm::core::RemoteSelection> selectedEntries() const;
    [[nodiscard]] QString askDestination(const QString& title);
    [[nodiscard]] quint64 nextOperationId();
    void setBusy(bool busy, const QString& message = {});

    QAction* m_newConnectionAction{nullptr};
    QAction* m_quitAction{nullptr};
    QAction* m_aboutAction{nullptr};
    QAction* m_backAction{nullptr};
    QAction* m_forwardAction{nullptr};
    QAction* m_upAction{nullptr};
    QAction* m_refreshAction{nullptr};
    QAction* m_createDirectoryAction{nullptr};
    QAction* m_renameAction{nullptr};
    QAction* m_moveAction{nullptr};
    QAction* m_copyAction{nullptr};
    QAction* m_removeAction{nullptr};
    QLineEdit* m_remotePathEdit{nullptr};
    QTableWidget* m_fileTable{nullptr};
    QThread* m_sshThread{nullptr};
    rfm::ssh::SshSession* m_sshSession{nullptr};
    rfm::core::ConnectionProfile m_activeProfile;
    QString m_currentPath;
    QStringList m_pendingSelectionNames;
    quint64 m_nextOperationId{1};
    bool m_connected{false};
    bool m_busy{false};
};

}  // namespace rfm::app
