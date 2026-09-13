#include "remotefilemanager/core/RemoteCopyStrategy.hpp"

namespace rfm::core
{

RemoteCopyMethod
selectRemoteCopyMethod(const ServerCapabilities& serverCapabilities,
                       const RemoteCopyExecutionCapabilities& executionCapabilities)
{
    if (serverCapabilities.detectionState == CapabilityDetectionState::Detected &&
        serverCapabilities.copyDataVersion1 == CapabilitySupport::Supported &&
        executionCapabilities.sftpCopyDataAvailable) {
        return RemoteCopyMethod::SftpCopyData;
    }
    if (executionCapabilities.nativeServerCopy == CapabilitySupport::Supported &&
        executionCapabilities.nativePrimitive != NativeServerCopyPrimitive::None) {
        return RemoteCopyMethod::NativeServerCopy;
    }
    return RemoteCopyMethod::ClientMediatedSftp;
}

} // namespace rfm::core
