#pragma once

#include <QString>
#include <QtTypes>

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

