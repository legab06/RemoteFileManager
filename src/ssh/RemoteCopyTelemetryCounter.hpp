#pragma once

#include "remotefilemanager/core/ServerSideCopyJob.hpp"

#include <limits>
#include <optional>

namespace rfm::ssh::detail
{

class RemoteCopyTelemetryCounter final
{
  public:
    void reset(std::optional<quint64> totalBytes = std::nullopt, bool byteProgressAvailable = false)
    {
        m_telemetry = {};
        if (totalBytes.has_value()) {
            m_telemetry.totalBytes = *totalBytes;
        }
        m_telemetry.byteProgressAvailable = totalBytes.has_value() || byteProgressAvailable;
    }

    [[nodiscard]] bool recordWrite(qint64 writtenBytes, qsizetype remainingBytes)
    {
        if (writtenBytes <= 0 || writtenBytes > remainingBytes) {
            return false;
        }
        const quint64 bytes = static_cast<quint64>(writtenBytes);
        if (bytes > std::numeric_limits<quint64>::max() - m_telemetry.transferredBytes) {
            return false;
        }
        m_telemetry.transferredBytes += bytes;
        return true;
    }

    [[nodiscard]] const rfm::core::RemoteCopyTelemetry& telemetry() const { return m_telemetry; }

  private:
    rfm::core::RemoteCopyTelemetry m_telemetry;
};

} // namespace rfm::ssh::detail
