#pragma once

#include <QList>
#include <QMetaType>
#include <QPair>
#include <QString>
#include <QtGlobal>

namespace rfm::core {

enum class RemoteBackendError {
    None,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    Unsupported,
    InvalidPath,
    CrossDevice,
    Failure,
};

struct RemoteBackendResult {
    RemoteBackendError error{RemoteBackendError::None};
    QString detail;

    [[nodiscard]] bool succeeded() const { return error == RemoteBackendError::None; }
};

struct RemoteNodeInfo {
    bool exists{false};
    bool directory{false};
};

struct RemoteProbeResult {
    RemoteBackendResult result;
    RemoteNodeInfo node;
};

struct RemoteDirectoryResult {
    RemoteBackendResult result;
    QList<QPair<QString, bool>> entries;
};

class RemoteFileBackend {
public:
    virtual ~RemoteFileBackend() = default;

    [[nodiscard]] virtual RemoteProbeResult probe(const QString& path) = 0;
    [[nodiscard]] virtual RemoteDirectoryResult list(const QString& path) = 0;
    [[nodiscard]] virtual RemoteBackendResult createDirectory(const QString& path) = 0;
    [[nodiscard]] virtual RemoteBackendResult rename(const QString& source,
                                                     const QString& destination) = 0;
    [[nodiscard]] virtual RemoteBackendResult removeFile(const QString& path) = 0;
    [[nodiscard]] virtual RemoteBackendResult removeDirectory(const QString& path) = 0;
    [[nodiscard]] virtual RemoteBackendResult copyOnServer(const QString& source,
                                                           const QString& destination,
                                                           bool recursive) = 0;
};

enum class RemoteOperationKind { CreateDirectory, Rename, Move, Copy, Remove };

struct RemoteSelection {
    QString path;
    bool directory{false};
};

struct RemoteItemResult {
    QString source;
    QString destination;
    bool success{false};
    QString error;
};

struct RemoteOperationResult {
    quint64 id{0};
    RemoteOperationKind kind{RemoteOperationKind::CreateDirectory};
    QList<RemoteItemResult> items;

    [[nodiscard]] bool allSucceeded() const;
};

class RemoteFileOperations final {
public:
    explicit RemoteFileOperations(RemoteFileBackend& backend);

    [[nodiscard]] RemoteOperationResult createDirectory(
        quint64 id, const QString& parent, const QString& name);
    [[nodiscard]] RemoteOperationResult rename(
        quint64 id, const QString& source, const QString& newName);
    [[nodiscard]] RemoteOperationResult move(
        quint64 id, const QList<RemoteSelection>& sources, const QString& destinationDirectory);
    [[nodiscard]] RemoteOperationResult copy(
        quint64 id, const QList<RemoteSelection>& sources, const QString& destinationDirectory);
    [[nodiscard]] RemoteOperationResult remove(
        quint64 id, const QList<RemoteSelection>& sources, bool recursive);

private:
    [[nodiscard]] RemoteItemResult transferOne(const RemoteSelection& source,
                                               const QString& destinationDirectory,
                                               bool copy);
    [[nodiscard]] RemoteBackendResult removeTree(
        const QString& path, bool directory, bool recursive);
    [[nodiscard]] static QString describeError(const RemoteBackendResult& result);

    RemoteFileBackend& m_backend;
};

}  // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::RemoteSelection)
Q_DECLARE_METATYPE(QList<rfm::core::RemoteSelection>)
Q_DECLARE_METATYPE(rfm::core::RemoteOperationResult)
