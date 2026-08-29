#include "remotefilemanager/app/OperationPanel.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace rfm::app
{
namespace
{

enum Column {
    KindColumn,
    SourceColumn,
    DestinationColumn,
    StateColumn,
    ProgressColumn,
    SpeedColumn,
    ActionsColumn,
    ErrorColumn,
    ColumnCount,
};

bool isTransfer(rfm::core::OperationKind kind)
{
    return kind == rfm::core::OperationKind::Upload || kind == rfm::core::OperationKind::Download;
}

QString kindText(rfm::core::OperationKind kind)
{
    switch (kind) {
    case rfm::core::OperationKind::Upload:
        return OperationPanel::tr("↑ Upload");
    case rfm::core::OperationKind::Download:
        return OperationPanel::tr("↓ Download");
    case rfm::core::OperationKind::RemoteCopy:
        return OperationPanel::tr("Remote Copy");
    case rfm::core::OperationKind::RemoteMove:
        return OperationPanel::tr("Remote Move");
    }
    return {};
}

QString displaySource(const rfm::core::OperationProgress& progress)
{
    if (progress.sources.isEmpty()) {
        return {};
    }
    const QString source = progress.sources.constFirst();
    QString name;
    if (progress.kind == rfm::core::OperationKind::Upload) {
        name = QFileInfo(source).fileName();
    } else {
        name = rfm::core::RemotePath::fileName(source);
    }
    if (name.isEmpty()) {
        name = source;
    }
    if (progress.sources.size() > 1) {
        name = OperationPanel::tr("%1 (+%2)").arg(name).arg(progress.sources.size() - 1);
    }
    return name;
}

QString displayCurrentItem(const rfm::core::OperationProgress& progress)
{
    if (progress.currentItem.isEmpty() ||
        (!progress.sources.isEmpty() && progress.currentItem == progress.sources.constFirst())) {
        return {};
    }
    const QString name = progress.kind == rfm::core::OperationKind::Upload
                             ? QFileInfo(progress.currentItem).fileName()
                             : rfm::core::RemotePath::fileName(progress.currentItem);
    return name.isEmpty() ? progress.currentItem : name;
}

int operationCategory(rfm::core::OperationState state)
{
    if (state != rfm::core::OperationState::Queued && !rfm::core::isTerminal(state)) {
        return 0;
    }
    return state == rfm::core::OperationState::Queued ? 1 : 2;
}

QString displayServer(const rfm::core::OperationProgress& progress)
{
    if (progress.serverHost.isEmpty() || progress.serverPort == 0) {
        return {};
    }
    const QString host = progress.serverHost.contains(QChar{':'})
                             ? QStringLiteral("[%1]").arg(progress.serverHost)
                             : progress.serverHost;
    return QStringLiteral("%1:%2").arg(host, QString::number(progress.serverPort));
}

} // namespace

OperationPanel::OperationPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("operationPanel"));
    auto* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* const historyActions = new QHBoxLayout;
    historyActions->addStretch();
    m_removeButton = new QPushButton(tr("Remove selected"), this);
    m_removeButton->setObjectName(QStringLiteral("removeOperationButton"));
    m_removeButton->setEnabled(false);
    m_clearButton = new QPushButton(tr("Clear history"), this);
    m_clearButton->setObjectName(QStringLiteral("clearOperationHistoryButton"));
    m_clearButton->setEnabled(false);
    historyActions->addWidget(m_removeButton);
    historyActions->addWidget(m_clearButton);
    layout->addLayout(historyActions);

    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("operationTable"));
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({tr("Operation"), tr("Source"), tr("Destination"),
                                        tr("Status"), tr("Progress"), tr("Speed"), tr("Actions"),
                                        tr("Error")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    QHeaderView* const header = m_table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(KindColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(SourceColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(DestinationColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(StateColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ProgressColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(SpeedColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ActionsColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ErrorColumn, QHeaderView::Stretch);
    layout->addWidget(m_table);

    connect(m_table, &QTableWidget::itemSelectionChanged, this,
            &OperationPanel::updateHistoryActions);
    connect(m_removeButton, &QPushButton::clicked, this, [this] {
        const quint64 id = selectedOperationId();
        if (id != 0) {
            emit removeTerminalRequested(id);
        }
    });
    connect(m_clearButton, &QPushButton::clicked, this, &OperationPanel::clearTerminalRequested);
}

QString OperationPanel::formatBytes(quint64 bytes)
{
    constexpr std::array<const char*, 6> units{"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const int precision = value < 10.0 ? 1 : 0;
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', precision), units.at(unit));
}

QString OperationPanel::formatSpeed(quint64 bytesPerSecond)
{
    return bytesPerSecond == 0 ? QStringLiteral("—")
                               : QStringLiteral("%1/s").arg(formatBytes(bytesPerSecond));
}

QString OperationPanel::stateText(rfm::core::OperationState state)
{
    switch (state) {
    case rfm::core::OperationState::Queued:
        return tr("Queued");
    case rfm::core::OperationState::Preparing:
        return tr("Preparing");
    case rfm::core::OperationState::Running:
        return tr("Running");
    case rfm::core::OperationState::Paused:
        return tr("Paused");
    case rfm::core::OperationState::Finalizing:
        return tr("Finalizing");
    case rfm::core::OperationState::Cancelling:
        return tr("Cancelling");
    case rfm::core::OperationState::Completed:
        return tr("Completed");
    case rfm::core::OperationState::Cancelled:
        return tr("Cancelled");
    case rfm::core::OperationState::Failed:
        return tr("Failed");
    }
    return {};
}

QList<quint64> OperationPanel::orderedOperationIds() const
{
    QList<quint64> operationIds = m_progress.keys();
    std::sort(operationIds.begin(), operationIds.end(), [this](quint64 leftId, quint64 rightId) {
        const rfm::core::OperationProgress& left = m_progress.value(leftId);
        const rfm::core::OperationProgress& right = m_progress.value(rightId);
        const int leftCategory = operationCategory(left.state);
        const int rightCategory = operationCategory(right.state);
        if (leftCategory != rightCategory) {
            return leftCategory < rightCategory;
        }
        if (leftCategory != 2) {
            return m_admissionOrder.value(leftId) < m_admissionOrder.value(rightId);
        }

        const bool leftHasFinishedAt = left.finishedAt.isValid();
        const bool rightHasFinishedAt = right.finishedAt.isValid();
        if (leftHasFinishedAt != rightHasFinishedAt) {
            return leftHasFinishedAt;
        }
        if (leftHasFinishedAt && left.finishedAt != right.finishedAt) {
            return left.finishedAt > right.finishedAt;
        }
        const quint64 leftTerminalOrder = m_terminalOrder.value(leftId);
        const quint64 rightTerminalOrder = m_terminalOrder.value(rightId);
        if (leftTerminalOrder != rightTerminalOrder) {
            return leftTerminalOrder > rightTerminalOrder;
        }
        return m_admissionOrder.value(leftId) > m_admissionOrder.value(rightId);
    });
    return operationIds;
}

QList<quint64> OperationPanel::currentOperationIds() const
{
    QList<quint64> operationIds;
    operationIds.reserve(m_table->rowCount());
    for (int row = 0; row < m_table->rowCount(); ++row) {
        operationIds.push_back(m_table->item(row, KindColumn)->data(Qt::UserRole).toULongLong());
    }
    return operationIds;
}

void OperationPanel::rebuildRows(const QList<quint64>& operationIds)
{
    QList<quint64> selectedIds;
    for (const QModelIndex& index : m_table->selectionModel()->selectedRows()) {
        selectedIds.push_back(
            m_table->item(index.row(), KindColumn)->data(Qt::UserRole).toULongLong());
    }
    const quint64 currentId =
        m_table->currentRow() < 0
            ? 0
            : m_table->item(m_table->currentRow(), KindColumn)->data(Qt::UserRole).toULongLong();
    QScrollBar* const scrollBar = m_table->verticalScrollBar();
    const bool wasAtTop = scrollBar->value() == scrollBar->minimum();
    const int anchorRow = wasAtTop ? -1 : m_table->rowAt(0);
    const quint64 anchorId =
        anchorRow < 0 ? 0 : m_table->item(anchorRow, KindColumn)->data(Qt::UserRole).toULongLong();

    const QSignalBlocker blocker(m_table);
    m_table->clearContents();
    m_table->setRowCount(0);
    m_rows.clear();
    for (const quint64 id : operationIds) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        for (int column = 0; column < ColumnCount; ++column) {
            m_table->setItem(row, column, new QTableWidgetItem);
        }
        m_rows.insert(id, row);
        updateRow(row, m_progress.value(id));
    }
    m_table->resizeRowsToContents();

    if (currentId != 0 && m_rows.contains(currentId)) {
        m_table->setCurrentCell(m_rows.value(currentId), KindColumn, QItemSelectionModel::NoUpdate);
    }
    for (const quint64 id : std::as_const(selectedIds)) {
        if (m_rows.contains(id)) {
            m_table->selectionModel()->select(m_table->model()->index(m_rows.value(id), KindColumn),
                                              QItemSelectionModel::Select |
                                                  QItemSelectionModel::Rows);
        }
    }
    if (wasAtTop) {
        scrollBar->setValue(scrollBar->minimum());
    } else if (anchorId != 0 && m_rows.contains(anchorId)) {
        m_table->scrollToItem(m_table->item(m_rows.value(anchorId), KindColumn),
                              QAbstractItemView::PositionAtTop);
    }
}

void OperationPanel::rebuildRowMappings()
{
    m_rows.clear();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const quint64 id = m_table->item(row, KindColumn)->data(Qt::UserRole).toULongLong();
        m_rows.insert(id, row);
    }
}

quint64 OperationPanel::selectedOperationId() const
{
    const QList<QTableWidgetItem*> selected = m_table->selectedItems();
    if (selected.isEmpty()) {
        return 0;
    }
    return m_table->item(selected.constFirst()->row(), KindColumn)
        ->data(Qt::UserRole)
        .toULongLong();
}

void OperationPanel::updateRow(int row, const rfm::core::OperationProgress& progress)
{
    QTableWidgetItem* const kindItem = m_table->item(row, KindColumn);
    kindItem->setText(kindText(progress.kind));
    kindItem->setData(Qt::UserRole, progress.id);
    const QString server = displayServer(progress);
    kindItem->setToolTip(server.isEmpty() ? QString{} : tr("Server: %1").arg(server));
    QTableWidgetItem* const sourceItem = m_table->item(row, SourceColumn);
    const QString source = displaySource(progress);
    const QString currentItem = displayCurrentItem(progress);
    sourceItem->setText(currentItem.isEmpty() ? source
                                              : tr("%1\nCurrent: %2").arg(source, currentItem));
    QString sourceToolTip = progress.sources.join(QChar{'\n'});
    if (!currentItem.isEmpty()) {
        sourceToolTip += tr("\n\nCurrent item: %1").arg(progress.currentItem);
    }
    sourceItem->setToolTip(sourceToolTip);
    QTableWidgetItem* const destinationItem = m_table->item(row, DestinationColumn);
    destinationItem->setText(progress.destination);
    destinationItem->setToolTip(progress.destination);
    m_table->item(row, StateColumn)->setText(stateText(progress.state));
    const bool isRunning = progress.state == rfm::core::OperationState::Running;
    m_table->item(row, SpeedColumn)
        ->setText(isRunning && progress.byteProgressAvailable ? formatSpeed(progress.bytesPerSecond)
                                                              : QStringLiteral("—"));
    QTableWidgetItem* const errorItem = m_table->item(row, ErrorColumn);
    errorItem->setText(progress.error);
    errorItem->setToolTip(progress.error);

    if (progress.byteProgressAvailable) {
        auto* progressBar = qobject_cast<QProgressBar*>(m_table->cellWidget(row, ProgressColumn));
        if (progressBar == nullptr) {
            progressBar = new QProgressBar(m_table);
            progressBar->setObjectName(QStringLiteral("operationProgressBar"));
            progressBar->setTextVisible(true);
            progressBar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
            progressBar->setMinimumWidth(
                progressBar->fontMetrics().horizontalAdvance(
                    QStringLiteral("999.9 GiB / 999.9 GiB")) +
                progressBar->style()->pixelMetric(QStyle::PM_ProgressBarChunkWidth) * 2);
            m_table->setCellWidget(row, ProgressColumn, progressBar);
        }
        if (progress.totalBytes == 0 && !rfm::core::isTerminal(progress.state)) {
            progressBar->setRange(0, 0);
            progressBar->setFormat(progress.totalItems == 0 ? tr("Calculating size")
                                                            : tr("Calculating size · %1 / %2 files")
                                                                  .arg(progress.completedItems)
                                                                  .arg(progress.totalItems));
        } else {
            progressBar->setRange(0, 1000);
            const double ratio =
                progress.totalBytes == 0
                    ? (progress.state == rfm::core::OperationState::Completed ? 1.0 : 0.0)
                    : std::clamp(static_cast<double>(progress.transferredBytes) /
                                     static_cast<double>(progress.totalBytes),
                                 0.0, 1.0);
            progressBar->setValue(static_cast<int>(std::round(ratio * 1000.0)));
            QString progressText = progress.totalBytes == 0
                                       ? formatBytes(progress.transferredBytes)
                                       : tr("%1 / %2").arg(formatBytes(progress.transferredBytes),
                                                           formatBytes(progress.totalBytes));
            if (progress.totalItems > 0) {
                progressText +=
                    tr(" · %1 / %2 files").arg(progress.completedItems).arg(progress.totalItems);
            }
            progressBar->setFormat(progressText);
        }
    } else if (!rfm::core::isTerminal(progress.state)) {
        auto* progressBar = qobject_cast<QProgressBar*>(m_table->cellWidget(row, ProgressColumn));
        if (progressBar == nullptr) {
            progressBar = new QProgressBar(m_table);
            progressBar->setObjectName(QStringLiteral("operationProgressBar"));
            m_table->setCellWidget(row, ProgressColumn, progressBar);
        }
        progressBar->setRange(0, 0);
        progressBar->setFormat(progress.state == rfm::core::OperationState::Cancelling
                                   ? tr("Cancelling")
                                   : tr("Working"));
    } else {
        m_table->removeCellWidget(row, ProgressColumn);
        m_table->item(row, ProgressColumn)
            ->setText(
                tr("%1 / %2 completed").arg(progress.completedItems).arg(progress.totalItems));
    }

    if (isTransfer(progress.kind) || progress.cancellationSupported) {
        QWidget* actions = m_table->cellWidget(row, ActionsColumn);
        if (actions == nullptr) {
            actions = new QWidget(m_table);
            auto* const actionsLayout = new QHBoxLayout(actions);
            actionsLayout->setContentsMargins(2, 0, 2, 0);
            actionsLayout->setSpacing(4);
            actionsLayout->setSizeConstraint(QLayout::SetMinimumSize);
            actions->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
            auto* const pauseResume = new QPushButton(actions);
            pauseResume->setObjectName(QStringLiteral("pauseResumeButton"));
            auto* const cancel = new QPushButton(tr("Cancel"), actions);
            cancel->setObjectName(QStringLiteral("cancelTransferButton"));
            pauseResume->setText(tr("Resume"));
            pauseResume->setMinimumWidth(pauseResume->sizeHint().width());
            pauseResume->setText(tr("Pause"));
            cancel->setMinimumWidth(cancel->sizeHint().width());
            actionsLayout->addWidget(pauseResume);
            actionsLayout->addWidget(cancel);
            actions->setMinimumWidth(actionsLayout->sizeHint().width());
            m_table->verticalHeader()->setMinimumSectionSize(
                std::max(pauseResume->sizeHint().height(), cancel->sizeHint().height()) +
                actionsLayout->contentsMargins().top() + actionsLayout->contentsMargins().bottom());
            connect(pauseResume, &QPushButton::clicked, this, [this, id = progress.id] {
                const auto current = m_progress.constFind(id);
                if (current == m_progress.cend()) {
                    return;
                }
                if (current->state == rfm::core::OperationState::Paused) {
                    emit resumeRequested(id);
                } else if (current->state == rfm::core::OperationState::Running) {
                    emit pauseRequested(id);
                }
            });
            connect(cancel, &QPushButton::clicked, this,
                    [this, id = progress.id] { emit cancelRequested(id); });
            m_table->setCellWidget(row, ActionsColumn, actions);
        }
        auto* const pauseResume =
            actions->findChild<QPushButton*>(QStringLiteral("pauseResumeButton"));
        auto* const cancel =
            actions->findChild<QPushButton*>(QStringLiteral("cancelTransferButton"));
        const bool canPause = isTransfer(progress.kind) && progress.pauseResumeSupported &&
                              progress.state == rfm::core::OperationState::Running;
        const bool canResume = isTransfer(progress.kind) && progress.pauseResumeSupported &&
                               progress.state == rfm::core::OperationState::Paused;
        pauseResume->setVisible(canPause || canResume);
        pauseResume->setEnabled(canPause || canResume);
        pauseResume->setText(canResume ? tr("Resume") : tr("Pause"));
        cancel->setVisible(progress.cancellationSupported &&
                           !rfm::core::isTerminal(progress.state));
        cancel->setEnabled(progress.state != rfm::core::OperationState::Cancelling);
    } else {
        m_table->removeCellWidget(row, ActionsColumn);
        m_table->item(row, ActionsColumn)->setText(QStringLiteral("—"));
    }
}

void OperationPanel::updateOperation(rfm::core::OperationProgress progress)
{
    const quint64 id = progress.id;
    if (!storeOperation(std::move(progress))) {
        return;
    }
    const QList<quint64> operationIds = orderedOperationIds();
    if (operationIds != currentOperationIds()) {
        rebuildRows(operationIds);
    } else {
        const int row = m_rows.value(id);
        updateRow(row, m_progress.value(id));
        m_table->resizeRowToContents(row);
    }
    updateHistoryActions();
}

void OperationPanel::restoreOperations(const QList<rfm::core::OperationProgress>& operations)
{
    bool hasOperations = false;
    for (const rfm::core::OperationProgress& operation : operations) {
        hasOperations = storeOperation(operation) || hasOperations;
    }
    if (hasOperations) {
        rebuildRows(orderedOperationIds());
    }
    updateHistoryActions();
}

bool OperationPanel::storeOperation(rfm::core::OperationProgress progress)
{
    if (progress.id == 0) {
        return false;
    }
    const auto existing = m_progress.constFind(progress.id);
    const bool wasTerminal =
        existing != m_progress.cend() && rfm::core::isTerminal(existing->state);
    if (existing == m_progress.cend()) {
        m_admissionOrder.insert(progress.id, m_nextOrder++);
    }
    if (rfm::core::isTerminal(progress.state) && !wasTerminal) {
        m_terminalOrder.insert(progress.id, m_nextOrder++);
    }
    m_progress.insert(progress.id, progress);
    return true;
}

bool OperationPanel::removeTerminalOperation(quint64 id)
{
    const auto progress = m_progress.constFind(id);
    const auto row = m_rows.constFind(id);
    if (progress == m_progress.cend() || row == m_rows.cend() ||
        !rfm::core::isTerminal(progress->state)) {
        return false;
    }
    const int removedRow = row.value();
    m_table->removeRow(removedRow);
    m_progress.remove(id);
    m_rows.remove(id);
    m_admissionOrder.remove(id);
    m_terminalOrder.remove(id);
    rebuildRowMappings();
    updateHistoryActions();
    return true;
}

void OperationPanel::clearTerminalOperations()
{
    for (int row = m_table->rowCount() - 1; row >= 0; --row) {
        const quint64 id = m_table->item(row, KindColumn)->data(Qt::UserRole).toULongLong();
        if (rfm::core::isTerminal(m_progress.value(id).state)) {
            m_table->removeRow(row);
            m_progress.remove(id);
            m_rows.remove(id);
            m_admissionOrder.remove(id);
            m_terminalOrder.remove(id);
        }
    }
    rebuildRowMappings();
    updateHistoryActions();
}

void OperationPanel::updateHistoryActions()
{
    const quint64 selectedId = selectedOperationId();
    m_removeButton->setEnabled(selectedId != 0 && m_progress.contains(selectedId) &&
                               rfm::core::isTerminal(m_progress.value(selectedId).state));
    m_clearButton->setEnabled(std::ranges::any_of(
        m_progress, [](const auto& operation) { return rfm::core::isTerminal(operation.state); }));
}

} // namespace rfm::app
