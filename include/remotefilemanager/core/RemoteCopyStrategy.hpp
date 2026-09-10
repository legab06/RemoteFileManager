#pragma once

#include "remotefilemanager/core/ServerCapabilities.hpp"

namespace rfm::core
{

enum class RemoteCopyMethod {
    SftpCopyData,
    // Reserved for a future portable and safe native-server implementation.
    NativeServerCopy,
    ClientMediatedSftp
};

// Selects only methods supported by the verified SFTP capabilities provided.
[[nodiscard]] RemoteCopyMethod selectRemoteCopyMethod(const ServerCapabilities& capabilities);

} // namespace rfm::core
