#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace rfm::core
{

enum class TransferBackendError {
    None,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    ConnectionLost,
    Unsupported,
    Io,
    Failure,
};

enum class TransferNodeType {
    Unknown,
    RegularFile,
    Directory,
    SymbolicLink,
    Fifo,
    Socket,
    CharacterDevice,
    BlockDevice,
    Other,
};

struct TransferBackendResult {
    TransferBackendError error{TransferBackendError::None};
    QString detail;
    [[nodiscard]] bool succeeded() const { return error == TransferBackendError::None; }
};

struct TransferNodeInfo {
    bool exists{false};
    TransferNodeType type{TransferNodeType::Unknown};
    quint64 size{0};

    [[nodiscard]] bool isDirectory() const { return type == TransferNodeType::Directory; }
    [[nodiscard]] bool isSymbolicLink() const { return type == TransferNodeType::SymbolicLink; }
    [[nodiscard]] bool isRegularFile() const { return type == TransferNodeType::RegularFile; }
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
    [[nodiscard]] virtual bool connectionAlive() const { return true; }
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

Q_DECLARE_METATYPE(rfm::core::TransferBackendError)
Q_DECLARE_METATYPE(rfm::core::TransferNodeType)
