#pragma once

#include "remotefilemanager/core/OperationProgress.hpp"

#include <QHash>
#include <QWidget>

class QTableWidget;

namespace rfm::app
{

class OperationPanel final : public QWidget
{
    Q_OBJECT

  public:
    explicit OperationPanel(QWidget* parent = nullptr);

    [[nodiscard]] static QString formatBytes(quint64 bytes);
    [[nodiscard]] static QString formatSpeed(quint64 bytesPerSecond);

  public slots:
    void updateOperation(rfm::core::OperationProgress progress);

  signals:
    void pauseRequested(quint64 id);
    void resumeRequested(quint64 id);
    void cancelRequested(quint64 id);

  private:
    [[nodiscard]] static QString stateText(rfm::core::OperationState state);
    [[nodiscard]] int ensureRow(const rfm::core::OperationProgress& progress);
    void updateRow(int row, const rfm::core::OperationProgress& progress);

    QTableWidget* m_table{nullptr};
    QHash<quint64, int> m_rows;
    QHash<quint64, rfm::core::OperationProgress> m_progress;
};

} // namespace rfm::app
