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
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMimeType>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QRubberBand>
#include <QScrollBar>
#include <QResizeEvent>
#include <QSettings>
#include <QSizePolicy>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <limits>
#include <numeric>
#include <utility>

namespace rfm::app
{

namespace
{

constexpr int SortValueRole = Qt::UserRole + 3;
constexpr int SortKindRole = Qt::UserRole + 4;
constexpr int SortNameRole = Qt::UserRole + 5;
constexpr int SortDirectoryRole = Qt::UserRole + 6;
constexpr int SortValueKnownRole = Qt::UserRole + 7;
constexpr int SortNaturalIndexRole = Qt::UserRole + 8;
constexpr auto tableHeaderStateKey = "ui/fileBrowserPane/headerState";
constexpr auto tableSortColumnKey = "ui/fileBrowserPane/sortColumn";
constexpr auto tableSortOrderKey = "ui/fileBrowserPane/sortOrder";
constexpr auto tableLayoutModeKey = "ui/fileBrowserPane/layoutMode";
constexpr int defaultNameColumnWidth = 280;
constexpr int defaultSizeColumnWidth = 110;
constexpr int defaultTypeColumnWidth = 180;
constexpr int defaultModifiedColumnWidth = 170;

QSet<FileBrowserPane*>& tableHeaderPanes()
{
    static QSet<FileBrowserPane*> panes;
    return panes;
}

enum class SortKind { Text, Size, Modified };

class SortableFileItem final : public QTableWidgetItem
{
  public:
    SortableFileItem(QString text, SortKind kind, QVariant value, const QString& name,
                     bool directory, bool valueKnown = true)
        : QTableWidgetItem(std::move(text))
    {
        setData(SortValueRole, std::move(value));
        setData(SortKindRole, static_cast<int>(kind));
        setData(SortNameRole, name);
        setData(SortDirectoryRole, directory);
        setData(SortValueKnownRole, valueKnown);
    }

    [[nodiscard]] bool operator<(const QTableWidgetItem& other) const override
    {
        const Qt::SortOrder order = tableWidget() != nullptr
                                        ? tableWidget()->horizontalHeader()->sortIndicatorOrder()
                                        : Qt::AscendingOrder;
        const bool directory = data(SortDirectoryRole).toBool();
        const bool otherDirectory = other.data(SortDirectoryRole).toBool();
        if (directory != otherDirectory) {
            return order == Qt::AscendingOrder ? directory : !directory;
        }

        const SortKind kind = static_cast<SortKind>(data(SortKindRole).toInt());
        const QVariant value = data(SortValueRole);
        const QVariant otherValue = other.data(SortValueRole);
        if (kind == SortKind::Modified) {
            const QDateTime modified = value.toDateTime();
            const QDateTime otherModified = otherValue.toDateTime();
            if (modified.isValid() != otherModified.isValid()) {
                const bool missing = !modified.isValid();
                return order == Qt::AscendingOrder ? !missing : missing;
            }
            if (modified.isValid() && modified != otherModified) {
                return modified < otherModified;
            }
        } else if (kind == SortKind::Size) {
            const bool valueKnown = data(SortValueKnownRole).toBool();
            const bool otherValueKnown = other.data(SortValueKnownRole).toBool();
            if (valueKnown != otherValueKnown) {
                const bool missing = !valueKnown;
                return order == Qt::AscendingOrder ? !missing : missing;
            }
            const quint64 size = value.toULongLong();
            const quint64 otherSize = otherValue.toULongLong();
            if (size != otherSize) {
                return size < otherSize;
            }
        } else {
            const QString textValue = value.toString().toCaseFolded();
            const QString otherTextValue = otherValue.toString().toCaseFolded();
            const int comparison = QString::localeAwareCompare(textValue, otherTextValue);
            if (comparison != 0) {
                return comparison < 0;
            }
        }

        return QString::localeAwareCompare(data(SortNameRole).toString().toCaseFolded(),
                                           other.data(SortNameRole).toString().toCaseFolded()) < 0;
    }
};

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

std::optional<QMimeType> mimeTypeForFileName(const QString& fileName)
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
    return mimeType;
}

struct FilePresentation {
    QString type;
    QIcon icon;
};

FilePresentation presentationForEntry(const rfm::core::RemoteEntry& entry)
{
    QFileIconProvider fallbackIcons;
    if (entry.symbolicLink) {
        const QStyle::StandardPixmap pixmap =
            entry.directory ? QStyle::SP_DirLinkIcon : QStyle::SP_FileLinkIcon;
        QIcon icon = QApplication::style()->standardIcon(pixmap);
        if (icon.isNull()) {
            icon = fallbackIcons.icon(entry.directory ? QFileIconProvider::Folder
                                                      : QFileIconProvider::File);
        }
        return {QObject::tr("Symbolic link"), std::move(icon)};
    }
    if (entry.directory) {
        return {QObject::tr("Folder"), fallbackIcons.icon(QFileIconProvider::Folder)};
    }

    const std::optional<QMimeType> mimeType = mimeTypeForFileName(entry.name);
    if (!mimeType.has_value()) {
        return {QObject::tr("File"), fallbackIcons.icon(QFileIconProvider::File)};
    }

    QString description = mimeType->comment().trimmed();
    if (description.isEmpty() || description == mimeType->name()) {
        description = QObject::tr("File");
    }
    QIcon icon = QIcon::fromTheme(mimeType->iconName());
    if (icon.isNull()) {
        icon = QIcon::fromTheme(mimeType->genericIconName());
    }
    if (icon.isNull()) {
        // Keep remote detection name-only while still letting each Qt platform
        // provide its native extension icon when no themed MIME icon exists.
        icon = fallbackIcons.icon(QFileInfo(entry.name));
    }
    if (icon.isNull()) {
        icon = fallbackIcons.icon(QFileIconProvider::File);
    }
    return {std::move(description), std::move(icon)};
}

QString modifiedDisplayText(const QDateTime& modifiedAt)
{
    return modifiedAt.isValid() ? QLocale{}.toString(modifiedAt, QLocale::ShortFormat)
                                : QObject::tr("—");
}

QString directoryItemCountText(quint64 count)
{
    return count == 1 ? QObject::tr("1 item")
                      : QObject::tr("%1 items").arg(QLocale{}.toString(count));
}

rfm::core::InternalTransferAction requestedTransferAction(Qt::KeyboardModifiers modifiers)
{
    return modifiers.testFlag(Qt::ControlModifier) || !modifiers.testFlag(Qt::ShiftModifier)
               ? rfm::core::InternalTransferAction::Copy
               : rfm::core::InternalTransferAction::Move;
}

bool transferActionWasExplicitlyRequested(Qt::KeyboardModifiers modifiers)
{
    return modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::ShiftModifier);
}

Qt::DropAction qtDropAction(rfm::core::InternalTransferAction action)
{
    return action == rfm::core::InternalTransferAction::Copy ? Qt::CopyAction : Qt::MoveAction;
}

class InternalDragTable final : public QTableWidget
{
  public:
    explicit InternalDragTable(QWidget* parent = nullptr) : QTableWidget(parent)
    {
        m_autoScrollTimer.setInterval(50);
        connect(&m_autoScrollTimer, &QTimer::timeout, this, [this]() {
            QScrollBar* const scrollBar = verticalScrollBar();
            const int previousValue = scrollBar->value();
            scrollBar->setValue(previousValue + m_autoScrollDirection * scrollBar->singleStep());
            if (scrollBar->value() == previousValue) {
                m_autoScrollTimer.stop();
                return;
            }
            updateRubberBandSelection(m_lastViewportPosition);
        });
    }

    std::function<void(Qt::DropActions)> startDragHandler;

  protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        if (startDragHandler) {
            startDragHandler(supportedActions);
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QTableWidget::mousePressEvent(event);
            return;
        }

        const QModelIndex pressedIndex = indexAt(event->position().toPoint());
        if (pressedIndex.isValid()) {
            finishRubberBand();
            m_itemInteractionActive = true;
            m_itemDragAttempted = false;
            m_itemPressViewportPosition = event->position().toPoint();
            m_itemInitialSelectedRows = selectedRowSet();
            m_preserveItemSelectionForDrag =
                selectionModel()->isSelected(pressedIndex) && m_itemInitialSelectedRows.size() > 1;
            QTableWidget::mousePressEvent(event);
            if (m_preserveItemSelectionForDrag) {
                m_itemClickSelectedRows = selectedRowSet();
                const bool control = event->modifiers().testFlag(Qt::ControlModifier);
                const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
                if (control && !shift) {
                    m_itemClickSelectedRows = m_itemInitialSelectedRows;
                    m_itemClickSelectedRows.remove(pressedIndex.row());
                } else if (!control && !shift) {
                    m_itemClickSelectedRows = {pressedIndex.row()};
                }
                setSelectedRows(m_itemInitialSelectedRows);
            }
            return;
        }

        finishItemInteraction();
        setFocus(Qt::MouseFocusReason);
        m_rubberBandPending = true;
        m_rubberBandActive = false;
        m_pressViewportPosition = event->position().toPoint();
        m_originContentPosition = contentPosition(m_pressViewportPosition);
        m_lastViewportPosition = m_pressViewportPosition;
        m_controlSelection = event->modifiers().testFlag(Qt::ControlModifier);
        m_initialSelectedRows.clear();
        for (const QModelIndex& index : selectionModel()->selectedRows(0)) {
            m_initialSelectedRows.insert(index.row());
        }
        if (!m_controlSelection) {
            clearSelection();
        }
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_itemInteractionActive) {
            if (!event->buttons().testFlag(Qt::LeftButton)) {
                finishItemInteraction();
                event->accept();
                return;
            }
            const QPoint position = event->position().toPoint();
            if (!m_itemDragAttempted &&
                (position - m_itemPressViewportPosition).manhattanLength() >=
                    QApplication::startDragDistance()) {
                m_itemDragAttempted = true;
                setState(QAbstractItemView::DraggingState);
                if (dragEnabled()) {
                    startDrag(model()->supportedDragActions());
                }
                if (m_itemInteractionActive) {
                    setState(QAbstractItemView::NoState);
                }
            }
            event->accept();
            return;
        }

        if (!m_rubberBandPending) {
            QTableWidget::mouseMoveEvent(event);
            return;
        }
        if (!event->buttons().testFlag(Qt::LeftButton)) {
            finishRubberBand();
            QTableWidget::mouseMoveEvent(event);
            return;
        }

        const QPoint position = event->position().toPoint();
        if (!m_rubberBandActive && (position - m_pressViewportPosition).manhattanLength() <
                                       QApplication::startDragDistance()) {
            event->accept();
            return;
        }

        if (!m_rubberBandActive) {
            m_rubberBandActive = true;
            if (m_rubberBand == nullptr) {
                m_rubberBand = new QRubberBand(QRubberBand::Rectangle, viewport());
                m_rubberBand->setObjectName(QStringLiteral("fileSelectionRubberBand"));
            }
            m_rubberBand->show();
        }
        updateRubberBandSelection(position);
        updateAutoScroll(position);
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_itemInteractionActive && event->button() == Qt::LeftButton) {
            if (!m_itemDragAttempted) {
                QTableWidget::mouseReleaseEvent(event);
                if (m_preserveItemSelectionForDrag) {
                    setSelectedRows(m_itemClickSelectedRows);
                }
            } else {
                event->accept();
            }
            finishItemInteraction();
            return;
        }
        if (m_rubberBandPending && event->button() == Qt::LeftButton) {
            finishRubberBand();
            event->accept();
            return;
        }
        QTableWidget::mouseReleaseEvent(event);
    }

    void focusOutEvent(QFocusEvent* event) override
    {
        finishRubberBand();
        finishItemInteraction();
        QTableWidget::focusOutEvent(event);
    }

  private:
    [[nodiscard]] QSet<int> selectedRowSet() const
    {
        QSet<int> rows;
        for (const QModelIndex& index : selectionModel()->selectedRows(0)) {
            rows.insert(index.row());
        }
        return rows;
    }

    void setSelectedRows(const QSet<int>& rows)
    {
        QItemSelection selection;
        const int columns = model()->columnCount();
        for (int row = 0; row < model()->rowCount(); ++row) {
            if (rows.contains(row)) {
                selection.select(model()->index(row, 0), model()->index(row, columns - 1));
            }
        }
        selectionModel()->select(selection,
                                 QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }

    [[nodiscard]] QPoint contentPosition(const QPoint& viewportPosition) const
    {
        const QRect bounds = viewport()->rect();
        const QPoint boundedPosition{qBound(bounds.left(), viewportPosition.x(), bounds.right()),
                                     qBound(bounds.top(), viewportPosition.y(), bounds.bottom())};
        return boundedPosition + QPoint(horizontalOffset(), verticalOffset());
    }

    void updateRubberBandSelection(const QPoint& viewportPosition)
    {
        m_lastViewportPosition = viewportPosition;
        const QPoint currentContentPosition = contentPosition(viewportPosition);
        const QRect contentRectangle =
            QRect(m_originContentPosition, currentContentPosition).normalized();
        const QRect viewportRectangle =
            contentRectangle.translated(-horizontalOffset(), -verticalOffset());
        m_rubberBand->setGeometry(viewportRectangle);

        QSet<int> intersectingRows;
        const int columns = model()->columnCount();
        for (int row = 0; row < model()->rowCount(); ++row) {
            QRect rowRectangle;
            for (int column = 0; column < columns; ++column) {
                rowRectangle = rowRectangle.united(visualRect(model()->index(row, column)));
            }
            rowRectangle.translate(horizontalOffset(), verticalOffset());
            if (!rowRectangle.isEmpty() && rowRectangle.intersects(contentRectangle)) {
                intersectingRows.insert(row);
            }
        }

        QSet<int> selectedRows = m_controlSelection ? m_initialSelectedRows : QSet<int>{};
        for (const int row : std::as_const(intersectingRows)) {
            if (m_controlSelection && selectedRows.contains(row)) {
                selectedRows.remove(row);
            } else {
                selectedRows.insert(row);
            }
        }

        setSelectedRows(selectedRows);
    }

    void updateAutoScroll(const QPoint& viewportPosition)
    {
        constexpr int AutoScrollMargin = 24;
        if (viewportPosition.y() < AutoScrollMargin) {
            m_autoScrollDirection = -1;
        } else if (viewportPosition.y() >= viewport()->height() - AutoScrollMargin) {
            m_autoScrollDirection = 1;
        } else {
            m_autoScrollDirection = 0;
        }
        if (m_autoScrollDirection == 0) {
            m_autoScrollTimer.stop();
        } else if (!m_autoScrollTimer.isActive()) {
            m_autoScrollTimer.start();
        }
    }

    void finishRubberBand()
    {
        m_autoScrollTimer.stop();
        m_autoScrollDirection = 0;
        m_rubberBandPending = false;
        m_rubberBandActive = false;
        if (m_rubberBand != nullptr) {
            m_rubberBand->hide();
        }
    }

    void finishItemInteraction()
    {
        m_itemInteractionActive = false;
        m_itemDragAttempted = false;
        m_preserveItemSelectionForDrag = false;
        m_itemInitialSelectedRows.clear();
        m_itemClickSelectedRows.clear();
        setState(QAbstractItemView::NoState);
    }

    QRubberBand* m_rubberBand{nullptr};
    QTimer m_autoScrollTimer;
    QPoint m_pressViewportPosition;
    QPoint m_originContentPosition;
    QPoint m_lastViewportPosition;
    QPoint m_itemPressViewportPosition;
    QSet<int> m_initialSelectedRows;
    QSet<int> m_itemInitialSelectedRows;
    QSet<int> m_itemClickSelectedRows;
    int m_autoScrollDirection{0};
    bool m_rubberBandPending{false};
    bool m_rubberBandActive{false};
    bool m_controlSelection{false};
    bool m_itemInteractionActive{false};
    bool m_itemDragAttempted{false};
    bool m_preserveItemSelectionForDrag{false};
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
    m_fileTable->setColumnCount(4);
    m_fileTable->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Type"), tr("Modified")});
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
    QHeaderView* const header = m_fileTable->horizontalHeader();
    header->setSectionsClickable(true);
    header->setSectionsMovable(true);
    header->setFirstSectionMovable(true);
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setMinimumSectionSize(48);
    header->resizeSection(0, defaultNameColumnWidth);
    header->resizeSection(1, defaultSizeColumnWidth);
    header->resizeSection(2, defaultTypeColumnWidth);
    header->resizeSection(3, defaultModifiedColumnWidth);
    m_fileTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_fileTable->setSortingEnabled(true);
    header->setSortIndicator(-1, Qt::AscendingOrder);
    restoreTableHeaderState();
    layout->addWidget(m_fileTable);

    m_tableHeaderSaveTimer = new QTimer(this);
    m_tableHeaderSaveTimer->setSingleShot(true);
    m_tableHeaderSaveTimer->setInterval(200);
    connect(m_tableHeaderSaveTimer, &QTimer::timeout, this, &FileBrowserPane::saveTableHeaderState);
    connect(header, &QHeaderView::sectionResized, this,
            [this](int, int, int) {
                if (!m_restoringTableHeaderState && !m_applyingLayoutState &&
                    !m_applyingAdaptiveLayout) {
                    markManualLayout();
                }
            });
    connect(header, &QHeaderView::sectionMoved, this,
            [this](int, int, int) {
                if (!m_restoringTableHeaderState && !m_applyingLayoutState) {
                    markManualLayout();
                }
            });
    connect(header, &QHeaderView::sortIndicatorChanged, this,
            &FileBrowserPane::handleSortIndicatorChanged);
    connect(header, &QHeaderView::sectionPressed, this,
            [this](int) { m_headerClickInProgress = true; });
    connect(header, &QHeaderView::sectionClicked, this,
            &FileBrowserPane::handleHeaderSectionClicked);
    tableHeaderPanes().insert(this);

    installEventFilter(this);
    m_pathEdit->installEventFilter(this);
    m_fileTable->installEventFilter(this);
    m_fileTable->viewport()->installEventFilter(this);
    header->installEventFilter(this);
    header->viewport()->installEventFilter(this);

    connect(m_fileTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int /* column */) { openEntry(row); });
    connect(m_fileTable, &QWidget::customContextMenuRequested, this,
            &FileBrowserPane::prepareContextMenu);
    connect(m_fileTable->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &FileBrowserPane::selectionChanged);
    dragTable->startDragHandler = [this](Qt::DropActions supportedActions) {
        startInternalDrag(supportedActions);
    };
    QTimer::singleShot(0, this, &FileBrowserPane::applyAdaptiveLayout);
}

FileBrowserPane::~FileBrowserPane()
{
    if (m_tableHeaderStateDirty) {
        saveTableHeaderState();
    }
    tableHeaderPanes().remove(this);
}

void FileBrowserPane::restoreTableHeaderState()
{
    QSettings settings;
    const QByteArray state = settings.value(QString::fromLatin1(tableHeaderStateKey)).toByteArray();
    if (state.isEmpty()) {
        m_adaptiveLayout = true;
        resetTableHeaderState();
        m_sortColumn = -1;
        return;
    }

    const QString layoutMode = settings.value(QString::fromLatin1(tableLayoutModeKey)).toString();
    // Profiles written before layoutMode existed have customized dimensions; preserve them.
    m_adaptiveLayout = layoutMode.isEmpty() ? false : layoutMode != QLatin1String("manual");

    const bool hasExplicitSortState = settings.contains(QString::fromLatin1(tableSortColumnKey));
    const int storedSortColumn =
        settings.value(QString::fromLatin1(tableSortColumnKey), -1).toInt();
    const int storedSortOrder =
        settings
            .value(QString::fromLatin1(tableSortOrderKey), static_cast<int>(Qt::DescendingOrder))
            .toInt();
    const QHeaderView* const header = m_fileTable->horizontalHeader();
    const int sortColumn = hasExplicitSortState ? storedSortColumn : header->sortIndicatorSection();
    const Qt::SortOrder sortOrder = hasExplicitSortState
                                        ? static_cast<Qt::SortOrder>(storedSortOrder)
                                        : header->sortIndicatorOrder();
    const bool validSortState =
        (sortColumn == -1 || (sortColumn >= 0 && sortColumn < header->count())) &&
        (sortColumn == -1 || sortOrder == Qt::AscendingOrder || sortOrder == Qt::DescendingOrder);
    if (!validSortState) {
        m_adaptiveLayout = true;
        resetTableHeaderState();
        m_sortColumn = -1;
        return;
    }
    applyTableHeaderState(state, sortColumn, sortOrder, m_adaptiveLayout);
}

void FileBrowserPane::applyTableHeaderState(const QByteArray& state, int sortColumn,
                                            Qt::SortOrder sortOrder, bool adaptiveLayout)
{
    if (state.isEmpty()) {
        resetTableHeaderState();
        return;
    }
    m_restoringTableHeaderState = true;
    m_adaptiveLayout = adaptiveLayout;
    const bool restored = m_fileTable->horizontalHeader()->restoreState(state);
    m_sortColumn = sortColumn;
    m_sortOrder = sortOrder;
    m_restoringTableHeaderState = false;
    if (!restored) {
        m_adaptiveLayout = true;
        resetTableHeaderState();
        m_sortColumn = -1;
        return;
    }
    applySortState();
    applyAdaptiveLayout();
}

void FileBrowserPane::resetTableHeaderState()
{
    m_applyingLayoutState = true;
    QHeaderView* const header = m_fileTable->horizontalHeader();
    for (int logicalIndex = 0; logicalIndex < header->count(); ++logicalIndex) {
        header->setSectionHidden(logicalIndex, false);
        const int currentVisualIndex = header->visualIndex(logicalIndex);
        if (currentVisualIndex != logicalIndex) {
            header->moveSection(currentVisualIndex, logicalIndex);
        }
    }
    const QList<int> defaultWidths{defaultNameColumnWidth, defaultSizeColumnWidth,
                                   defaultTypeColumnWidth, defaultModifiedColumnWidth};
    for (int logicalIndex = 0;
         logicalIndex < header->count() && logicalIndex < defaultWidths.size(); ++logicalIndex) {
        header->resizeSection(logicalIndex, defaultWidths.at(logicalIndex));
    }
    m_applyingSortState = true;
    header->setSortIndicator(-1, Qt::AscendingOrder);
    header->setSortIndicatorShown(false);
    m_applyingSortState = false;
    m_applyingLayoutState = false;
}

void FileBrowserPane::applyAdaptiveLayout()
{
    if (!m_adaptiveLayout || m_fileTable == nullptr) {
        return;
    }
    QHeaderView* const header = m_fileTable->horizontalHeader();
    const int viewportWidth = m_fileTable->viewport()->width();
    if (viewportWidth <= 0 || header->count() == 0) {
        return;
    }
    const QList<int> minimumWidths{72, 100, 130};
    const QList<int> adaptiveBaseWidths{defaultSizeColumnWidth, defaultTypeColumnWidth,
                                        defaultModifiedColumnWidth};
    QList<int> otherWidths;
    otherWidths.reserve(header->count() - 1);
    for (int column = 1; column < header->count(); ++column) {
        otherWidths.push_back(
            qMax(minimumWidths.at(column - 1), adaptiveBaseWidths.at(column - 1)));
    }
    int otherColumnsWidth = std::accumulate(otherWidths.cbegin(), otherWidths.cend(), 0);
    int contentWidth = defaultNameColumnWidth;
    const QFontMetrics metrics(m_fileTable->font());
    for (int row = 0; row < m_fileTable->rowCount(); ++row) {
        if (const QTableWidgetItem* const item = m_fileTable->item(row, 0); item != nullptr) {
            contentWidth = qMax(contentWidth, metrics.horizontalAdvance(item->text()) + 32);
        }
    }
    constexpr int minimumNameWidth = 160;
    const int contentLimit = qBound(520, contentWidth, 900);
    const int maximumNameWidth = qMax(520, qMin(viewportWidth * 2 / 3, contentLimit));
    const int availableNameWidth = viewportWidth - otherColumnsWidth;
    // Name follows the remaining viewport continuously.  The only clamp is the
    // absolute minimum/maximum; this keeps scrollbar appearance from feeding a
    // second, discontinuous width decision.
    const int suggestedNameWidth =
        qBound(minimumNameWidth, availableNameWidth, maximumNameWidth);

    if (availableNameWidth < minimumNameWidth) {
        int remainingReduction = otherColumnsWidth - qMax(0, viewportWidth - minimumNameWidth);
        while (remainingReduction > 0) {
            bool reduced = false;
            for (int index = 0; index < otherWidths.size() && remainingReduction > 0; ++index) {
                if (otherWidths[index] > minimumWidths.at(index)) {
                    --otherWidths[index];
                    --remainingReduction;
                    reduced = true;
                }
            }
            if (!reduced) {
                break;
            }
        }
    }

    m_applyingAdaptiveLayout = true;
    if (header->sectionSize(0) != suggestedNameWidth) {
        header->resizeSection(0, suggestedNameWidth);
    }
    for (int index = 0; index < otherWidths.size(); ++index) {
        const int logicalIndex = index + 1;
        if (header->sectionSize(logicalIndex) != otherWidths.at(index)) {
            header->resizeSection(logicalIndex, otherWidths.at(index));
        }
    }
    m_applyingAdaptiveLayout = false;
}

void FileBrowserPane::markManualLayout()
{
    m_adaptiveLayout = false;
    if (m_headerDragActive) {
        m_tableHeaderStateDirty = true;
        return;
    }
    scheduleTableHeaderStateSave();
}

void FileBrowserPane::applyDefaultFileView()
{
    m_tableHeaderSaveTimer->stop();
    m_tableHeaderStateDirty = false;
    m_adaptiveLayout = true;
    m_sortColumn = -1;
    m_sortOrder = Qt::DescendingOrder;
    resetTableHeaderState();
    applySortState();
    applyAdaptiveLayout();
}

void FileBrowserPane::resetFileView()
{
    QSettings settings;
    settings.remove(QString::fromLatin1(tableHeaderStateKey));
    settings.remove(QString::fromLatin1(tableSortColumnKey));
    settings.remove(QString::fromLatin1(tableSortOrderKey));
    settings.remove(QString::fromLatin1(tableLayoutModeKey));
    settings.sync();
    for (FileBrowserPane* const pane : tableHeaderPanes()) {
        pane->applyDefaultFileView();
    }
}

void FileBrowserPane::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_adaptiveLayout) {
        applyAdaptiveLayout();
    }
}

void FileBrowserPane::handleHeaderSectionClicked(int logicalIndex)
{
    m_headerClickInProgress = false;
    if (m_restoringTableHeaderState || logicalIndex < 0 ||
        logicalIndex >= m_fileTable->columnCount()) {
        return;
    }
    if (logicalIndex != m_sortColumn) {
        m_sortColumn = logicalIndex;
        m_sortOrder = Qt::DescendingOrder;
    } else if (m_sortOrder == Qt::DescendingOrder) {
        m_sortOrder = Qt::AscendingOrder;
    } else {
        m_sortColumn = -1;
    }
    applySortState();
    scheduleTableHeaderStateSave();
}

void FileBrowserPane::handleSortIndicatorChanged(int logicalIndex, Qt::SortOrder order)
{
    if (m_restoringTableHeaderState || m_applyingSortState || m_headerClickInProgress) {
        return;
    }
    m_sortColumn = logicalIndex;
    m_sortOrder = order;
    if (m_sortColumn < 0 || m_sortColumn >= m_fileTable->columnCount()) {
        m_sortColumn = -1;
    } else {
        m_fileTable->horizontalHeader()->setSortIndicatorShown(true);
    }
    if (m_sortColumn < 0) {
        restoreNaturalOrder();
    }
    scheduleTableHeaderStateSave();
}

void FileBrowserPane::applySortState()
{
    m_applyingSortState = true;
    QHeaderView* const header = m_fileTable->horizontalHeader();
    if (m_sortColumn < 0 || m_sortColumn >= m_fileTable->columnCount()) {
        header->setSortIndicator(-1, Qt::AscendingOrder);
        header->setSortIndicatorShown(false);
        m_applyingSortState = false;
        restoreNaturalOrder();
        header->setSortIndicatorShown(false);
        return;
    }
    header->setSortIndicatorShown(true);
    header->setSortIndicator(m_sortColumn, m_sortOrder);
    m_fileTable->sortItems(m_sortColumn, m_sortOrder);
    m_applyingSortState = false;
}

void FileBrowserPane::restoreNaturalOrder()
{
    if (m_fileTable->rowCount() < 2) {
        return;
    }
    QStringList selectedNames;
    for (const QModelIndex& index : m_fileTable->selectionModel()->selectedRows(0)) {
        if (const QTableWidgetItem* const item = m_fileTable->item(index.row(), 0);
            item != nullptr) {
            selectedNames.push_back(item->text());
        }
    }
    QList<QList<QTableWidgetItem*>> rows;
    rows.reserve(m_fileTable->rowCount());
    for (int row = 0; row < m_fileTable->rowCount(); ++row) {
        QList<QTableWidgetItem*> items;
        items.reserve(m_fileTable->columnCount());
        for (int column = 0; column < m_fileTable->columnCount(); ++column) {
            items.push_back(m_fileTable->takeItem(row, column));
        }
        rows.push_back(std::move(items));
    }
    std::ranges::sort(rows, [](const auto& first, const auto& second) {
        const auto naturalIndex = [](const auto& row) {
            return row.constFirst()->data(SortNaturalIndexRole).toInt();
        };
        return naturalIndex(first) < naturalIndex(second);
    });
    m_fileTable->setSortingEnabled(false);
    for (int row = 0; row < rows.size(); ++row) {
        for (int column = 0; column < rows.at(row).size(); ++column) {
            m_fileTable->setItem(row, column, rows.at(row).at(column));
        }
    }
    m_fileTable->clearSelection();
    for (int row = 0; row < m_fileTable->rowCount(); ++row) {
        if (const QTableWidgetItem* const item = m_fileTable->item(row, 0);
            item != nullptr && selectedNames.contains(item->text())) {
            m_fileTable->selectionModel()->select(m_fileTable->model()->index(row, 0),
                                                  QItemSelectionModel::Select |
                                                      QItemSelectionModel::Rows);
        }
    }
    m_fileTable->setSortingEnabled(true);
}

void FileBrowserPane::scheduleTableHeaderStateSave()
{
    if (m_restoringTableHeaderState || m_applyingLayoutState || m_applyingAdaptiveLayout) {
        return;
    }
    m_tableHeaderStateDirty = true;
    if (m_tableHeaderSaveTimer != nullptr) {
        m_tableHeaderSaveTimer->start();
    }
}

void FileBrowserPane::saveTableHeaderState()
{
    if (m_fileTable == nullptr || !m_tableHeaderStateDirty) {
        return;
    }
    if (m_tableHeaderSaveTimer != nullptr) {
        m_tableHeaderSaveTimer->stop();
    }
    const QByteArray state = m_fileTable->horizontalHeader()->saveState();
    QSettings settings;
    const QString headerKey = QString::fromLatin1(tableHeaderStateKey);
    const QString sortColumnKey = QString::fromLatin1(tableSortColumnKey);
    const QString sortOrderKey = QString::fromLatin1(tableSortOrderKey);
    const QString layoutModeKey = QString::fromLatin1(tableLayoutModeKey);
    const int sortOrder = static_cast<int>(m_sortOrder);
    if (settings.value(headerKey).toByteArray() != state ||
        settings.value(sortColumnKey, -1).toInt() != m_sortColumn ||
        settings.value(sortOrderKey, static_cast<int>(Qt::DescendingOrder)).toInt() != sortOrder ||
        settings.value(layoutModeKey).toString() !=
            (m_adaptiveLayout ? QStringLiteral("adaptive") : QStringLiteral("manual"))) {
        settings.setValue(headerKey, state);
        settings.setValue(sortColumnKey, m_sortColumn);
        settings.setValue(sortOrderKey, sortOrder);
        settings.setValue(layoutModeKey,
                          m_adaptiveLayout ? QStringLiteral("adaptive") : QStringLiteral("manual"));
        settings.sync();
    }
    m_tableHeaderStateDirty = false;
    for (FileBrowserPane* const pane : tableHeaderPanes()) {
        if (pane != this) {
            pane->applyTableHeaderState(state, m_sortColumn, m_sortOrder, m_adaptiveLayout);
        }
    }
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
    if (m_currentLocation.source == rfm::core::FileSource::None) {
        return {};
    }
    return rfm::core::encodeInternalTransfer({m_currentLocation.source, m_currentLocation.machineId,
                                              m_applicationInstanceId,
                                              m_currentLocation.source == rfm::core::FileSource::Ssh
                                                  ? m_connectionIdentity
                                                  : rfm::core::RemoteConnectionIdentity{},
                                              m_paneId, selectedEntries()});
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
    const FilePresentation presentation = presentationForEntry(entry);
    QStringList lines{tr("Name: %1").arg(safePropertyValue(entry.name)),
                      tr("Type: %1").arg(presentation.type),
                      tr("Path: %1").arg(safePropertyValue(path))};
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
    lines.push_back(tr("Modified: %1").arg(modifiedDisplayText(entry.modifiedAt)));
    return FileEntryProperties{safePropertyValue(entry.name), lines.join(QChar{'\n'})};
}

bool FileBrowserPane::eventFilter(QObject* watched, QEvent* event)
{
    QHeaderView* const header = m_fileTable->horizontalHeader();
    const bool headerEvent = watched == header || watched == header->viewport();
    if (watched == this || watched == m_pathEdit || watched == m_fileTable ||
        watched == m_fileTable->viewport() || headerEvent) {
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
    if (headerEvent) {
        if (event->type() == QEvent::MouseButtonPress) {
            const auto* const mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && header->sectionsMovable()) {
                const int mouseX = mouseEvent->position().toPoint().x();
                m_headerDragLogicalIndex = header->logicalIndexAt(mouseX);
                if (m_headerDragLogicalIndex >= 0) {
                    const int sectionStart =
                        header->sectionViewportPosition(m_headerDragLogicalIndex);
                    const int sectionEnd =
                        sectionStart + header->sectionSize(m_headerDragLogicalIndex);
                    constexpr int resizeHandleMargin = 4;
                    if (mouseX - sectionStart <= resizeHandleMargin ||
                        sectionEnd - mouseX <= resizeHandleMargin) {
                        m_headerDragLogicalIndex = -1;
                    }
                }
                m_headerDragStartPosition = mouseEvent->position().toPoint();
                m_headerDragActive = false;
            }
        } else if (event->type() == QEvent::MouseMove &&
                   m_headerDragLogicalIndex >= 0) {
            const auto* const mouseEvent = static_cast<QMouseEvent*>(event);
            if (!m_headerDragActive &&
                (mouseEvent->position().toPoint() - m_headerDragStartPosition)
                        .manhattanLength() >= QApplication::startDragDistance()) {
                m_headerDragActive = true;
            }
            if (m_headerDragActive) {
                    const int currentVisualIndex =
                        header->visualIndex(m_headerDragLogicalIndex);
                    const int mouseX = mouseEvent->position().toPoint().x();
                    const int currentStart =
                        header->sectionViewportPosition(m_headerDragLogicalIndex);
                    const int currentEnd = currentStart +
                                           header->sectionSize(m_headerDragLogicalIndex);
                    int targetVisualIndex = currentVisualIndex;
                    if (mouseX > currentEnd) {
                        for (int visualIndex = currentVisualIndex + 1;
                             visualIndex < header->count(); ++visualIndex) {
                            const int logicalIndex = header->logicalIndex(visualIndex);
                            const int center = header->sectionViewportPosition(logicalIndex) +
                                               header->sectionSize(logicalIndex) / 2;
                            if (mouseX > center) {
                                targetVisualIndex = visualIndex;
                            } else {
                                break;
                            }
                        }
                    } else if (mouseX < currentStart) {
                        for (int visualIndex = currentVisualIndex - 1; visualIndex >= 0;
                             --visualIndex) {
                            const int logicalIndex = header->logicalIndex(visualIndex);
                            const int center = header->sectionViewportPosition(logicalIndex) +
                                               header->sectionSize(logicalIndex) / 2;
                            if (mouseX < center) {
                                targetVisualIndex = visualIndex;
                            } else {
                                break;
                            }
                        }
                    }
                    if (targetVisualIndex != currentVisualIndex) {
                        header->moveSection(currentVisualIndex, targetVisualIndex);
                    }
            }
        } else if (event->type() == QEvent::MouseButtonRelease &&
                   static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            if (m_headerDragActive) {
                m_headerDragActive = false;
                scheduleTableHeaderStateSave();
            }
            m_headerDragLogicalIndex = -1;
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
            const rfm::core::InternalTransferAction action =
                requestedTransferAction(dragEvent->modifiers());
            const Qt::DropAction dropAction = qtDropAction(action);
            const bool unsupportedCrossSourceMove =
                payload.has_value() && action == rfm::core::InternalTransferAction::Move &&
                payload->source != m_currentLocation.source;
            const bool valid =
                payload.has_value() && dragEvent->possibleActions().testFlag(dropAction) &&
                !unsupportedCrossSourceMove && validateDrop(*payload, destination).accepted();
            updateDropAppearance(payload.has_value(), valid, valid ? folderRow : -1);
            if (valid) {
                dragEvent->setDropAction(dropAction);
                dragEvent->accept();
            } else if (unsupportedCrossSourceMove) {
                dragEvent->setDropAction(Qt::IgnoreAction);
                dragEvent->ignore();
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
            const rfm::core::InternalTransferAction action =
                requestedTransferAction(dropEvent->modifiers());
            const Qt::DropAction dropAction = qtDropAction(action);
            const bool unsupportedCrossSourceMove =
                payload.has_value() && action == rfm::core::InternalTransferAction::Move &&
                payload->source != m_currentLocation.source;
            const bool valid =
                payload.has_value() && dropEvent->possibleActions().testFlag(dropAction) &&
                !unsupportedCrossSourceMove && validateDrop(*payload, destination).accepted();
            updateDropAppearance(false, false);
            if (!valid) {
                if (unsupportedCrossSourceMove) {
                    emit crossSourceMoveUnsupported();
                }
                dropEvent->setDropAction(Qt::IgnoreAction);
                dropEvent->ignore();
                return true;
            }
            dropEvent->setDropAction(dropAction);
            dropEvent->accept();
            emit activated();
            emit internalDropRequested(
                *payload, action, destination,
                transferActionWasExplicitlyRequested(dropEvent->modifiers()));
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
    QHash<QString, quint64> preservedDirectoryCounts;
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
        if (normalizedLocation.source == rfm::core::FileSource::Ssh) {
            for (int row = 0; row < m_fileTable->rowCount(); ++row) {
                const QTableWidgetItem* const nameItem = m_fileTable->item(row, 0);
                const QTableWidgetItem* const sizeItem = m_fileTable->item(row, 1);
                if (nameItem != nullptr && sizeItem != nullptr &&
                    nameItem->data(Qt::UserRole).toBool() &&
                    !nameItem->data(Qt::UserRole + 1).toBool() &&
                    sizeItem->data(SortValueKnownRole).toBool()) {
                    preservedDirectoryCounts.insert(nameItem->text(),
                                                    sizeItem->data(SortValueRole).toULongLong());
                }
            }
        }
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
    const bool internalTransferEnabled = m_currentLocation.source != rfm::core::FileSource::None;
    m_fileTable->setDragEnabled(internalTransferEnabled);
    m_fileTable->setAcceptDrops(internalTransferEnabled);
    m_fileTable->viewport()->setAcceptDrops(internalTransferEnabled);
    const bool sortingEnabled = m_fileTable->isSortingEnabled();
    m_fileTable->setSortingEnabled(false);
    ++m_directoryCountGeneration;
    m_pendingDirectoryCountNames.clear();
    m_fileTable->setRowCount(static_cast<int>(entries.size()));
    for (qsizetype row = 0; row < entries.size(); ++row) {
        const auto& entry = entries.at(row);
        const FilePresentation presentation = presentationForEntry(entry);
        auto* const nameItem = new SortableFileItem(entry.name, SortKind::Text, entry.name,
                                                    entry.name, entry.directory);
        nameItem->setIcon(presentation.icon);
        nameItem->setData(Qt::UserRole, entry.directory);
        nameItem->setData(Qt::UserRole + 1, entry.symbolicLink);
        nameItem->setData(Qt::UserRole + 2, QVariant::fromValue(entry));
        nameItem->setData(SortNaturalIndexRole, static_cast<int>(row));
        m_fileTable->setItem(static_cast<int>(row), 0, nameItem);
        const qint64 displaySize =
            entry.size > static_cast<quint64>(std::numeric_limits<qint64>::max())
                ? std::numeric_limits<qint64>::max()
                : static_cast<qint64>(entry.size);
        const bool countableDirectory = entry.directory && !entry.symbolicLink;
        if (countableDirectory) {
            m_pendingDirectoryCountNames.push_back(entry.name);
        }
        const auto preservedCount = preservedDirectoryCounts.constFind(entry.name);
        const bool hasPreservedCount =
            countableDirectory && preservedCount != preservedDirectoryCounts.cend();
        auto* const sizeItem = new SortableFileItem(
            hasPreservedCount    ? directoryItemCountText(*preservedCount)
            : countableDirectory ? tr("…")
            : entry.directory    ? tr("—")
                                 : QLocale{}.formattedDataSize(displaySize),
            SortKind::Size, QVariant::fromValue(hasPreservedCount ? *preservedCount : entry.size),
            entry.name, entry.directory, !countableDirectory || hasPreservedCount);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_fileTable->setItem(static_cast<int>(row), 1, sizeItem);
        m_fileTable->setItem(static_cast<int>(row), 2,
                             new SortableFileItem(presentation.type, SortKind::Text,
                                                  presentation.type, entry.name, entry.directory));
        m_fileTable->setItem(static_cast<int>(row), 3,
                             new SortableFileItem(modifiedDisplayText(entry.modifiedAt),
                                                  SortKind::Modified, entry.modifiedAt, entry.name,
                                                  entry.directory));
    }
    m_fileTable->setSortingEnabled(sortingEnabled);
    if (sortingEnabled) {
        applySortState();
    }
    applyAdaptiveLayout();
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
    QTimer::singleShot(0, this, &FileBrowserPane::requestNextDirectoryItemCount);
}

void FileBrowserPane::clear()
{
    m_currentLocation = {};
    m_contextMenuRow = -1;
    m_pendingSelectionNames.clear();
    ++m_directoryCountGeneration;
    m_pendingDirectoryCountNames.clear();
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

void FileBrowserPane::setDirectoryItemCount(const rfm::core::BrowserLocation& location,
                                            quint64 generation, const QString& name,
                                            std::optional<quint64> count)
{
    if (location != m_activeDirectoryCountLocation ||
        generation != m_activeDirectoryCountGeneration || name != m_activeDirectoryCountName) {
        return;
    }
    m_activeDirectoryCountLocation = {};
    m_activeDirectoryCountName.clear();
    m_activeDirectoryCountGeneration = 0;

    if (location == m_currentLocation && generation == m_directoryCountGeneration) {
        for (int row = 0; row < m_fileTable->rowCount(); ++row) {
            const QTableWidgetItem* const nameItem = m_fileTable->item(row, 0);
            if (nameItem == nullptr || nameItem->text() != name) {
                continue;
            }
            QTableWidgetItem* const sizeItem = m_fileTable->item(row, 1);
            if (sizeItem != nullptr) {
                const bool hadKnownCount = sizeItem->data(SortValueKnownRole).toBool();
                const bool countChanged =
                    count.has_value() &&
                    (!hadKnownCount || sizeItem->data(SortValueRole).toULongLong() != *count);
                const bool showFailure =
                    !count.has_value() && !hadKnownCount && sizeItem->text() != tr("—");
                if (!countChanged && !showFailure) {
                    break;
                }
                const bool sortingEnabled = m_fileTable->isSortingEnabled();
                m_fileTable->setSortingEnabled(false);
                sizeItem->setText(count.has_value() ? directoryItemCountText(*count) : tr("—"));
                sizeItem->setData(SortValueKnownRole, count.has_value());
                if (count.has_value()) {
                    sizeItem->setData(SortValueRole, QVariant::fromValue(*count));
                }
                m_fileTable->setSortingEnabled(sortingEnabled);
            }
            break;
        }
    }
    QTimer::singleShot(0, this, &FileBrowserPane::requestNextDirectoryItemCount);
}

void FileBrowserPane::requestNextDirectoryItemCount()
{
    if (!m_activeDirectoryCountName.isEmpty() || m_pendingDirectoryCountNames.isEmpty() ||
        !m_currentLocation.isValid()) {
        return;
    }
    m_activeDirectoryCountLocation = m_currentLocation;
    m_activeDirectoryCountGeneration = m_directoryCountGeneration;
    m_activeDirectoryCountName = m_pendingDirectoryCountNames.takeFirst();
    emit directoryItemCountRequested(m_activeDirectoryCountLocation,
                                     m_activeDirectoryCountGeneration, m_activeDirectoryCountName);
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
    Q_UNUSED(supportedActions)
    const QByteArray data = createInternalDragData();
    if (data.isEmpty()) {
        return;
    }
    emit internalDragStarted();
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
    const QString itemKind = m_currentLocation.source == rfm::core::FileSource::Local
                                 ? tr("%1 local item(s)")
                                 : tr("%1 remote item(s)");
    painter.drawText(pixmap.rect(), Qt::AlignCenter, itemKind.arg(selectedEntries().size()));
    drag.setPixmap(pixmap);
    drag.setHotSpot(QPoint(12, 12));
    const Qt::DropActions allowedActions = Qt::CopyAction | Qt::MoveAction;
    drag.exec(allowedActions,
              qtDropAction(requestedTransferAction(QApplication::keyboardModifiers())));
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
    return m_currentLocation.source == rfm::core::FileSource::Local
               ? QDir(m_currentLocation.path).filePath(name->text())
               : rfm::core::RemotePath::join(m_currentLocation.path, name->text());
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
    rfm::core::BrowserLocation destinationLocation = m_currentLocation;
    destinationLocation.path = destination;
    return rfm::core::validateInternalTransfer(
        payload, m_applicationInstanceId, destinationLocation, m_connectionIdentity,
        rfm::core::InternalTransferCompatibility::AllowLocalAndSsh);
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
            name == nullptr
                ? QString{}
                : (m_currentLocation.source == rfm::core::FileSource::Local
                       ? QDir(m_currentLocation.path).filePath(name->text())
                       : rfm::core::RemotePath::join(m_currentLocation.path, name->text()));
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
