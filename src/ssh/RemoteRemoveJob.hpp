#pragma once

#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/RemotePath.hpp"

#include <memory>
#include <vector>

namespace rfm::ssh
{

enum class RemoteRemoveReadState { Entry, End, Error };

struct RemoteRemoveRead {
    RemoteRemoveReadState state{RemoteRemoveReadState::End};
    QString name;
    bool directory{false};
    rfm::core::RemoteBackendResult result;
};

class RemoteRemoveDirectory
{
  public:
    virtual ~RemoteRemoveDirectory() = default;
    [[nodiscard]] virtual RemoteRemoveRead read() = 0;
};

struct RemoteRemoveOpenResult {
    rfm::core::RemoteBackendResult result;
    std::unique_ptr<RemoteRemoveDirectory> directory;
};

class RemoteRemoveBackend
{
  public:
    virtual ~RemoteRemoveBackend() = default;
    [[nodiscard]] virtual rfm::core::RemoteBackendResult
    preflightDirectory(const QString& path) = 0;
    [[nodiscard]] virtual RemoteRemoveOpenResult openDirectory(const QString& path) = 0;
    [[nodiscard]] virtual rfm::core::RemoteBackendResult removeFile(const QString& path) = 0;
    [[nodiscard]] virtual rfm::core::RemoteBackendResult removeDirectory(const QString& path) = 0;
};

class RemoteRemoveJob final
{
  public:
    static constexpr qsizetype operationsPerStep = 32;

    RemoteRemoveJob(RemoteRemoveBackend& backend, quint64 id,
                    QList<rfm::core::RemoteSelection> sources, bool recursive)
        : m_backend(backend), m_sources(std::move(sources))
    {
        m_result = {id, rfm::core::RemoteOperationKind::Remove, {}};
        m_result.items.reserve(m_sources.size());
        for (const rfm::core::RemoteSelection& source : std::as_const(m_sources)) {
            m_result.items.push_back(
                {rfm::core::RemotePath::normalize(source.path), {}, false, {}});
        }
        m_recursive = recursive;
    }

    [[nodiscard]] bool isFinished() const { return m_finished; }
    [[nodiscard]] const rfm::core::RemoteOperationResult& result() const { return m_result; }

    void cancel(const QString& error)
    {
        if (!m_finished) {
            finishCancelled(error);
        }
    }

    void step()
    {
        if (m_finished) {
            return;
        }
        for (qsizetype operations = 0; operations < operationsPerStep && !m_finished;
             ++operations) {
            if (m_preflighting) {
                stepPreflight();
            } else {
                stepRemoval();
            }
        }
    }

  private:
    struct DirectoryFrame {
        QString path;
        qsizetype sourceIndex{0};
        std::unique_ptr<RemoteRemoveDirectory> directory;
    };

    struct RemoveTask {
        QString path;
        bool directory{false};
        qsizetype sourceIndex{0};
    };

    void stepPreflight()
    {
        if (!m_directories.empty()) {
            DirectoryFrame& frame = m_directories.back();
            const RemoteRemoveRead read = frame.directory->read();
            if (read.state == RemoteRemoveReadState::Error) {
                finishPreflightFailure(describe(read.result));
                return;
            }
            if (read.state == RemoteRemoveReadState::End) {
                m_plan.push_back({frame.path, true, frame.sourceIndex});
                m_directories.pop_back();
                return;
            }
            if (read.name == QStringLiteral(".") || read.name == QStringLiteral("..")) {
                return;
            }
            if (!rfm::core::RemotePath::isValidName(read.name)) {
                finishPreflightFailure(
                    QStringLiteral("The remote removal tree contains an invalid entry name."));
                return;
            }
            const QString path = rfm::core::RemotePath::join(frame.path, read.name);
            if (path.isEmpty() || rfm::core::RemotePath::isProtected(path)) {
                finishPreflightFailure(QStringLiteral("Invalid or protected remote path."));
                return;
            }
            if (!read.directory) {
                m_plan.push_back({path, false, frame.sourceIndex});
                return;
            }
            openPreflightDirectory(path, frame.sourceIndex);
            return;
        }

        if (m_nextSource == m_sources.size()) {
            m_preflighting = false;
            m_remainingActions.fill(0, m_sources.size());
            for (const RemoveTask& task : std::as_const(m_plan)) {
                ++m_remainingActions[task.sourceIndex];
            }
            return;
        }

        const qsizetype sourceIndex = m_nextSource++;
        const rfm::core::RemoteSelection& source = m_sources.at(sourceIndex);
        const QString path = rfm::core::RemotePath::normalize(source.path);
        if (path.isEmpty() || rfm::core::RemotePath::isProtected(path)) {
            finishPreflightFailure(QStringLiteral("Invalid or protected remote path."));
            return;
        }
        if (!source.directory) {
            m_plan.push_back({path, false, sourceIndex});
            return;
        }
        if (!m_recursive) {
            finishPreflightFailure(
                QStringLiteral("Deleting a folder requires recursive confirmation."));
            return;
        }
        openPreflightDirectory(path, sourceIndex);
    }

    void openPreflightDirectory(const QString& path, qsizetype sourceIndex)
    {
        const rfm::core::RemoteBackendResult preflight = m_backend.preflightDirectory(path);
        if (!preflight.succeeded()) {
            finishPreflightFailure(describe(preflight));
            return;
        }
        RemoteRemoveOpenResult open = m_backend.openDirectory(path);
        if (!open.result.succeeded() || open.directory == nullptr) {
            finishPreflightFailure(describe(open.result));
            return;
        }
        m_directories.push_back({path, sourceIndex, std::move(open.directory)});
    }

    void stepRemoval()
    {
        if (m_nextTask == m_plan.size()) {
            m_finished = true;
            return;
        }
        const RemoveTask& task = m_plan.at(m_nextTask++);
        if (m_failedSources.contains(task.sourceIndex)) {
            return;
        }
        const rfm::core::RemoteBackendResult result =
            task.directory ? m_backend.removeDirectory(task.path) : m_backend.removeFile(task.path);
        if (!result.succeeded()) {
            m_failedSources.insert(task.sourceIndex);
            rfm::core::RemoteItemResult& item = m_result.items[task.sourceIndex];
            item.error = describe(result);
            return;
        }
        if (--m_remainingActions[task.sourceIndex] == 0) {
            m_result.items[task.sourceIndex].success = true;
        }
    }

    void finishPreflightFailure(const QString& error)
    {
        m_directories.clear();
        for (rfm::core::RemoteItemResult& item : m_result.items) {
            item.success = false;
            item.error = error;
        }
        m_finished = true;
    }

    void finishCancelled(const QString& error)
    {
        m_directories.clear();
        for (rfm::core::RemoteItemResult& item : m_result.items) {
            if (!item.success) {
                item.error = error;
            }
        }
        m_finished = true;
    }

    [[nodiscard]] static QString describe(const rfm::core::RemoteBackendResult& result)
    {
        if (!result.detail.isEmpty()) {
            return result.detail;
        }
        switch (result.error) {
        case rfm::core::RemoteBackendError::NotFound:
            return QStringLiteral("The remote item was not found.");
        case rfm::core::RemoteBackendError::AlreadyExists:
            return QStringLiteral("A remote item already exists at this destination.");
        case rfm::core::RemoteBackendError::PermissionDenied:
            return QStringLiteral("Permission denied by the server.");
        case rfm::core::RemoteBackendError::Unsupported:
            return QStringLiteral("This operation is not supported by the server.");
        case rfm::core::RemoteBackendError::InvalidPath:
            return QStringLiteral("Invalid or protected remote path.");
        case rfm::core::RemoteBackendError::CrossDevice:
            return QStringLiteral("The source and destination are on different filesystems.");
        case rfm::core::RemoteBackendError::Failure:
            return QStringLiteral("The remote operation failed.");
        case rfm::core::RemoteBackendError::None:
            return QStringLiteral("The remote removal preflight failed.");
        }
        return QStringLiteral("The remote removal preflight failed.");
    }

    RemoteRemoveBackend& m_backend;
    QList<rfm::core::RemoteSelection> m_sources;
    rfm::core::RemoteOperationResult m_result;
    std::vector<DirectoryFrame> m_directories;
    QList<RemoveTask> m_plan;
    QSet<qsizetype> m_failedSources;
    QList<qsizetype> m_remainingActions;
    qsizetype m_nextSource{0};
    qsizetype m_nextTask{0};
    bool m_recursive{false};
    bool m_preflighting{true};
    bool m_finished{false};
};

} // namespace rfm::ssh
