#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>

namespace rfm::core
{

enum class AuthenticationMode {
    KeyOrAgent,
    PasswordOnly,
};

struct ConnectionProfile {
    ConnectionProfile() = default;
    ConnectionProfile(QString displayName, QString host, QString username, quint16 port,
                      QString id = {}, bool allowPasswordAuthentication = false,
                      QString privateKeyPath = {},
                      AuthenticationMode authenticationMode = AuthenticationMode::KeyOrAgent);

    QString displayName;
    QString host;
    QString username;
    quint16 port{22};
    QString id;
    bool allowPasswordAuthentication{false};
    QString privateKeyPath;
    AuthenticationMode authenticationMode{AuthenticationMode::KeyOrAgent};

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isValidSavedProfile() const;
    [[nodiscard]] QString effectiveDisplayName() const;
};

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::ConnectionProfile)
Q_DECLARE_METATYPE(rfm::core::AuthenticationMode)
