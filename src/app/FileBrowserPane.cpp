#include "remotefilemanager/app/FileBrowserPane.hpp"

#include "remotefilemanager/core/LocalFileSystem.hpp"
#include "remotefilemanager/core/RemotePath.hpp"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFocusEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QLocale>
#include <QMimeDatabase>
#include <QMimeData>
#include <QMimeType>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <functional>
#include <limits>
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
                            base.blueF() * baseRatio + accent.blueF() * accentRatio, base.alphaF());
}

QString safePropertyValue(const QString& value)
{
    QString safe = value;
    for (qsizetype index = 0; index < safe.size(); ++index) {
        const QChar character = safe.at(index);
        const QChar::Category category = character.category();
        if (category == QChar::Other_Control || category == QChar::Separator_Line ||
            category == QChar::Separator_Paragraph) {
            safe[index] = QChar{' '};
        }
    }
    return safe.simplified();
}

std::optional<QString> mimeDescriptionForFileName(const QString& fileName)
{
    if (QFileInfo(fileName).suffix().isEmpty()) {
        return std::nullopt;
    }
    const QMimeType mimeType =
        QMimeDatabase{}.mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
    if (!mimeType.isValid() || mimeType.isDefault() ||
        mimeType.name() == QStringLiteral("application/octet-stream")) {
        return std::nullopt;
    }
    const QString description = mimeType.comment().trimmed();
    if (description.isEmpty() || description == mimeType.name()) {
        return std::nullopt;
    }
    return description;
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
    m_pathEdit->setPlaceholderText(tr("Local folder or sftp://user@server/path"));
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
    dragTable->startDragHandler = [this](Qt::DropActions supportedActions) {
        startInternalDrag(supportedActions);
    };
}

QString FileBrowserPane::currentPath() const { return m_currentLocation.path; }

rfm::core::BrowserLocation FileBrowserPane::currentLocation() const { return m_currentLocation; }

rfm::core::FileSource FileBrowserPane::source() const { return m_currentLocation.source; }

bool FileBrowserPane::hasLocation() const { return m_currentLocation.isValid(); }

QList<rfm::core::RemoteSelection> FileBrowserPane::selectedEntries() const
{
    QList<rfm::core::RemoteSelection> selection;
    const QModelIndexList rows = m_fileTable->selectionModel()->selectedRows(0);
    for (const QModelIndex& index : rows) {
        const QTableWidgetItem* const item = m_fileTable->item(index.row(), 0);
        if (item != nullptr) {
            const QString path =
                m_currentLocation.source == rfm::core::FileSource::Local
                    ? QDir(m_currentLocation.path).filePath(item->text())
                    : rfm::core::RemotePath::join(m_currentLocation.path, item->text());
            selection.push_back({path, item->data(Qt::UserRole).toBool()});
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
    if (m_currentLocation.source != rfm::core::FileSource::Ssh) {
        return {};
    }
    return rfm::core::encodeInternalTransfer(
        {m_applicationInstanceId, m_connectionIdentity, m_paneId, selectedEntries()});
}

std::optional<FileEntryProperties> FileBrowserPane::contextEntryProperties() const
{
    if (m_contextMenuRow < 0 || m_contextMenuRow >= m_fileTable->rowCount()) {
        return std::nullopt;
    }
    const QTableWidgetItem* const nameItem = m_fileTable->item(m_contextMenuRow, 0);
    if (nameItem == nullptr || !nameItem->data(Qt::UserRole + 2).isValid()) {
        return std::nullopt;
    }
    const rfm::core::RemoteEntry entry =
        nameItem->data(Qt::UserRole + 2).value<rfm::core::RemoteEntry>();
    const QString path = m_currentLocation.source == rfm::core::FileSource::Local
                             ? QDir(m_currentLocation.path).filePath(entry.name)
                             : rfm::core::RemotePath::join(m_currentLocation.path, entry.name);
    const QString type = entry.symbolicLink
                             ? tr("Symbolic link")
                             : entry.directory
                                   ? tr("Folder")
                                   : mimeDescriptionForFileName(entry.name).value_or(tr("File"));
    QStringList lines{tr("Name: %1").arg(safePropertyValue(entry.name)),
                      tr("Type: %1").arg(type), tr("Path: %1").arg(safePropertyValue(path))};
    if (!entry.directory) {
        const QString extension = safePropertyValue(QFileInfo(entry.name).suffix());
        if (!extension.isEmpty()) {
            lines.push_back(tr("Extension: %1").arg(extension));
        }
    }
    if (!entry.directory || entry.size > 0) {
        const qint64 displaySize =
            entry.size > static_cast<quint64>(std::numeric_limits<qint64>::max())
                ? std::numeric_limits<qint64>::max()
                : static_cast<qint64>(entry.size);
        lines.push_back(tr("Size: %1").arg(QLocale{}.formattedDataSize(displaySize)));
    }
    if (entry.modifiedAt.isValid()) {
        lines.push_back(
            tr("Modified: %1").arg(QLocale{}.toString(entry.modifiedAt, QLocale::ShortFormat)));
    }
    return FileEntryProperties{safePropertyValue(entry.name), lines.join(QChar{'\n'})};
}

bool FileBrowserPane::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == this || watched == m_pathEdit || watched == m_fileTable ||
        watched == m_fileTable->viewport()) {
        bool activatesPane = event->type() == QEvent::MouseButtonPress;
        if (event->type() == QEvent::FocusIn) {
            const Qt::FocusReason reason = static_cast<QFocusEvent*>(event)->reason();
            activatesPane = reason == Qt::MouseFocusReason || reason == Qt::TabFocusReason ||
                            reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
        }
        if (activatesPane) {
            emit activated();
        }
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
            const bool valid =
                payload.has_value() && validateDrop(*payload, destination).accepted();
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
            const bool valid =
                payload.has_value() && validateDrop(*payload, destination).accepted();
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
    const QString machineId = m_currentLocation.source == rfm::core::FileSource::Ssh &&
                                      !m_currentLocation.machineId.isEmpty()
                                  ? m_currentLocation.machineId
                                  : QStringLiteral("ssh");
    showDirectory({rfm::core::FileSource::Ssh, machineId, path}, displayPath, entries, navigation);
}

void FileBrowserPane::showDirectory(const rfm::core::BrowserLocation& location,
                                    const QString& displayPath,
                                    const QList<rfm::core::RemoteEntry>& entries,
                                    PaneNavigation navigation)
{
    rfm::core::BrowserLocation normalizedLocation = location;
    normalizedLocation.path = normalizedPath(location);
    const QString& normalizedPath = normalizedLocation.path;
    if (normalizedPath.isEmpty()) {
        return;
    }
    QStringList namesToSelect;
    int previousScrollPosition = -1;
    const bool sameDirectory = normalizedLocation == m_currentLocation;
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

    const rfm::core::BrowserLocation previousLocation = m_currentLocation;
    const bool pathChanged = previousLocation != normalizedLocation;
    if (navigation == PaneNavigation::Initial) {
        m_backHistory.clear();
        m_forwardHistory.clear();
    } else if (pathChanged && navigation == PaneNavigation::Normal) {
        if (previousLocation.isValid() &&
            (m_backHistory.isEmpty() || m_backHistory.constLast() != previousLocation)) {
            m_backHistory.push_back(previousLocation);
        }
        m_forwardHistory.clear();
    } else if (pathChanged && navigation == PaneNavigation::Back && !m_backHistory.isEmpty()) {
        m_backHistory.removeLast();
        if (previousLocation.isValid()) {
            m_forwardHistory.push_back(previousLocation);
        }
    } else if (pathChanged && navigation == PaneNavigation::Forward &&
               !m_forwardHistory.isEmpty()) {
        m_forwardHistory.removeLast();
        if (previousLocation.isValid()) {
            m_backHistory.push_back(previousLocation);
        }
    } else if (pathChanged && navigation == PaneNavigation::SafetyFallback) {
        m_backHistory.removeIf([&normalizedLocation](const rfm::core::BrowserLocation& location) {
            return location == normalizedLocation;
        });
        m_forwardHistory.clear();
    }
    m_currentLocation = normalizedLocation;
    m_contextMenuRow = -1;
    m_fileTable->setDragEnabled(m_currentLocation.source == rfm::core::FileSource::Ssh);
    m_fileTable->setAcceptDrops(m_currentLocation.source == rfm::core::FileSource::Ssh);
    m_fileTable->viewport()->setAcceptDrops(m_currentLocation.source == rfm::core::FileSource::Ssh);
    m_fileTable->setRowCount(static_cast<int>(entries.size()));
    QFileIconProvider icons;
    for (qsizetype row = 0; row < entries.size(); ++row) {
        const auto& entry = entries.at(row);
        auto* const nameItem = new QTableWidgetItem(
            icons.icon(entry.directory ? QFileIconProvider::Folder : QFileIconProvider::File),
            entry.name);
        nameItem->setData(Qt::UserRole, entry.directory);
        nameItem->setData(Qt::UserRole + 1, entry.symbolicLink);
        nameItem->setData(Qt::UserRole + 2, QVariant::fromValue(entry));
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
    m_currentLocation = {};
    m_contextMenuRow = -1;
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

void FileBrowserPane::removeHistoryForSource(rfm::core::FileSource source)
{
    m_backHistory.removeIf(
        [source](const rfm::core::BrowserLocation& location) { return location.source == source; });
    m_forwardHistory.removeIf(
        [source](const rfm::core::BrowserLocation& location) { return location.source == source; });
    emit historyChanged();
}

void FileBrowserPane::removeLocalHistoryUnderPath(const QString& rootPath)
{
    removeHistoryUnderPath(rfm::core::FileSource::Local,
                           QString::fromLatin1(rfm::core::LocalMachineId), rootPath);
}

void FileBrowserPane::removeHistoryUnderPath(rfm::core::FileSource source, const QString& machineId,
                                             const QString& rootPath)
{
    const auto belongsToUnmountedVolume = [source, &machineId,
                                           &rootPath](const rfm::core::BrowserLocation& location) {
        if (location.source != source || location.machineId != machineId) {
            return false;
        }
        return source == rfm::core::FileSource::Local
                   ? rfm::core::localPathIsAtOrBelow(location.path, rootPath)
                   : rfm::core::RemotePath::isAtOrBelow(location.path, rootPath);
    };
    m_backHistory.removeIf(belongsToUnmountedVolume);
    m_forwardHistory.removeIf(belongsToUnmountedVolume);
    emit historyChanged();
}

void FileBrowserPane::setPendingSelectionNames(QStringList names)
{
    m_pendingSelectionNames = std::move(names);
}

void FileBrowserPane::setInteractionEnabled(bool enabled)
{
    if (!enabled && m_fileTable->isEnabled()) {
        QWidget* const focusWidget = QApplication::focusWidget();
        if (focusWidget == m_fileTable || m_fileTable->isAncestorOf(focusWidget)) {
            m_pathEdit->setFocus(Qt::OtherFocusReason);
            m_restoreTableFocus = true;
        }
    }
    m_fileTable->setEnabled(enabled);
    if (enabled && m_restoreTableFocus) {
        if (QApplication::focusWidget() == m_pathEdit) {
            m_fileTable->setFocus(Qt::OtherFocusReason);
        }
        m_restoreTableFocus = false;
    }
}

void FileBrowserPane::setActiveAppearance(bool active)
{
    const QPalette normalPalette =
        parentWidget() != nullptr ? parentWidget()->palette() : palette();
    QPalette panePalette = normalPalette;
    QPalette pathPalette = normalPalette;
    if (active) {
        const QColor highlight = normalPalette.color(QPalette::Highlight);
        panePalette.setColor(QPalette::Window,
                             blendedColor(normalPalette.color(QPalette::Window), highlight, 0.07F));
        pathPalette.setColor(QPalette::Base,
                             blendedColor(normalPalette.color(QPalette::Base), highlight, 0.11F));
    }
    setAutoFillBackground(true);
    setPalette(panePalette);
    m_pathEdit->setPalette(pathPalette);
    m_fileTable->setPalette(normalPalette);
}

void FileBrowserPane::setTransferContext(QString applicationInstanceId,
                                         rfm::core::RemoteConnectionIdentity connection,
                                         quint64 paneId)
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
    rfm::core::BrowserLocation location = m_currentLocation;
    location.path = path;
    const QString pathValue = normalizedPath(location);
    if (pathValue.isEmpty() || pathValue == m_currentLocation.path) {
        return;
    }
    location.path = pathValue;
    requestLocation(location, PaneNavigation::Normal);
}

void FileBrowserPane::requestParentDirectory()
{
    if (!m_currentLocation.isValid()) {
        return;
    }
    if (m_currentLocation.source == rfm::core::FileSource::Local) {
        const QDir directory(m_currentLocation.path);
        if (directory.isRoot()) {
            return;
        }
        navigateTo(QFileInfo(m_currentLocation.path).dir().absolutePath());
    } else {
        if (m_currentLocation.path == QStringLiteral(".")) {
            return;
        }
        QString parent = rfm::core::RemotePath::parent(m_currentLocation.path);
        if (parent.isEmpty()) {
            parent = QStringLiteral(".");
        }
        navigateTo(parent);
    }
}

void FileBrowserPane::requestBack()
{
    if (!m_backHistory.isEmpty()) {
        requestLocation(m_backHistory.constLast(), PaneNavigation::Back);
    }
}

void FileBrowserPane::requestForward()
{
    if (!m_forwardHistory.isEmpty()) {
        requestLocation(m_forwardHistory.constLast(), PaneNavigation::Forward);
    }
}

void FileBrowserPane::requestRefresh()
{
    if (m_currentLocation.isValid()) {
        requestLocation(m_currentLocation, PaneNavigation::Refresh);
    }
}

void FileBrowserPane::openEntry(int row)
{
    const QTableWidgetItem* const item = m_fileTable->item(row, 0);
    if (item == nullptr ||
        (!item->data(Qt::UserRole).toBool() && !item->data(Qt::UserRole + 1).toBool())) {
        return;
    }
    navigateTo(m_currentLocation.source == rfm::core::FileSource::Local
                   ? QDir(m_currentLocation.path).filePath(item->text())
                   : rfm::core::RemotePath::join(m_currentLocation.path, item->text()));
}

void FileBrowserPane::prepareContextMenu(const QPoint& position)
{
    if (QTableWidgetItem* const item = m_fileTable->itemAt(position);
        item != nullptr && !item->isSelected()) {
        m_contextMenuRow = item->row();
        m_fileTable->clearSelection();
        m_fileTable->selectRow(item->row());
    } else if (item != nullptr) {
        m_contextMenuRow = item->row();
    } else if (item == nullptr) {
        m_contextMenuRow = -1;
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
        return m_currentLocation.path;
    }
    const QTableWidgetItem* const name = m_fileTable->item(hit->row(), 0);
    if (name == nullptr || !name->data(Qt::UserRole).toBool()) {
        return m_currentLocation.path;
    }
    if (folderRow != nullptr) {
        *folderRow = hit->row();
    }
    return rfm::core::RemotePath::join(m_currentLocation.path, name->text());
}

QString FileBrowserPane::normalizedPath(const rfm::core::BrowserLocation& location) const
{
    if (location.source == rfm::core::FileSource::Local) {
        return location.path.isEmpty()
                   ? QString{}
                   : QDir::cleanPath(QFileInfo(location.path).absoluteFilePath());
    }
    return rfm::core::RemotePath::normalize(location.path);
}

void FileBrowserPane::requestLocation(const rfm::core::BrowserLocation& location,
                                      PaneNavigation navigation)
{
    emit navigationRequested(location.path, navigation);
    emit locationNavigationRequested(location, navigation);
}

rfm::core::InternalTransferValidation
FileBrowserPane::validateDrop(const rfm::core::InternalTransferPayload& payload,
                              const QString& destination) const
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
    m_fileTable->setStyleSheet(
        QStringLiteral("QTableWidget[dropState=\"valid\"] { border: 2px solid palette(highlight); }"
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
        const QString path =
            name == nullptr ? QString{}
                            : rfm::core::RemotePath::join(m_currentLocation.path, name->text());
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
