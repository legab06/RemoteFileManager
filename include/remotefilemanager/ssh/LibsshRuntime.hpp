#pragma once

#include <QString>

namespace rfm::ssh {

class LibsshRuntime final {
public:
    [[nodiscard]] static QString version();
};

}  // namespace rfm::ssh

