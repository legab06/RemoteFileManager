#include "remotefilemanager/ssh/SshAuthenticationPolicy.hpp"

#include <QDir>
#include <QFileInfo>

namespace rfm::ssh
{

AuthenticationNextStep SshAuthenticationPolicy::afterPasswordless(
    AuthenticationResult result, bool passwordAuthenticationAllowed, bool serverOffersPassword)
{
    if (result == AuthenticationResult::Success) {
        return AuthenticationNextStep::OpenSession;
    }
    if ((result == AuthenticationResult::Denied || result == AuthenticationResult::Partial) &&
        passwordAuthenticationAllowed && serverOffersPassword) {
        return AuthenticationNextStep::RequestPassword;
    }
    if (result == AuthenticationResult::Denied || result == AuthenticationResult::Partial) {
        return AuthenticationNextStep::Reject;
    }
    return AuthenticationNextStep::Fail;
}

QString SshAuthenticationPolicy::resolvePrivateKeyPath(const QString& path)
{
    const QString normalized = QDir::fromNativeSeparators(path.trimmed());
    if (normalized == QStringLiteral("~")) {
        return QDir::toNativeSeparators(QDir::homePath());
    }
    if (normalized.startsWith(QStringLiteral("~/"))) {
        return QDir::toNativeSeparators(
            QDir(QDir::homePath()).absoluteFilePath(normalized.sliced(2)));
    }
    if (normalized.isEmpty()) {
        return {};
    }
    return QDir::toNativeSeparators(QFileInfo(normalized).absoluteFilePath());
}

PasswordAuthenticationReason
SshAuthenticationPolicy::passwordPromptReason(rfm::core::AuthenticationMode mode,
                                              AuthenticationResult result, bool hasExplicitIdentity)
{
    if (mode == rfm::core::AuthenticationMode::PasswordOnly) {
        return PasswordAuthenticationReason::PasswordOnly;
    }
    if (result == AuthenticationResult::Partial) {
        return PasswordAuthenticationReason::AdditionalPasswordRequired;
    }
    return hasExplicitIdentity ? PasswordAuthenticationReason::ExplicitKeyFailed
                               : PasswordAuthenticationReason::KeyOrAgentFailed;
}

} // namespace rfm::ssh
