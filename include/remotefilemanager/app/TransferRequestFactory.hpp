#pragma once

#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/TransferTypes.hpp"

#include <optional>

namespace rfm::app
{

class TransferRequestFactory final
{
  public:
    [[nodiscard]] static std::optional<rfm::core::TransferRequest>
    upload(quint64 id, const QString& localPath, const QString& remoteDirectory);
    [[nodiscard]] static std::optional<rfm::core::TransferRequest>
    download(quint64 id, const rfm::core::RemoteSelection& remoteEntry,
             const QString& localDirectory, QString* error = nullptr);
};

} // namespace rfm::app
