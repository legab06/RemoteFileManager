#pragma once

#include <QString>

namespace rfm::core
{

enum class LocalPathFlavor { Native, Posix, Windows };

struct LocalDownloadPathResult {
    QString path;
    QString error;

    [[nodiscard]] bool succeeded() const { return !path.isEmpty() && error.isEmpty(); }
};

class LocalDownloadPath final
{
  public:
    [[nodiscard]] static LocalDownloadPathResult child(
        const QString& selectedRoot, const QString& parent, const QString& remoteName,
        LocalPathFlavor flavor = LocalPathFlavor::Native);
};

} // namespace rfm::core
