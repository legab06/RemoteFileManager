#pragma once

#include <QMainWindow>

class QAction;
class QLineEdit;

namespace rfm::app {

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void createActions();
    void createMenus();
    void createNavigationBar();
    void createPlacesDock();
    void createEmptyState();
    void showConnectionPlaceholder();
    void showAboutDialog();

    QAction* m_newConnectionAction{nullptr};
    QAction* m_quitAction{nullptr};
    QAction* m_aboutAction{nullptr};
    QAction* m_backAction{nullptr};
    QAction* m_forwardAction{nullptr};
    QAction* m_upAction{nullptr};
    QAction* m_refreshAction{nullptr};
    QLineEdit* m_remotePathEdit{nullptr};
};

}  // namespace rfm::app

