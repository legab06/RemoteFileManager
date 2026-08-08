#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace rfm::core
{

enum class TransferBackendError { None, NotFound, AlreadyExists, PermissionDenied, Io, Failure };

struct TransferBackendResult {
    TransferBackendError error{TransferBackendError::None};
    QString detail;
    [[nodiscard]] bool succeeded() const { return error == TransferBackendError::None; }
};

struct TransferNodeInfo {
    bool exists{false};
    bool directory{false};
    bool symbolicLink{false};
    quint64 size{0};
};
struct TransferStatResult {
    TransferBackendResult result;
    TransferNodeInfo node;
};

struct TransferDirectoryEntry {
    QString name;
    TransferNodeInfo node;
};

class RemoteTransferBackend
{
  public:
    virtual ~RemoteTransferBackend() = default;
    [[nodiscard]] virtual TransferStatResult stat(const QString& path) = 0;
    [[nodiscard]] virtual TransferBackendResult openRead(const QString& path, quint64& handle) = 0;
    [[nodiscard]] virtual TransferBackendResult openWriteExclusive(const QString& path,
                                                                   quint64& handle) = 0;
    [[nodiscard]] virtual TransferBackendResult read(quint64 handle, QByteArray& data,
                                                     qsizetype maximum) = 0;
    [[nodiscard]] virtual TransferBackendResult write(quint64 handle, const QByteArray& data) = 0;
    [[nodiscard]] virtual TransferBackendResult close(quint64 handle) = 0;
    [[nodiscard]] virtual TransferBackendResult createDirectory(const QString& path) = 0;
    [[nodiscard]] virtual TransferBackendResult rename(const QString& source,
                                                       const QString& destination) = 0;
    [[nodiscard]] virtual TransferBackendResult remove(const QString& path) = 0;
    [[nodiscard]] virtual TransferBackendResult openDirectory(const QString& path,
                                                              quint64& handle) = 0;
    [[nodiscard]] virtual TransferBackendResult
    readDirectory(quint64 handle, std::optional<TransferDirectoryEntry>& entry) = 0;
    [[nodiscard]] virtual TransferBackendResult closeDirectory(quint64 handle) = 0;
};

} // namespace rfm::core
