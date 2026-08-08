#pragma once

#include "remotefilemanager/core/TransferTypes.hpp"

#include <QHash>
#include <QWidget>

class QTableWidget;

namespace rfm::app
{

class TransferPanel final : public QWidget
{
    Q_OBJECT

  public:
    explicit TransferPanel(QWidget* parent = nullptr);

    [[nodiscard]] static QString formatBytes(quint64 bytes);
    [[nodiscard]] static QString formatSpeed(quint64 bytesPerSecond);

  public slots:
    void updateTransfer(rfm::core::TransferProgress progress);

  signals:
    void pauseRequested(quint64 id);
    void resumeRequested(quint64 id);
    void cancelRequested(quint64 id);

  private:
    [[nodiscard]] static QString stateText(rfm::core::TransferState state);
    [[nodiscard]] int ensureRow(const rfm::core::TransferProgress& progress);
    void updateRow(int row, const rfm::core::TransferProgress& progress);

    QTableWidget* m_table{nullptr};
    QHash<quint64, int> m_rows;
    QHash<quint64, rfm::core::TransferProgress> m_progress;
};

} // namespace rfm::app
