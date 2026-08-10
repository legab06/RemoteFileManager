#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <utility>

namespace rfm::core {

ConnectionProfile::ConnectionProfile(QString displayNameValue, QString hostValue,
                                     QString usernameValue, quint16 portValue, QString idValue,
                                     bool allowPasswordFallbackValue)
    : displayName(std::move(displayNameValue)),
      host(std::move(hostValue)),
      username(std::move(usernameValue)),
      port(portValue),
      id(std::move(idValue)),
      allowPasswordFallback(allowPasswordFallbackValue)
{
}

bool ConnectionProfile::isValid() const
{
    return !host.trimmed().isEmpty() && !username.trimmed().isEmpty() && port != 0;
}

bool ConnectionProfile::isValidSavedProfile() const
{
    return isValid() && !id.trimmed().isEmpty();
}

QString ConnectionProfile::effectiveDisplayName() const
{
    if (!displayName.trimmed().isEmpty()) {
        return displayName.trimmed();
    }

    return QStringLiteral("%1@%2").arg(username.trimmed(), host.trimmed());
}

}  // namespace rfm::core
