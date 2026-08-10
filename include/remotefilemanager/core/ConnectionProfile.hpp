#pragma once

#include <QString>
#include <QMetaType>
#include <QtGlobal>

namespace rfm::core {

struct ConnectionProfile {
    ConnectionProfile() = default;
    ConnectionProfile(QString displayName, QString host, QString username, quint16 port,
                      QString id = {}, bool allowPasswordFallback = false);

    QString displayName;
    QString host;
    QString username;
    quint16 port{22};
    QString id;
    bool allowPasswordFallback{false};

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool isValidSavedProfile() const;
    [[nodiscard]] QString effectiveDisplayName() const;
};

}  // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::ConnectionProfile)
