#pragma once

#include "remotefilemanager/core/Storage.hpp"

namespace rfm::core::detail
{

struct LinuxMountTable {
    QList<LinuxMountInfo> mounts;
    bool complete{false};
};

// Unfiltered entries, including bind, file and pseudo-filesystem mounts.
// Display consumers may still use valid entries from an incomplete table.
[[nodiscard]] LinuxMountTable parseLinuxMountTable(const QByteArray& contents);

} // namespace rfm::core::detail
