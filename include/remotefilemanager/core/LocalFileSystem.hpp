#pragma once

#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/Storage.hpp"

#include <QList>
#include <QObject>
#include <QString>

namespace rfm::core
{

struct LocalDirectoryResult {
    QString path;
    QList<RemoteEntry> entries;
    QString error;

    [[nodiscard]] bool succeeded() const { return error.isEmpty(); }
};

class LocalFileSystem final
{
  public:
    [[nodiscard]] static LocalDirectoryResult listDirectory(const QString& path);
    [[nodiscard]] static QList<StorageVolume> mountedVolumes();
};

class LocalFileSystemWorker final : public QObject
{
    Q_OBJECT

  public slots:
    void listDirectory(quint64 requestId, QString path);
    void listVolumes();

  signals:
    void directoryListed(quint64 requestId, QString path, QList<rfm::core::RemoteEntry> entries);
    void directoryListingFailed(quint64 requestId, QString path, QString error);
    void volumesListed(QList<rfm::core::StorageVolume> volumes);
};

} // namespace rfm::core
