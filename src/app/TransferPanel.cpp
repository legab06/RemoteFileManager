#include "remotefilemanager/app/TransferPanel.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace rfm::app
{
namespace
{

enum Column {
    DirectionColumn,
    SourceColumn,
    DestinationColumn,
    StateColumn,
    ProgressColumn,
    SpeedColumn,
    ActionsColumn,
    ErrorColumn,
    ColumnCount,
};

bool isTerminal(rfm::core::TransferState state)
{
    return state == rfm::core::TransferState::Completed ||
           state == rfm::core::TransferState::Cancelled ||
           state == rfm::core::TransferState::Failed;
}

QString displayName(const rfm::core::TransferProgress& progress)
{
    if (progress.direction == rfm::core::TransferDirection::Upload) {
        const QString name = QFileInfo(progress.source).fileName();
        return name.isEmpty() ? progress.source : name;
    }
    const QString name = rfm::core::RemotePath::fileName(progress.source);
    return name.isEmpty() ? progress.source : name;
}

} // namespace

TransferPanel::TransferPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("transferPanel"));
    auto* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("transferTable"));
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({tr("Direction"), tr("Source"), tr("Destination"),
                                        tr("Status"), tr("Progress"), tr("Speed"), tr("Actions"),
                                        tr("Error")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    QHeaderView* const header = m_table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(DirectionColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(SourceColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(DestinationColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(StateColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ProgressColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(SpeedColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ActionsColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ErrorColumn, QHeaderView::Stretch);
    layout->addWidget(m_table);
}

QString TransferPanel::formatBytes(quint64 bytes)
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

QString TransferPanel::formatSpeed(quint64 bytesPerSecond)
{
    return bytesPerSecond == 0 ? QStringLiteral("—")
                               : QStringLiteral("%1/s").arg(formatBytes(bytesPerSecond));
}

QString TransferPanel::stateText(rfm::core::TransferState state)
{
    switch (state) {
    case rfm::core::TransferState::Queued:
        return tr("Queued");
    case rfm::core::TransferState::Preparing:
        return tr("Preparing");
    case rfm::core::TransferState::Transferring:
        return tr("Transferring");
    case rfm::core::TransferState::Paused:
        return tr("Paused");
    case rfm::core::TransferState::Finalizing:
        return tr("Finalizing");
    case rfm::core::TransferState::Cancelling:
        return tr("Cancelling");
    case rfm::core::TransferState::Completed:
        return tr("Completed");
    case rfm::core::TransferState::Cancelled:
        return tr("Cancelled");
    case rfm::core::TransferState::Failed:
        return tr("Failed");
    }
    return {};
}

int TransferPanel::ensureRow(const rfm::core::TransferProgress& progress)
{
    const auto existing = m_rows.constFind(progress.id);
    if (existing != m_rows.cend()) {
        return existing.value();
    }

    const int row = m_table->rowCount();
    m_table->insertRow(row);
    for (int column = 0; column < ColumnCount; ++column) {
        m_table->setItem(row, column, new QTableWidgetItem);
    }
    m_table->item(row, DirectionColumn)->setData(Qt::UserRole, progress.id);

    auto* const progressBar = new QProgressBar(m_table);
    progressBar->setObjectName(QStringLiteral("transferProgressBar"));
    progressBar->setTextVisible(true);
    progressBar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    progressBar->setMinimumWidth(
        progressBar->fontMetrics().horizontalAdvance(QStringLiteral("999.9 GiB / 999.9 GiB")) +
        progressBar->style()->pixelMetric(QStyle::PM_ProgressBarChunkWidth) * 2);
    m_table->setCellWidget(row, ProgressColumn, progressBar);

    auto* const actions = new QWidget(m_table);
    auto* const actionsLayout = new QHBoxLayout(actions);
    actionsLayout->setContentsMargins(2, 0, 2, 0);
    actionsLayout->setSpacing(4);
    actionsLayout->setSizeConstraint(QLayout::SetMinimumSize);
    actions->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    auto* const pauseResume = new QPushButton(actions);
    pauseResume->setObjectName(QStringLiteral("pauseResumeButton"));
    pauseResume->setProperty("transferId", QVariant::fromValue(progress.id));
    auto* const cancel = new QPushButton(tr("Cancel"), actions);
    cancel->setObjectName(QStringLiteral("cancelTransferButton"));
    cancel->setProperty("transferId", QVariant::fromValue(progress.id));
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
        if (current->state == rfm::core::TransferState::Paused) {
            emit resumeRequested(id);
        } else if (current->state == rfm::core::TransferState::Transferring) {
            emit pauseRequested(id);
        }
    });
    connect(cancel, &QPushButton::clicked, this,
            [this, id = progress.id] { emit cancelRequested(id); });
    m_table->setCellWidget(row, ActionsColumn, actions);

    m_rows.insert(progress.id, row);
    return row;
}

void TransferPanel::updateRow(int row, const rfm::core::TransferProgress& progress)
{
    QTableWidgetItem* const directionItem = m_table->item(row, DirectionColumn);
    directionItem->setText(progress.direction == rfm::core::TransferDirection::Upload
                               ? tr("↑ Upload")
                               : tr("↓ Download"));
    directionItem->setData(Qt::UserRole, progress.id);
    QTableWidgetItem* const sourceItem = m_table->item(row, SourceColumn);
    sourceItem->setText(displayName(progress));
    sourceItem->setToolTip(progress.source);
    QTableWidgetItem* const destinationItem = m_table->item(row, DestinationColumn);
    destinationItem->setText(progress.destination);
    destinationItem->setToolTip(progress.destination);
    m_table->item(row, StateColumn)->setText(stateText(progress.state));
    m_table->item(row, SpeedColumn)->setText(formatSpeed(progress.bytesPerSecond));
    QTableWidgetItem* const errorItem = m_table->item(row, ErrorColumn);
    errorItem->setText(progress.error);
    errorItem->setToolTip(progress.error);

    auto* const progressBar = qobject_cast<QProgressBar*>(m_table->cellWidget(row, ProgressColumn));
    if (progress.totalBytes == 0 && !isTerminal(progress.state)) {
        progressBar->setRange(0, 0);
        progressBar->setFormat(tr("Calculating size"));
    } else {
        progressBar->setRange(0, 1000);
        const double ratio =
            progress.totalBytes == 0
                ? (progress.state == rfm::core::TransferState::Completed ? 1.0 : 0.0)
                : std::clamp(static_cast<double>(progress.transferredBytes) /
                                 static_cast<double>(progress.totalBytes),
                             0.0, 1.0);
        progressBar->setValue(static_cast<int>(std::round(ratio * 1000.0)));
        progressBar->setFormat(progress.totalBytes == 0
                                   ? formatBytes(progress.transferredBytes)
                                   : tr("%1 / %2").arg(formatBytes(progress.transferredBytes),
                                                       formatBytes(progress.totalBytes)));
    }

    QWidget* const actions = m_table->cellWidget(row, ActionsColumn);
    auto* const pauseResume = actions->findChild<QPushButton*>(QStringLiteral("pauseResumeButton"));
    auto* const cancel = actions->findChild<QPushButton*>(QStringLiteral("cancelTransferButton"));
    const bool canPause = progress.state == rfm::core::TransferState::Transferring;
    const bool canResume = progress.state == rfm::core::TransferState::Paused;
    pauseResume->setVisible(canPause || canResume);
    pauseResume->setEnabled(canPause || canResume);
    pauseResume->setText(canResume ? tr("Resume") : tr("Pause"));
    cancel->setVisible(!isTerminal(progress.state));
    cancel->setEnabled(progress.state != rfm::core::TransferState::Cancelling);
}

void TransferPanel::updateTransfer(rfm::core::TransferProgress progress)
{
    if (progress.id == 0) {
        return;
    }
    m_progress.insert(progress.id, progress);
    updateRow(ensureRow(progress), progress);
}

} // namespace rfm::app
