#pragma once

#include "remotefilemanager/core/OperationProgress.hpp"

#include <QHash>
#include <QWidget>

class QTableWidget;
class QPushButton;

namespace rfm::app
{

class OperationPanel final : public QWidget
{
    Q_OBJECT

  public:
    explicit OperationPanel(QWidget* parent = nullptr);

    [[nodiscard]] static QString formatBytes(quint64 bytes);
    [[nodiscard]] static QString formatSpeed(quint64 bytesPerSecond);
    [[nodiscard]] bool removeTerminalOperation(quint64 id);
    void clearTerminalOperations();

  public slots:
    void updateOperation(rfm::core::OperationProgress progress);

  signals:
    void pauseRequested(quint64 id);
    void resumeRequested(quint64 id);
    void cancelRequested(quint64 id);
    void removeTerminalRequested(quint64 id);
    void clearTerminalRequested();

  private:
    [[nodiscard]] static QString stateText(rfm::core::OperationState state);
    [[nodiscard]] int ensureRow(const rfm::core::OperationProgress& progress);
    [[nodiscard]] quint64 selectedOperationId() const;
    void updateRow(int row, const rfm::core::OperationProgress& progress);
    void updateHistoryActions();

    QTableWidget* m_table{nullptr};
    QPushButton* m_removeButton{nullptr};
    QPushButton* m_clearButton{nullptr};
    QHash<quint64, int> m_rows;
    QHash<quint64, rfm::core::OperationProgress> m_progress;
};

} // namespace rfm::app
