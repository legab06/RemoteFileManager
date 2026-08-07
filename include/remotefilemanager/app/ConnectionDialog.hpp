#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QLineEdit;
class QSpinBox;

namespace rfm::app {

class ConnectionDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ConnectionDialog(QWidget* parent = nullptr);

    [[nodiscard]] rfm::core::ConnectionProfile profile() const;
    [[nodiscard]] QString password() const;

private slots:
    void updateState();

private:
    QLineEdit* m_hostEdit{nullptr};
    QLineEdit* m_userEdit{nullptr};
    QSpinBox* m_portSpin{nullptr};
    QCheckBox* m_passwordCheck{nullptr};
    QLineEdit* m_passwordEdit{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
};

}  // namespace rfm::app
