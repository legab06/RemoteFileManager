#pragma once

#include "remotefilemanager/core/VolumeService.hpp"

#include <QByteArray>
#include <QDialog>

class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace rfm::app
{

class VolumeAuthenticationDialog final : public QDialog
{
    Q_OBJECT

  public:
    explicit VolumeAuthenticationDialog(QString server, QString device,
                                        rfm::core::VolumeOperation operation,
                                        QWidget* parent = nullptr);
    ~VolumeAuthenticationDialog() override;

    [[nodiscard]] QByteArray takePassword();

  private:
    void clearPasswordEdit();

    QLineEdit* m_passwordEdit{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
};

} // namespace rfm::app
