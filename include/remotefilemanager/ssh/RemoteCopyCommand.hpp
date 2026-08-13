#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace rfm::ssh
{

class RemoteCopyCommand final
{
  public:
    [[nodiscard]] static QString quoteArgument(const QString& argument);
    [[nodiscard]] static QString build(const QString& source, const QString& destination,
                                       bool recursive);
    [[nodiscard]] static QString buildMoveStaging(const QString& source,
                                                  const QString& destination);
    [[nodiscard]] static std::optional<quint32>
    parseMoveStagingStatus(const QByteArray& standardOutput);
    [[nodiscard]] static QString buildRemove(const QString& path, bool recursive,
                                             bool protectMountPoint = false,
                                             const QString& mountPointIdentity = {});
};

} // namespace rfm::ssh
