#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <QWidget>

class QCheckBox;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;

namespace rfm::app
{

class ServerProfileForm final : public QWidget
{
    Q_OBJECT

  public:
    explicit ServerProfileForm(QWidget* parent = nullptr);

    [[nodiscard]] rfm::core::ConnectionProfile profile(const QString& id = {}) const;
    void setProfile(const rfm::core::ConnectionProfile& profile);

  signals:
    void validityChanged(bool valid);

  private:
    void publishValidity();
    void browseForPrivateKey();
    void updateAuthenticationControls();

    QLineEdit* m_nameEdit{nullptr};
    QLineEdit* m_hostEdit{nullptr};
    QLineEdit* m_userEdit{nullptr};
    QSpinBox* m_portSpin{nullptr};
    QRadioButton* m_keyModeRadio{nullptr};
    QRadioButton* m_passwordOnlyRadio{nullptr};
    QLineEdit* m_privateKeyEdit{nullptr};
    QPushButton* m_privateKeyBrowseButton{nullptr};
    QCheckBox* m_passwordCheck{nullptr};
};

} // namespace rfm::app
