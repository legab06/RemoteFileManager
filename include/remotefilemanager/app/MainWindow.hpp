#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"

#include <QMainWindow>

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
    void showRemoteDirectory(const QString& path, const QList<rfm::core::RemoteEntry>& entries);
    void showConnectionError(const QString& message);
    void openSelectedEntry(int row, int column);
    void requestParentDirectory();
    void setBusy(bool busy, const QString& message = {});

    QAction* m_newConnectionAction{nullptr};
    QAction* m_quitAction{nullptr};
    QAction* m_aboutAction{nullptr};
    QAction* m_backAction{nullptr};
    QAction* m_forwardAction{nullptr};
    QAction* m_upAction{nullptr};
    QAction* m_refreshAction{nullptr};
    QLineEdit* m_remotePathEdit{nullptr};
    QTableWidget* m_fileTable{nullptr};
    QThread* m_sshThread{nullptr};
    rfm::ssh::SshSession* m_sshSession{nullptr};
    rfm::core::ConnectionProfile m_activeProfile;
    QString m_currentPath;
};

}  // namespace rfm::app
