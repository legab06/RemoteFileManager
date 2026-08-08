#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>

namespace rfm::core
{

enum class TransferDirection { Upload, Download };
enum class TransferState {
    Queued,
    Preparing,
    Transferring,
    Paused,
    Finalizing,
    Cancelling,
    Completed,
    Cancelled,
    Failed,
};

struct TransferRequest {
    quint64 id{0};
    TransferDirection direction{TransferDirection::Upload};
    QString source;
    QString destination;
    bool directory{false};
};

struct TransferProgress {
    quint64 id{0};
    TransferState state{TransferState::Queued};
    QString source;
    QString destination;
    quint64 transferredBytes{0};
    quint64 totalBytes{0};
    quint64 bytesPerSecond{0};
    QString error;
    quint64 completedFiles{0};
    quint64 totalFiles{0};
    QString currentItem;
    TransferDirection direction{TransferDirection::Upload};
    bool directory{false};
};

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::TransferRequest)
Q_DECLARE_METATYPE(rfm::core::TransferProgress)
