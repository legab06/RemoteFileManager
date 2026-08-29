#pragma once

#include "remotefilemanager/core/LocalFileSystem.hpp"

namespace rfm::core::detail
{

[[nodiscard]] LocalFileOperationResult
executeLocalCopyMove(const LocalFileOperationRequest& request, LocalFileOperationBackend* backend,
                     const std::function<bool()>& cancellationRequested);

} // namespace rfm::core::detail
