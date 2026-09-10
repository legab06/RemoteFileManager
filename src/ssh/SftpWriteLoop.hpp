#pragma once

#include <QByteArray>

#include <functional>

namespace rfm::ssh::detail
{

enum class SftpWriteLoopResult { Completed, WriteError, NoProgress };

using SftpWriteFunction = std::function<qint64(const char* data, qsizetype size)>;

[[nodiscard]] inline SftpWriteLoopResult writeSftpBuffer(const QByteArray& data,
                                                         const SftpWriteFunction& write)
{
    if (data.isEmpty()) {
        return SftpWriteLoopResult::Completed;
    }
    qsizetype offset = 0;
    while (offset < data.size()) {
        const qsizetype remaining = data.size() - offset;
        const qint64 written = write(data.constData() + offset, remaining);
        if (written < 0 || written > remaining) {
            return SftpWriteLoopResult::WriteError;
        }
        if (written == 0) {
            return SftpWriteLoopResult::NoProgress;
        }
        offset += static_cast<qsizetype>(written);
    }
    return SftpWriteLoopResult::Completed;
}

} // namespace rfm::ssh::detail
