#pragma once

#include "remotefilemanager/core/LocalFileSystem.hpp"

namespace rfm::core::detail
{

[[nodiscard]] LocalFileOperationResult
executeLocalCopyMove(const LocalFileOperationRequest& request, LocalFileOperationBackend* backend,
                     const std::function<bool()>& cancellationRequested);

[[nodiscard]] bool destinationMayReenterSourceOnLinux(const QString& source,
                                                       const QString& destination,
                                                       const QByteArray& mountInfo);

} // namespace rfm::core::detail
