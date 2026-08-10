#include "remotefilemanager/app/FileBrowserPane.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QColor>
#include <QFileIconProvider>
#include <QEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QLocale>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

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
                            base.blueF() * baseRatio + accent.blueF() * accentRatio,
                            base.alphaF());
}

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

    m_fileTable = new QTableWidget(this);
    m_fileTable->setObjectName(QStringLiteral("remoteFileTable"));
    m_fileTable->setColumnCount(3);
    m_fileTable->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified")});
    m_fileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fileTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fileTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_fileTable->setShowGrid(false);
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

bool FileBrowserPane::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == this || watched == m_pathEdit || watched == m_fileTable ||
         watched == m_fileTable->viewport()) &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn)) {
        emit activated();
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
    if (item == nullptr || !item->data(Qt::UserRole).toBool()) {
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

} // namespace rfm::app
