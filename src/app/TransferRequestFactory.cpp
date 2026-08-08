#include "remotefilemanager/app/TransferRequestFactory.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QDir>
#include <QFileInfo>

namespace rfm::app
{

std::optional<rfm::core::TransferRequest>
TransferRequestFactory::upload(quint64 id, const QString& localPath, const QString& remoteDirectory)
{
    const QFileInfo source(localPath);
    const QString name = source.fileName();
    const QString destination = rfm::core::RemotePath::join(remoteDirectory, name);
    if (id == 0 || localPath.isEmpty() || name.isEmpty() || destination.isEmpty()) {
        return std::nullopt;
    }
    return rfm::core::TransferRequest{id, rfm::core::TransferDirection::Upload,
                                      source.absoluteFilePath(), destination, source.isDir()};
}

std::optional<rfm::core::TransferRequest>
TransferRequestFactory::download(quint64 id, const rfm::core::RemoteSelection& remoteEntry,
                                 const QString& localDirectory)
{
    const QString name = rfm::core::RemotePath::fileName(remoteEntry.path);
    if (id == 0 || name.isEmpty() || localDirectory.isEmpty()) {
        return std::nullopt;
    }
    return rfm::core::TransferRequest{id, rfm::core::TransferDirection::Download, remoteEntry.path,
                                      QDir(localDirectory).filePath(name), remoteEntry.directory};
}

} // namespace rfm::app
