#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <QList>
#include <QWidget>

class QListWidget;
class QHBoxLayout;
class QPushButton;
class QResizeEvent;

namespace rfm::app
{

class HomePage final : public QWidget
{
    Q_OBJECT

  public:
    explicit HomePage(QWidget* parent = nullptr);

    void setProfiles(const QList<rfm::core::ConnectionProfile>& profiles);
    QSize minimumSizeHint() const override;

  signals:
    void connectProfileRequested(QString id);
    void editProfileRequested(QString id);
    void newConnectionRequested();

  private:
    void resizeEvent(QResizeEvent* event) override;
    void updateActionPresentation();
    void updateContentWidth();
    void updateConnectButton();
    void requestSelectedProfile();

    QListWidget* m_serverList{nullptr};
    QHBoxLayout* m_actionsLayout{nullptr};
    QWidget* m_content{nullptr};
    int m_compactContentWidth{0};
    QPushButton* m_connectButton{nullptr};
    QPushButton* m_editButton{nullptr};
    QPushButton* m_newConnectionButton{nullptr};
};

} // namespace rfm::app
