#include "remotefilemanager/app/FileBrowserPane.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QColor>
#include <QBrush>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileIconProvider>
#include <QEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QLocale>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <limits>
#include <functional>
#include <utility>

namespace rfm::app
{

namespace
{

QColor blendedColor(const QColor& base, const QColor& accent, float accentRatio)
{
    const float baseRatio = 1.0F - accentRatio;
    return QColor::fromRgbF(base.redF() * baseRatio + accent.redF() * accentRatio,
                            base.greenF() * baseRatio + accent.greenF() * accentRatio,
                            base.blueF() * baseRatio + accent.blueF() * accentRatio,
                            base.alphaF());
}

class InternalDragTable final : public QTableWidget
{
  public:
    using QTableWidget::QTableWidget;
    std::function<void(Qt::DropActions)> startDragHandler;

  protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        if (startDragHandler) {
            startDragHandler(supportedActions);
        }
    }
};

} // namespace

FileBrowserPane::FileBrowserPane(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("fileBrowserPane"));
    auto* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setObjectName(QStringLiteral("remotePathEdit"));
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(tr("sftp://user@server/path"));
    m_pathEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(m_pathEdit);

    auto* const dragTable = new InternalDragTable(this);
    m_fileTable = dragTable;
    m_fileTable->setObjectName(QStringLiteral("remoteFileTable"));
    m_fileTable->setColumnCount(3);
    m_fileTable->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified")});
    m_fileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fileTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fileTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_fileTable->setShowGrid(false);
    m_fileTable->setDragEnabled(true);
    m_fileTable->setAcceptDrops(true);
    m_fileTable->viewport()->setAcceptDrops(true);
    m_fileTable->setDragDropMode(QAbstractItemView::DragDrop);
    m_fileTable->setDropIndicatorShown(false);
    m_fileTable->verticalHeader()->hide();
    m_fileTable->horizontalHeader()->setStretchLastSection(true);
    m_fileTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_fileTable);

    installEventFilter(this);
    m_pathEdit->installEventFilter(this);
    m_fileTable->installEventFilter(this);
    m_fileTable->viewport()->installEventFilter(this);

    connect(m_fileTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int /* column */) { openEntry(row); });
    connect(m_fileTable, &QWidget::customContextMenuRequested, this,
            &FileBrowserPane::prepareContextMenu);
    connect(m_fileTable->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &FileBrowserPane::selectionChanged);
    dragTable->startDragHandler =
        [this](Qt::DropActions supportedActions) { startInternalDrag(supportedActions); };
}

QString FileBrowserPane::currentPath() const { return m_currentPath; }

QList<rfm::core::RemoteSelection> FileBrowserPane::selectedEntries() const
{
    QList<rfm::core::RemoteSelection> selection;
    const QModelIndexList rows = m_fileTable->selectionModel()->selectedRows(0);
    for (const QModelIndex& index : rows) {
        const QTableWidgetItem* const item = m_fileTable->item(index.row(), 0);
        if (item != nullptr) {
            selection.push_back({rfm::core::RemotePath::join(m_currentPath, item->text()),
                                 item->data(Qt::UserRole).toBool()});
        }
    }
    return selection;
}

QLineEdit* FileBrowserPane::pathEdit() const { return m_pathEdit; }

QTableWidget* FileBrowserPane::fileTable() const { return m_fileTable; }

bool FileBrowserPane::canGoBack() const { return !m_backHistory.isEmpty(); }

bool FileBrowserPane::canGoForward() const { return !m_forwardHistory.isEmpty(); }

QByteArray FileBrowserPane::createInternalDragData() const
{
    return rfm::core::encodeInternalTransfer(
        {m_applicationInstanceId, m_connectionIdentity, m_paneId, selectedEntries()});
}

bool FileBrowserPane::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == this || watched == m_pathEdit || watched == m_fileTable ||
         watched == m_fileTable->viewport()) &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn)) {
        emit activated();
    }
    if (watched == m_fileTable->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* const dragEvent = static_cast<QDropEvent*>(event);
            const QMimeData* const mimeData = dragEvent->mimeData();
            const auto payload =
                mimeData != nullptr && mimeData->hasFormat(rfm::core::InternalTransferMimeType)
                    ? rfm::core::decodeInternalTransfer(
                          mimeData->data(rfm::core::InternalTransferMimeType))
                    : std::nullopt;
            const QPoint position = dragEvent->position().toPoint();
            int folderRow = -1;
            const QString destination = dropDestinationAt(position, &folderRow);
            const bool valid = payload.has_value() && validateDrop(*payload, destination).accepted();
            updateDropAppearance(payload.has_value(), valid, valid ? folderRow : -1);
            if (valid) {
                dragEvent->setDropAction(Qt::CopyAction);
                dragEvent->accept();
            } else if (payload.has_value()) {
                dragEvent->setDropAction(Qt::IgnoreAction);
                dragEvent->accept();
            } else {
                dragEvent->ignore();
            }
            return true;
        }
        if (event->type() == QEvent::DragLeave) {
            updateDropAppearance(false, false);
            event->accept();
            return true;
        }
        if (event->type() == QEvent::Drop) {
            auto* const dropEvent = static_cast<QDropEvent*>(event);
            const QMimeData* const mimeData = dropEvent->mimeData();
            const auto payload =
                mimeData != nullptr && mimeData->hasFormat(rfm::core::InternalTransferMimeType)
                    ? rfm::core::decodeInternalTransfer(
                          mimeData->data(rfm::core::InternalTransferMimeType))
                    : std::nullopt;
            const QString destination = dropDestinationAt(dropEvent->position().toPoint());
            const bool valid = payload.has_value() && validateDrop(*payload, destination).accepted();
            updateDropAppearance(false, false);
            if (!valid) {
                dropEvent->ignore();
                return true;
            }
            dropEvent->setDropAction(Qt::CopyAction);
            dropEvent->accept();
            emit activated();
            emit internalDropRequested(*payload, destination);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FileBrowserPane::showDirectory(const QString& path, const QString& displayPath,
                                    const QList<rfm::core::RemoteEntry>& entries,
                                    PaneNavigation navigation)
{
    const QString normalizedPath = rfm::core::RemotePath::normalize(path);
    if (normalizedPath.isEmpty()) {
        return;
    }
    QStringList namesToSelect;
    int previousScrollPosition = -1;
    const bool sameDirectory = normalizedPath ==
                               rfm::core::RemotePath::normalize(m_currentPath);
    if (sameDirectory) {
        for (const QModelIndex& index : m_fileTable->selectionModel()->selectedRows(0)) {
            if (const QTableWidgetItem* const item = m_fileTable->item(index.row(), 0);
                item != nullptr) {
                namesToSelect.push_back(item->text());
            }
        }
        previousScrollPosition = m_fileTable->verticalScrollBar()->value();
    }
    for (const QString& name : std::as_const(m_pendingSelectionNames)) {
        if (!namesToSelect.contains(name)) {
            namesToSelect.push_back(name);
        }
    }
    m_pendingSelectionNames.clear();

    const QString previousPath = m_currentPath;
    const bool pathChanged = rfm::core::RemotePath::normalize(previousPath) !=
                             normalizedPath;
    if (navigation == PaneNavigation::Initial) {
        m_backHistory.clear();
        m_forwardHistory.clear();
    } else if (pathChanged && navigation == PaneNavigation::Normal) {
        if (!previousPath.isEmpty() &&
            (m_backHistory.isEmpty() || m_backHistory.constLast() != previousPath)) {
            m_backHistory.push_back(previousPath);
        }
        m_forwardHistory.clear();
    } else if (pathChanged && navigation == PaneNavigation::Back && !m_backHistory.isEmpty()) {
        m_backHistory.removeLast();
        if (!previousPath.isEmpty()) {
            m_forwardHistory.push_back(previousPath);
        }
    } else if (pathChanged && navigation == PaneNavigation::Forward &&
               !m_forwardHistory.isEmpty()) {
        m_forwardHistory.removeLast();
        if (!previousPath.isEmpty()) {
            m_backHistory.push_back(previousPath);
        }
    }
    m_currentPath = normalizedPath;
    m_fileTable->setRowCount(static_cast<int>(entries.size()));
    QFileIconProvider icons;
    for (qsizetype row = 0; row < entries.size(); ++row) {
        const auto& entry = entries.at(row);
        auto* const nameItem = new QTableWidgetItem(
            icons.icon(entry.directory ? QFileIconProvider::Folder : QFileIconProvider::File),
            entry.name);
        nameItem->setData(Qt::UserRole, entry.directory);
        nameItem->setData(Qt::UserRole + 1, entry.symbolicLink);
        m_fileTable->setItem(static_cast<int>(row), 0, nameItem);
        const qint64 displaySize =
            entry.size > static_cast<quint64>(std::numeric_limits<qint64>::max())
                ? std::numeric_limits<qint64>::max()
                : static_cast<qint64>(entry.size);
        auto* const sizeItem = new QTableWidgetItem(
            entry.directory ? QString{} : QLocale{}.formattedDataSize(displaySize));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_fileTable->setItem(static_cast<int>(row), 1, sizeItem);
        m_fileTable->setItem(
            static_cast<int>(row), 2,
            new QTableWidgetItem(QLocale{}.toString(entry.modifiedAt, QLocale::ShortFormat)));
    }
    for (int row = 0; row < m_fileTable->rowCount(); ++row) {
        const QTableWidgetItem* const item = m_fileTable->item(row, 0);
        if (item != nullptr && namesToSelect.contains(item->text())) {
            m_fileTable->selectionModel()->select(m_fileTable->model()->index(row, 0),
                                                  QItemSelectionModel::Select |
                                                      QItemSelectionModel::Rows);
        }
    }
    if (previousScrollPosition >= 0) {
        m_fileTable->verticalScrollBar()->setValue(previousScrollPosition);
    }
    m_pathEdit->setText(displayPath);
    updateCutAppearance();
    emit historyChanged();
}

void FileBrowserPane::clear()
{
    m_currentPath.clear();
    m_pendingSelectionNames.clear();
    m_backHistory.clear();
    m_forwardHistory.clear();
    m_pathEdit->clear();
    m_fileTable->clearContents();
    m_fileTable->setRowCount(0);
    clearTransferContext();
    setCutPaths({});
    updateDropAppearance(false, false);
    setInteractionEnabled(false);
    emit historyChanged();
}

void FileBrowserPane::setPendingSelectionNames(QStringList names)
{
    m_pendingSelectionNames = std::move(names);
}

void FileBrowserPane::setInteractionEnabled(bool enabled)
{
    m_fileTable->setEnabled(enabled);
}

void FileBrowserPane::setActiveAppearance(bool active)
{
    const QPalette normalPalette =
        parentWidget() != nullptr ? parentWidget()->palette() : palette();
    QPalette panePalette = normalPalette;
    QPalette pathPalette = normalPalette;
    if (active) {
        const QColor highlight = normalPalette.color(QPalette::Highlight);
        panePalette.setColor(
            QPalette::Window,
            blendedColor(normalPalette.color(QPalette::Window), highlight, 0.07F));
        pathPalette.setColor(QPalette::Base,
                             blendedColor(normalPalette.color(QPalette::Base), highlight, 0.11F));
    }
    setAutoFillBackground(true);
    setPalette(panePalette);
    m_pathEdit->setPalette(pathPalette);
    m_fileTable->setPalette(normalPalette);
}

void FileBrowserPane::setTransferContext(
    QString applicationInstanceId, rfm::core::RemoteConnectionIdentity connection, quint64 paneId)
{
    m_applicationInstanceId = std::move(applicationInstanceId);
    m_connectionIdentity = std::move(connection);
    m_paneId = paneId;
}

void FileBrowserPane::clearTransferContext()
{
    m_applicationInstanceId.clear();
    m_connectionIdentity = {};
    m_paneId = 0;
    updateDropAppearance(false, false);
}

void FileBrowserPane::setCutPaths(QSet<QString> paths)
{
    m_cutPaths.clear();
    for (const QString& path : std::as_const(paths)) {
        const QString normalized = rfm::core::RemotePath::normalize(path);
        if (!normalized.isEmpty()) {
            m_cutPaths.insert(normalized);
        }
    }
    updateCutAppearance();
}

void FileBrowserPane::focusLocation()
{
    m_pathEdit->setFocus(Qt::ShortcutFocusReason);
    m_pathEdit->selectAll();
}

void FileBrowserPane::navigateTo(const QString& path)
{
    const QString normalizedPath = rfm::core::RemotePath::normalize(path);
    if (normalizedPath.isEmpty() ||
        normalizedPath == rfm::core::RemotePath::normalize(m_currentPath)) {
        return;
    }
    emit navigationRequested(normalizedPath, PaneNavigation::Normal);
}

void FileBrowserPane::requestParentDirectory()
{
    if (m_currentPath.isEmpty() || m_currentPath == QStringLiteral(".")) {
        return;
    }
    QString parent = rfm::core::RemotePath::parent(m_currentPath);
    if (parent.isEmpty()) {
        parent = QStringLiteral(".");
    }
    navigateTo(parent);
}

void FileBrowserPane::requestBack()
{
    if (!m_backHistory.isEmpty()) {
        emit navigationRequested(m_backHistory.constLast(), PaneNavigation::Back);
    }
}

void FileBrowserPane::requestForward()
{
    if (!m_forwardHistory.isEmpty()) {
        emit navigationRequested(m_forwardHistory.constLast(), PaneNavigation::Forward);
    }
}

void FileBrowserPane::requestRefresh()
{
    if (!m_currentPath.isEmpty()) {
        emit navigationRequested(m_currentPath, PaneNavigation::Refresh);
    }
}

void FileBrowserPane::openEntry(int row)
{
    const QTableWidgetItem* const item = m_fileTable->item(row, 0);
    if (item == nullptr || (!item->data(Qt::UserRole).toBool() &&
                            !item->data(Qt::UserRole + 1).toBool())) {
        return;
    }
    navigateTo(rfm::core::RemotePath::join(m_currentPath, item->text()));
}

void FileBrowserPane::prepareContextMenu(const QPoint& position)
{
    if (QTableWidgetItem* const item = m_fileTable->itemAt(position);
        item != nullptr && !item->isSelected()) {
        m_fileTable->clearSelection();
        m_fileTable->selectRow(item->row());
    } else if (item == nullptr) {
        m_fileTable->clearSelection();
    }
    emit contextMenuRequested(m_fileTable->viewport()->mapToGlobal(position));
}

void FileBrowserPane::startInternalDrag(Qt::DropActions supportedActions)
{
    const QByteArray data = createInternalDragData();
    if (data.isEmpty()) {
        return;
    }
    auto* const mimeData = new QMimeData;
    mimeData->setData(rfm::core::InternalTransferMimeType, data);
    QDrag drag(m_fileTable);
    drag.setMimeData(mimeData);

    QPixmap pixmap(180, 36);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(palette().color(QPalette::Highlight));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(pixmap.rect().adjusted(0, 0, -1, -1), 5, 5);
    painter.setPen(palette().color(QPalette::HighlightedText));
    painter.drawText(pixmap.rect(), Qt::AlignCenter,
                     tr("%1 remote item(s)").arg(selectedEntries().size()));
    drag.setPixmap(pixmap);
    drag.setHotSpot(QPoint(12, 12));
    drag.exec(supportedActions & (Qt::CopyAction | Qt::MoveAction), Qt::CopyAction);
}

QString FileBrowserPane::dropDestinationAt(const QPoint& position, int* folderRow) const
{
    if (folderRow != nullptr) {
        *folderRow = -1;
    }
    const QTableWidgetItem* const hit = m_fileTable->itemAt(position);
    if (hit == nullptr) {
        return m_currentPath;
    }
    const QTableWidgetItem* const name = m_fileTable->item(hit->row(), 0);
    if (name == nullptr || !name->data(Qt::UserRole).toBool()) {
        return m_currentPath;
    }
    if (folderRow != nullptr) {
        *folderRow = hit->row();
    }
    return rfm::core::RemotePath::join(m_currentPath, name->text());
}

rfm::core::InternalTransferValidation FileBrowserPane::validateDrop(
    const rfm::core::InternalTransferPayload& payload, const QString& destination) const
{
    return rfm::core::validateInternalTransfer(payload, m_applicationInstanceId,
                                               m_connectionIdentity, destination);
}

void FileBrowserPane::updateDropAppearance(bool active, bool valid, int folderRow)
{
    if (m_dropHighlightRow >= 0 && m_dropHighlightRow < m_fileTable->rowCount()) {
        for (int column = 0; column < m_fileTable->columnCount(); ++column) {
            if (QTableWidgetItem* const item = m_fileTable->item(m_dropHighlightRow, column)) {
                item->setBackground(QBrush{});
                item->setForeground(QBrush{});
            }
        }
    }
    updateCutAppearance();
    m_dropHighlightRow = active && valid ? folderRow : -1;
    if (m_dropHighlightRow >= 0) {
        for (int column = 0; column < m_fileTable->columnCount(); ++column) {
            if (QTableWidgetItem* const item = m_fileTable->item(m_dropHighlightRow, column)) {
                item->setBackground(palette().color(QPalette::Highlight));
                item->setForeground(palette().color(QPalette::HighlightedText));
            }
        }
    }
    m_fileTable->setProperty("dropState", active ? (valid ? "valid" : "invalid") : "none");
    m_fileTable->setStyleSheet(QStringLiteral(
        "QTableWidget[dropState=\"valid\"] { border: 2px solid palette(highlight); }"
        "QTableWidget[dropState=\"invalid\"] { border: 2px dashed palette(mid); }"));
    if (active && !valid) {
        m_fileTable->viewport()->setCursor(Qt::ForbiddenCursor);
    } else {
        m_fileTable->viewport()->unsetCursor();
    }
    m_fileTable->style()->unpolish(m_fileTable);
    m_fileTable->style()->polish(m_fileTable);
}

void FileBrowserPane::updateCutAppearance()
{
    const QColor cutColor = palette().color(QPalette::Disabled, QPalette::Text);
    for (int row = 0; row < m_fileTable->rowCount(); ++row) {
        const QTableWidgetItem* const name = m_fileTable->item(row, 0);
        const QString path = name == nullptr
                                 ? QString{}
                                 : rfm::core::RemotePath::join(m_currentPath, name->text());
        const bool cut = m_cutPaths.contains(rfm::core::RemotePath::normalize(path));
        for (int column = 0; column < m_fileTable->columnCount(); ++column) {
            if (QTableWidgetItem* const item = m_fileTable->item(row, column)) {
                item->setForeground(cut ? QBrush(cutColor) : QBrush{});
                QFont font = item->font();
                font.setItalic(cut);
                item->setFont(font);
            }
        }
    }
}

} // namespace rfm::app
