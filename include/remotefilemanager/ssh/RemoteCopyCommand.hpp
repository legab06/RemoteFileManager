#pragma once

#include <QString>

namespace rfm::ssh {

class RemoteCopyCommand final {
public:
    [[nodiscard]] static QString quoteArgument(const QString& argument);
    [[nodiscard]] static QString build(
        const QString& source, const QString& destination, bool recursive);
};

}  // namespace rfm::ssh
