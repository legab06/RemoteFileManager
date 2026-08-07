#include "remotefilemanager/core/ConnectionProfile.hpp"

namespace rfm::core {

bool ConnectionProfile::isValid() const
{
    return !host.trimmed().isEmpty() && !username.trimmed().isEmpty() && port != 0;
}

QString ConnectionProfile::effectiveDisplayName() const
{
    if (!displayName.trimmed().isEmpty()) {
        return displayName.trimmed();
    }

    return QStringLiteral("%1@%2").arg(username.trimmed(), host.trimmed());
}

}  // namespace rfm::core

