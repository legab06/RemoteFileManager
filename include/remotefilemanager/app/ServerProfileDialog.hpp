#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <QDialog>

class QDialogButtonBox;

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

  private slots:
    void updateState();

  private:
    ServerProfileForm* m_profileForm{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
    QString m_profileId;
};

} // namespace rfm::app
