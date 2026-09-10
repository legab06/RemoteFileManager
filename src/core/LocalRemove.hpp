#pragma once

#include "remotefilemanager/core/LocalFileSystem.hpp"

namespace rfm::core::detail
{

enum class RemovalBoundary { Clear, MountPoint, Unavailable };
using RemovalBoundaryProbe = std::function<RemovalBoundary(const QString&)>;

// Private injection seam; an empty probe selects the platform implementation.
[[nodiscard]] LocalFileOperationResult executeLocalRemove(const LocalFileOperationRequest& request,
                                                          const RemovalBoundaryProbe& probe = {});

// Uses the same raw parser as the volume-list parser, without its display filters.
[[nodiscard]] RemovalBoundaryProbe linuxRemovalBoundaryProbe(const QByteArray& mountInfo);

} // namespace rfm::core::detail
