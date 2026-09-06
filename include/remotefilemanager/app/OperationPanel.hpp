#pragma once

#include "remotefilemanager/core/OperationProgress.hpp"

#include <QHash>
#include <QList>
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
    [[nodiscard]] bool removeTerminalOperation(quint64 id);
    void clearTerminalOperations();
    void restoreOperations(const QList<rfm::core::OperationProgress>& operations);

  public slots:
    void updateOperation(rfm::core::OperationProgress progress);

  signals:
    void pauseRequested(quint64 id);
    void resumeRequested(quint64 id);
    void cancelRequested(quint64 id);
    void removeTerminalRequested(quint64 id);
    void clearTerminalRequested();
    void terminalOperationsAvailableChanged(bool available);

  private:
    [[nodiscard]] static QString stateText(rfm::core::OperationState state);
    [[nodiscard]] QList<quint64> orderedOperationIds() const;
    [[nodiscard]] QList<quint64> currentOperationIds() const;
    bool storeOperation(rfm::core::OperationProgress progress);
    void rebuildRows(const QList<quint64>& operationIds);
    void rebuildRowMappings();
    void updateRow(int row, const rfm::core::OperationProgress& progress);
    void updateHistoryActions();

    QTableWidget* m_table{nullptr};
    QHash<quint64, int> m_rows;
    QHash<quint64, rfm::core::OperationProgress> m_progress;
    QHash<quint64, quint64> m_admissionOrder;
    QHash<quint64, quint64> m_terminalOrder;
    quint64 m_nextOrder{1};
};

} // namespace rfm::app
