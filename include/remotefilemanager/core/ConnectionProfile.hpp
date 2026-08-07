#pragma once

#include <QString>
#include <QMetaType>
#include <QtGlobal>

namespace rfm::core {

struct ConnectionProfile {
    QString displayName;
    QString host;
    QString username;
    quint16 port{22};

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QString effectiveDisplayName() const;
};

}  // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::ConnectionProfile)
