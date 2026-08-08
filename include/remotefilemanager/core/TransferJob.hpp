#pragma once

#include "remotefilemanager/core/TransferTypes.hpp"

namespace rfm::core
{

class TransferJob
{
  public:
    virtual ~TransferJob() = default;

    virtual void step() = 0;
    [[nodiscard]] virtual bool requestPause() = 0;
    [[nodiscard]] virtual bool resume() = 0;
    [[nodiscard]] virtual bool requestCancel() = 0;
    [[nodiscard]] virtual bool isFinished() const = 0;
    [[nodiscard]] virtual bool isPaused() const = 0;
    [[nodiscard]] virtual const TransferProgress& progress() const = 0;
};

} // namespace rfm::core
