#pragma once

#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/TransferTypes.hpp"

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace rfm::core
{

enum class OperationKind { Upload, Download, RemoteCopy, RemoteMove };

enum class OperationState {
    Queued,
    Preparing,
    Running,
    Paused,
    Finalizing,
    Cancelling,
    Completed,
    Cancelled,
    Failed,
};

struct OperationProgress {
    quint64 id{0};
    OperationKind kind{OperationKind::Upload};
    OperationState state{OperationState::Queued};
    QStringList sources;
    QString destination;
    quint64 transferredBytes{0};
    quint64 totalBytes{0};
    quint64 bytesPerSecond{0};
    quint64 completedItems{0};
    quint64 totalItems{0};
    QString currentItem;
    QString error;
    bool byteProgressAvailable{false};
    bool pauseResumeSupported{false};
    bool cancellationSupported{false};
    QDateTime finishedAt;
    QString serverHost;
    quint16 serverPort{0};
};

struct RemoteOperationRequest {
    quint64 id{0};
    RemoteOperationKind kind{RemoteOperationKind::Copy};
    QList<RemoteSelection> sources;
    QString destinationDirectory;
};

[[nodiscard]] bool isTerminal(OperationState state);
[[nodiscard]] OperationProgress operationProgress(const TransferProgress& transfer);
[[nodiscard]] OperationProgress operationProgress(const RemoteOperationRequest& request,
                                                  OperationState state, const QString& error = {});
[[nodiscard]] OperationProgress beginRemoteOperation(quint64 id, OperationKind kind,
                                                     const QList<RemoteSelection>& sources,
                                                     const QString& destinationDirectory);
[[nodiscard]] OperationProgress finishRemoteOperation(const RemoteOperationResult& result,
                                                      const OperationProgress& started = {});

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::OperationProgress)
Q_DECLARE_METATYPE(rfm::core::RemoteOperationRequest)
