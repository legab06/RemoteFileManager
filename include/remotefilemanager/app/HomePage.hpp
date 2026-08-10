#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <QList>
#include <QWidget>

class QListWidget;
class QPushButton;

namespace rfm::app
{

class HomePage final : public QWidget
{
    Q_OBJECT

  public:
    explicit HomePage(QWidget* parent = nullptr);

    void setProfiles(const QList<rfm::core::ConnectionProfile>& profiles);

  signals:
    void connectProfileRequested(QString id);
    void newConnectionRequested();

  private:
    void updateConnectButton();
    void requestSelectedProfile();

    QListWidget* m_serverList{nullptr};
    QPushButton* m_connectButton{nullptr};
};

} // namespace rfm::app
