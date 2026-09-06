#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/SecurePassword.hpp"

#include <QDialog>

class QCloseEvent;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QProgressBar;

namespace rfm::app
{

class PasswordAuthenticationDialog final : public QDialog
{
    Q_OBJECT

  public:
    explicit PasswordAuthenticationDialog(const rfm::core::ConnectionProfile& profile,
                                          QWidget* parent = nullptr);
    ~PasswordAuthenticationDialog() override;

    [[nodiscard]] rfm::core::SecurePassword takePassword();
    [[nodiscard]] bool isAuthenticating() const;
    void setAuthenticationMessage(const QString& message);
    void showAuthenticationError(const QString& message);
    void authenticationSucceeded();
    void connectionFailed();

  signals:
    void authenticationRequested();

  protected:
    void reject() override;
    void closeEvent(QCloseEvent* event) override;

  private:
    void requestAuthentication();
    void setAuthenticating(bool authenticating);
    void clearPasswordEdit();
    void clearPasswordEdit(QString& extractedSecret);

    QLineEdit* m_passwordEdit{nullptr};
    QLabel* m_explanation{nullptr};
    QProgressBar* m_activityIndicator{nullptr};
    QLabel* m_statusLabel{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
    bool m_authenticating{false};
};

} // namespace rfm::app
