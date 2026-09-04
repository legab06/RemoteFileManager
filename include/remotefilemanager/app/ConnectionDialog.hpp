#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <QDialog>

class QCheckBox;
class QCloseEvent;
class QDialogButtonBox;
class QLabel;
class QProgressBar;

namespace rfm::app
{

class ServerProfileForm;

class ConnectionDialog final : public QDialog
{
    Q_OBJECT

  public:
    enum class State {
        Idle,
        Connecting,
        Error,
    };
    Q_ENUM(State)

    explicit ConnectionDialog(QWidget* parent = nullptr);

    [[nodiscard]] rfm::core::ConnectionProfile profile() const;
    [[nodiscard]] bool saveServerRequested() const;
    [[nodiscard]] State state() const;
    void setProfile(const rfm::core::ConnectionProfile& profile);
    void setConnecting(bool connecting);
    void showConnectionError(const QString& message);
    void clearConnectionError();
    void connectionSucceeded();
    void connectionCancelled();

  signals:
    void connectionRequested(rfm::core::ConnectionProfile profile);

  protected:
    void reject() override;
    void closeEvent(QCloseEvent* event) override;

  private slots:
    void updateState();
    void requestConnection();

  private:
    void applyState();

    ServerProfileForm* m_profileForm{nullptr};
    QCheckBox* m_saveServerCheck{nullptr};
    QProgressBar* m_activityIndicator{nullptr};
    QLabel* m_statusLabel{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
    QString m_profileId;
    State m_state{State::Idle};
};

} // namespace rfm::app
