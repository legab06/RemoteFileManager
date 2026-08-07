#pragma once

#include <QString>

namespace rfm::core {

class RemotePath final {
public:
    [[nodiscard]] static bool isValidName(const QString& name);
    [[nodiscard]] static QString normalize(const QString& path);
    [[nodiscard]] static QString join(const QString& directory, const QString& name);
    [[nodiscard]] static QString parent(const QString& path);
    [[nodiscard]] static QString fileName(const QString& path);
    [[nodiscard]] static bool isProtected(const QString& path);
};

}  // namespace rfm::core
