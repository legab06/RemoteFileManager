#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <QMetaType>
#include <QString>

namespace rfm::ssh
{

enum class AuthenticationResult {
    Success,
    Denied,
    Partial,
    Error,
};

enum class AuthenticationNextStep {
    OpenSession,
    RequestPassword,
    Reject,
    Fail,
};

enum class PasswordAuthenticationReason {
    ExplicitKeyFailed,
    KeyOrAgentFailed,
    AdditionalPasswordRequired,
    PasswordOnly,
};

class SshAuthenticationPolicy final
{
  public:
    [[nodiscard]] static AuthenticationNextStep
    afterPasswordless(AuthenticationResult result, bool passwordAuthenticationAllowed,
                      bool serverOffersPassword);
    [[nodiscard]] static QString resolvePrivateKeyPath(const QString& path);
    [[nodiscard]] static PasswordAuthenticationReason
    passwordPromptReason(rfm::core::AuthenticationMode mode, AuthenticationResult result,
                         bool hasExplicitIdentity);
};

} // namespace rfm::ssh

Q_DECLARE_METATYPE(rfm::ssh::AuthenticationResult)
Q_DECLARE_METATYPE(rfm::ssh::AuthenticationNextStep)
Q_DECLARE_METATYPE(rfm::ssh::PasswordAuthenticationReason)
