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

enum class NativeServerCopyPrimitive { None, PosixCp };

struct RemoteCopyExecutionCapabilities {
    bool sftpCopyDataAvailable{false};
    CapabilitySupport nativeServerCopy{CapabilitySupport::Unknown};
    NativeServerCopyPrimitive nativePrimitive{NativeServerCopyPrimitive::None};
};

// Selects only methods supported by both the current server and the executing backend/session.
[[nodiscard]] RemoteCopyMethod
selectRemoteCopyMethod(const ServerCapabilities& serverCapabilities,
                       const RemoteCopyExecutionCapabilities& executionCapabilities);

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::NativeServerCopyPrimitive)
Q_DECLARE_METATYPE(rfm::core::RemoteCopyExecutionCapabilities)
