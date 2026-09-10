#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/ServerCapabilities.hpp"

#include <QDialog>

class QDialogButtonBox;
class QLabel;
class QTableWidget;

namespace rfm::app
{

class ServerProfileForm;

class ServerProfileDialog final : public QDialog
{
    Q_OBJECT

  public:
    explicit ServerProfileDialog(QWidget* parent = nullptr);

    [[nodiscard]] rfm::core::ConnectionProfile profile() const;
    void setProfile(const rfm::core::ConnectionProfile& profile);
    void setServerCapabilities(const rfm::core::ServerCapabilities& capabilities,
                               bool currentlyConnected = false);
    void clearServerCapabilities();

  private slots:
    void updateState();

  private:
    ServerProfileForm* m_profileForm{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
    QLabel* m_sftpProtocolVersion{nullptr};
    QLabel* m_lastDetected{nullptr};
    QLabel* m_capabilitiesContext{nullptr};
    QLabel* m_copyDataStatus{nullptr};
    QLabel* m_copyDataDescription{nullptr};
    QTableWidget* m_sftpExtensions{nullptr};
    QString m_profileId;
};

} // namespace rfm::app
