#include "remotefilemanager/ssh/LibsshRuntime.hpp"

#include <libssh/libssh.h>

namespace rfm::ssh {

QString LibsshRuntime::version()
{
    const char* const version = ssh_version(0);
    return version == nullptr ? QStringLiteral("unknown") : QString::fromUtf8(version);
}

}  // namespace rfm::ssh
