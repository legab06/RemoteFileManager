#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QLineEdit;
class QSpinBox;

namespace rfm::app
{

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
    QLineEdit* m_nameEdit{nullptr};
    QLineEdit* m_hostEdit{nullptr};
    QLineEdit* m_userEdit{nullptr};
    QSpinBox* m_portSpin{nullptr};
    QCheckBox* m_passwordCheck{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
    QString m_profileId;
};

} // namespace rfm::app
