#include "remotefilemanager/app/ConnectionDialog.hpp"
#include "remotefilemanager/app/MainWindow.hpp"
#include "remotefilemanager/app/TransferPanel.hpp"
#include "remotefilemanager/app/TransferRequestFactory.hpp"

#include <QAction>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace
{

rfm::core::TransferProgress progress(quint64 id, rfm::core::TransferState state,
                                     quint64 transferred = 0, quint64 total = 0, quint64 speed = 0)
{
    rfm::core::TransferProgress value;
    value.id = id;
    value.state = state;
    value.source = QStringLiteral("/tmp/archive.bin");
    value.destination = QStringLiteral("/srv/archive.bin");
    value.transferredBytes = transferred;
    value.totalBytes = total;
    value.bytesPerSecond = speed;
    value.direction = rfm::core::TransferDirection::Upload;
    return value;
}

int rowForId(QTableWidget* table, quint64 id)
{
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->data(Qt::UserRole).toULongLong() == id) {
            return row;
        }
    }
    return -1;
}

} // namespace

class MainWindowTest final : public QObject
{
    Q_OBJECT

  private slots:
    void exposesInitialDisconnectedShell();
    void validatesSecureConnectionForm();
    void enablesMultipleRemoteSelection();
    void buildsPortableTransferRequests();
    void queuesFilesAndFoldersAsSeparateUploads();
    void queuesDownloadsFromRemoteSelection();
    void displaysTransferProgressAndMultipleEntries();
    void displaysTransferStatesInEnglish();
    void exposesPauseResumeAndCancelIntentions();
    void displaysTerminalTransferStatesAndErrors();
    void formatsTransferSizesAndSpeeds();
    void refreshTimerIsConnectionAwareAndCoalescesListings();
    void refreshesAfterCompletedUpload();
};

void MainWindowTest::exposesInitialDisconnectedShell()
{
    rfm::app::MainWindow window;

    QCOMPARE(window.objectName(), QStringLiteral("mainWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("RemoteFileManager"));

    const auto* const pathEdit = window.findChild<QLineEdit*>(QStringLiteral("remotePathEdit"));
    QVERIFY(pathEdit != nullptr);
    QVERIFY(pathEdit->isReadOnly());

    const auto* const connectionButton =
        window.findChild<QPushButton*>(QStringLiteral("newConnectionButton"));
    QVERIFY(connectionButton != nullptr);
    QVERIFY(connectionButton->isEnabled());

    const auto* const transferPanel =
        window.findChild<rfm::app::TransferPanel*>(QStringLiteral("transferPanel"));
    const auto* const transferTable =
        window.findChild<QTableWidget*>(QStringLiteral("transferTable"));
    QVERIFY(transferPanel != nullptr);
    QVERIFY(transferTable != nullptr);
    QCOMPARE(transferTable->rowCount(), 0);
    const auto* const uploadAction = window.findChild<QAction*>(QStringLiteral("uploadAction"));
    const auto* const downloadAction = window.findChild<QAction*>(QStringLiteral("downloadAction"));
    QVERIFY(uploadAction != nullptr);
    QVERIFY(downloadAction != nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("uploadFilesAction")) == nullptr);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("uploadFolderAction")) == nullptr);
    QVERIFY(uploadAction->menu() == nullptr);
    QVERIFY(!uploadAction->isEnabled());
    QVERIFY(!downloadAction->isEnabled());
    const auto* const refreshTimer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(refreshTimer != nullptr);
    QCOMPARE(refreshTimer->interval(), 3000);
    QVERIFY(!refreshTimer->isActive());
}

void MainWindowTest::enablesMultipleRemoteSelection()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("first.txt"), 10, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false},
    };
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral(".")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("remoteFileTable"));
    QVERIFY(table != nullptr);
    QCOMPARE(table->selectionMode(), QAbstractItemView::ExtendedSelection);
    table->selectionModel()->select(table->model()->index(0, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(1, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(table->selectionModel()->selectedRows(0).size(), 2);
    auto* const createAction = window.findChild<QAction*>(QStringLiteral("createDirectoryAction"));
    auto* const renameAction = window.findChild<QAction*>(QStringLiteral("renameAction"));
    auto* const moveAction = window.findChild<QAction*>(QStringLiteral("moveAction"));
    auto* const copyAction = window.findChild<QAction*>(QStringLiteral("copyAction"));
    auto* const removeAction = window.findChild<QAction*>(QStringLiteral("removeAction"));
    auto* const uploadAction = window.findChild<QAction*>(QStringLiteral("uploadAction"));
    auto* const downloadAction = window.findChild<QAction*>(QStringLiteral("downloadAction"));
    QVERIFY(createAction != nullptr);
    QVERIFY(renameAction != nullptr);
    QVERIFY(moveAction != nullptr);
    QVERIFY(copyAction != nullptr);
    QVERIFY(removeAction != nullptr);
    QVERIFY(uploadAction != nullptr);
    QVERIFY(downloadAction != nullptr);
    QVERIFY(createAction->isEnabled());
    QVERIFY(!renameAction->isEnabled());
    QVERIFY(moveAction->isEnabled());
    QVERIFY(copyAction->isEnabled());
    QVERIFY(removeAction->isEnabled());
    QVERIFY(uploadAction->isEnabled());
    QVERIFY(downloadAction->isEnabled());
    QCOMPARE(uploadAction->text(), QStringLiteral("Upload"));
    QCOMPARE(downloadAction->text(), QStringLiteral("Download"));
    QCOMPARE(uploadAction->toolTip(), QStringLiteral("Upload files or folders to the server"));
    QCOMPARE(downloadAction->toolTip(), QStringLiteral("Download the selection to this computer"));
    QVERIFY(!uploadAction->icon().isNull());
    QVERIFY(!downloadAction->icon().isNull());
    QVERIFY(uploadAction->icon().cacheKey() != downloadAction->icon().cacheKey());
    const auto* const refreshTimer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(refreshTimer != nullptr);
    QVERIFY(refreshTimer->isActive());
}

void MainWindowTest::validatesSecureConnectionForm()
{
    rfm::app::ConnectionDialog dialog;
    auto* const host = dialog.findChild<QLineEdit*>(QStringLiteral("hostEdit"));
    auto* const user = dialog.findChild<QLineEdit*>(QStringLiteral("usernameEdit"));
    auto* const port = dialog.findChild<QSpinBox*>(QStringLiteral("portSpin"));
    auto* const buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(host != nullptr);
    QVERIFY(user != nullptr);
    QVERIFY(port != nullptr);
    QVERIFY(buttons != nullptr);
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());

    host->setText(QStringLiteral("server.example.test"));
    user->setText(QStringLiteral("gabriel"));
    QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QCOMPARE(dialog.profile().port, quint16{22});
    QCOMPARE(dialog.profile().effectiveDisplayName(),
             QStringLiteral("gabriel@server.example.test"));
}

void MainWindowTest::buildsPortableTransferRequests()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString filePath = temporary.filePath(QStringLiteral("archive.bin"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("data"), qint64{4});
    file.close();
    const QString folderPath = temporary.filePath(QStringLiteral("photos"));
    QVERIFY(QDir().mkdir(folderPath));

    const auto fileUpload =
        rfm::app::TransferRequestFactory::upload(101, filePath, QStringLiteral("/srv/uploads"));
    QVERIFY(fileUpload.has_value());
    QCOMPARE(fileUpload->destination, QStringLiteral("/srv/uploads/archive.bin"));
    QVERIFY(!fileUpload->directory);

    const auto folderUpload =
        rfm::app::TransferRequestFactory::upload(102, folderPath, QStringLiteral("/srv/uploads"));
    QVERIFY(folderUpload.has_value());
    QCOMPARE(folderUpload->destination, QStringLiteral("/srv/uploads/photos"));
    QVERIFY(folderUpload->directory);

    const auto download = rfm::app::TransferRequestFactory::download(
        103, {QStringLiteral("/srv/photos"), true}, temporary.path());
    QVERIFY(download.has_value());
    QCOMPARE(download->destination, QDir(temporary.path()).filePath(QStringLiteral("photos")));
    QVERIFY(download->directory);
}

void MainWindowTest::queuesFilesAndFoldersAsSeparateUploads()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString filePath = temporary.filePath(QStringLiteral("archive.bin"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("data"), qint64{4});
    file.close();
    const QString folderPath = temporary.filePath(QStringLiteral("photos"));
    QVERIFY(QDir().mkdir(folderPath));

    rfm::app::MainWindow window;
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv/uploads")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, {})));
    QSignalSpy requested(&window, &rfm::app::MainWindow::transferRequested);
    const QStringList localPaths{filePath, folderPath};
    QVERIFY(QMetaObject::invokeMethod(&window, "queueUploads", Qt::DirectConnection,
                                      Q_ARG(QStringList, localPaths)));
    QCOMPARE(requested.size(), 2);

    const auto fileRequest = requested.at(0).constFirst().value<rfm::core::TransferRequest>();
    const auto folderRequest = requested.at(1).constFirst().value<rfm::core::TransferRequest>();
    QCOMPARE(fileRequest.destination, QStringLiteral("/srv/uploads/archive.bin"));
    QVERIFY(!fileRequest.directory);
    QCOMPARE(folderRequest.destination, QStringLiteral("/srv/uploads/photos"));
    QVERIFY(folderRequest.directory);
}

void MainWindowTest::queuesDownloadsFromRemoteSelection()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("archive.tar"), 10, {}, false, false},
        {QStringLiteral("photos"), 0, {}, true, false},
    };
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("remoteFileTable"));
    QVERIFY(table != nullptr);
    table->selectionModel()->select(table->model()->index(0, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    table->selectionModel()->select(table->model()->index(1, 0),
                                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QSignalSpy requested(&window, &rfm::app::MainWindow::transferRequested);
    QTemporaryDir destination;
    QVERIFY(destination.isValid());

    QVERIFY(QMetaObject::invokeMethod(&window, "queueDownloads", Qt::DirectConnection,
                                      Q_ARG(QString, destination.path())));
    QCOMPARE(requested.size(), 2);
    auto* const newConnectionAction =
        window.findChild<QAction*>(QStringLiteral("newConnectionAction"));
    QVERIFY(newConnectionAction != nullptr);
    QVERIFY(!newConnectionAction->isEnabled());
    QHash<QString, rfm::core::TransferRequest> bySource;
    for (const QList<QVariant>& arguments : requested) {
        const auto request = arguments.constFirst().value<rfm::core::TransferRequest>();
        bySource.insert(request.source, request);
    }
    QCOMPARE(bySource.value(QStringLiteral("/srv/archive.tar")).destination,
             QDir(destination.path()).filePath(QStringLiteral("archive.tar")));
    QVERIFY(!bySource.value(QStringLiteral("/srv/archive.tar")).directory);
    QCOMPARE(bySource.value(QStringLiteral("/srv/photos")).destination,
             QDir(destination.path()).filePath(QStringLiteral("photos")));
    QVERIFY(bySource.value(QStringLiteral("/srv/photos")).directory);
}

void MainWindowTest::displaysTransferProgressAndMultipleEntries()
{
    rfm::app::TransferPanel panel;
    panel.show();
    auto first = progress(201, rfm::core::TransferState::Queued);
    panel.updateTransfer(first);
    auto second = progress(202, rfm::core::TransferState::Queued);
    second.source = QStringLiteral("/tmp/photos");
    second.destination = QStringLiteral("/srv/photos");
    second.directory = true;
    panel.updateTransfer(second);

    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("transferTable"));
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 2);
    const int firstRow = rowForId(table, 201);
    QVERIFY(firstRow >= 0);
    QCOMPARE(table->horizontalHeaderItem(0)->text(), QStringLiteral("Direction"));
    QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("Status"));
    QCOMPARE(table->horizontalHeaderItem(4)->text(), QStringLiteral("Progress"));
    QCOMPARE(table->horizontalHeaderItem(5)->text(), QStringLiteral("Speed"));
    QCOMPARE(table->horizontalHeaderItem(7)->text(), QStringLiteral("Error"));
    QCOMPARE(table->item(firstRow, 0)->text(), QStringLiteral("↑ Upload"));
    QCOMPARE(table->item(firstRow, 3)->text(), QStringLiteral("Queued"));

    panel.updateTransfer(progress(201, rfm::core::TransferState::Transferring, 512, 1024, 1024));
    QCOMPARE(table->item(firstRow, 3)->text(), QStringLiteral("Transferring"));
    QCOMPARE(table->item(firstRow, 5)->text(), QStringLiteral("1.0 KiB/s"));
    auto* const bar = qobject_cast<QProgressBar*>(table->cellWidget(firstRow, 4));
    QVERIFY(bar != nullptr);
    QCOMPARE(bar->value(), 500);
    QCOMPARE(bar->format(), QStringLiteral("512 B / 1.0 KiB"));
}

void MainWindowTest::displaysTransferStatesInEnglish()
{
    rfm::app::TransferPanel panel;
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("transferTable"));
    QVERIFY(table != nullptr);
    const QList<QPair<rfm::core::TransferState, QString>> states{
        {rfm::core::TransferState::Queued, QStringLiteral("Queued")},
        {rfm::core::TransferState::Preparing, QStringLiteral("Preparing")},
        {rfm::core::TransferState::Transferring, QStringLiteral("Transferring")},
        {rfm::core::TransferState::Paused, QStringLiteral("Paused")},
        {rfm::core::TransferState::Finalizing, QStringLiteral("Finalizing")},
        {rfm::core::TransferState::Cancelling, QStringLiteral("Cancelling")},
        {rfm::core::TransferState::Completed, QStringLiteral("Completed")},
        {rfm::core::TransferState::Cancelled, QStringLiteral("Cancelled")},
        {rfm::core::TransferState::Failed, QStringLiteral("Failed")},
    };
    quint64 id = 600;
    for (const auto& [state, text] : states) {
        panel.updateTransfer(progress(++id, state));
        QCOMPARE(table->item(rowForId(table, id), 3)->text(), text);
    }
}

void MainWindowTest::exposesPauseResumeAndCancelIntentions()
{
    rfm::app::TransferPanel panel;
    panel.show();
    QSignalSpy pauses(&panel, &rfm::app::TransferPanel::pauseRequested);
    QSignalSpy resumes(&panel, &rfm::app::TransferPanel::resumeRequested);
    QSignalSpy cancellations(&panel, &rfm::app::TransferPanel::cancelRequested);
    panel.updateTransfer(progress(301, rfm::core::TransferState::Transferring, 1, 10));
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("transferTable"));
    QVERIFY(table != nullptr);
    const int row = rowForId(table, 301);
    QWidget* const actions = table->cellWidget(row, 6);
    auto* const pauseResume = actions->findChild<QPushButton*>(QStringLiteral("pauseResumeButton"));
    auto* const cancel = actions->findChild<QPushButton*>(QStringLiteral("cancelTransferButton"));
    QVERIFY(pauseResume != nullptr);
    QVERIFY(cancel != nullptr);
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(1), QHeaderView::Stretch);
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(2), QHeaderView::Stretch);
    QCOMPARE(table->horizontalHeader()->sectionResizeMode(6), QHeaderView::ResizeToContents);
    QVERIFY(pauseResume->minimumWidth() >= pauseResume->sizeHint().width());
    QVERIFY(cancel->minimumWidth() >= cancel->sizeHint().width());
    QVERIFY(actions->minimumWidth() >= pauseResume->minimumWidth() + cancel->minimumWidth());
    for (const int width : {1000, 760, 520}) {
        panel.resize(width, 240);
        QCoreApplication::processEvents();
        QVERIFY(table->columnWidth(6) >= actions->minimumWidth());
        QVERIFY(pauseResume->width() >= pauseResume->minimumWidth());
        QVERIFY(cancel->width() >= cancel->minimumWidth());
    }
    QCOMPARE(pauseResume->text(), QStringLiteral("Pause"));
    QTest::mouseClick(pauseResume, Qt::LeftButton);
    QCOMPARE(pauses.size(), 1);

    panel.updateTransfer(progress(301, rfm::core::TransferState::Paused, 1, 10));
    QCOMPARE(pauseResume->text(), QStringLiteral("Resume"));
    QTest::mouseClick(pauseResume, Qt::LeftButton);
    QCOMPARE(resumes.size(), 1);
    panel.updateTransfer(progress(301, rfm::core::TransferState::Transferring, 2, 10));
    QCOMPARE(pauseResume->text(), QStringLiteral("Pause"));
    QTest::mouseClick(cancel, Qt::LeftButton);
    QCOMPARE(cancellations.size(), 1);
    QCOMPARE(cancellations.constFirst().constFirst().toULongLong(), quint64{301});
    QCOMPARE(cancel->text(), QStringLiteral("Cancel"));
}

void MainWindowTest::displaysTerminalTransferStatesAndErrors()
{
    rfm::app::TransferPanel panel;
    panel.show();
    panel.updateTransfer(progress(401, rfm::core::TransferState::Completed, 10, 10));
    auto failure = progress(402, rfm::core::TransferState::Failed, 4, 10);
    failure.error = QStringLiteral("/srv/archive: permission denied");
    panel.updateTransfer(failure);
    auto* const table = panel.findChild<QTableWidget*>(QStringLiteral("transferTable"));
    QVERIFY(table != nullptr);
    const int completedRow = rowForId(table, 401);
    const int failedRow = rowForId(table, 402);
    QCOMPARE(table->item(completedRow, 3)->text(), QStringLiteral("Completed"));
    QCOMPARE(table->item(failedRow, 3)->text(), QStringLiteral("Failed"));
    QCOMPARE(table->item(failedRow, 7)->text(), failure.error);
    QWidget* const completedActions = table->cellWidget(completedRow, 6);
    QVERIFY(
        completedActions->findChild<QPushButton*>(QStringLiteral("pauseResumeButton"))->isHidden());
    QVERIFY(completedActions->findChild<QPushButton*>(QStringLiteral("cancelTransferButton"))
                ->isHidden());
}

void MainWindowTest::formatsTransferSizesAndSpeeds()
{
    QCOMPARE(rfm::app::TransferPanel::formatBytes(0), QStringLiteral("0 B"));
    QCOMPARE(rfm::app::TransferPanel::formatBytes(1536), QStringLiteral("1.5 KiB"));
    QCOMPARE(rfm::app::TransferPanel::formatSpeed(0), QStringLiteral("—"));
    QCOMPARE(rfm::app::TransferPanel::formatSpeed(1024), QStringLiteral("1.0 KiB/s"));
}

void MainWindowTest::refreshTimerIsConnectionAwareAndCoalescesListings()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("selected.txt"), 10, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false},
    };
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    auto* const table = window.findChild<QTableWidget*>(QStringLiteral("remoteFileTable"));
    auto* const timer = window.findChild<QTimer*>(QStringLiteral("autoRefreshTimer"));
    QVERIFY(table != nullptr);
    QVERIFY(timer != nullptr);
    QVERIFY(timer->isActive());
    table->selectRow(0);

    QObject::disconnect(&window, SIGNAL(directoryRequested(QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().constFirst().toString(), QStringLiteral("/srv"));
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);

    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QCOMPARE(table->selectionModel()->selectedRows(0).size(), 1);
    QCOMPARE(table->item(table->selectionModel()->selectedRows(0).constFirst().row(), 0)->text(),
             QStringLiteral("selected.txt"));
}

void MainWindowTest::refreshesAfterCompletedUpload()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("before.txt"), 10, {}, false, false},
    };
    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    QObject::disconnect(&window, SIGNAL(directoryRequested(QString)), nullptr, nullptr);
    QSignalSpy requested(&window, &rfm::app::MainWindow::directoryRequested);
    auto* const debounce = window.findChild<QTimer*>(QStringLiteral("refreshDebounceTimer"));
    QVERIFY(debounce != nullptr);

    auto upload = progress(501, rfm::core::TransferState::Completed, 10, 10);
    upload.destination = QStringLiteral("/srv/uploaded.txt");
    upload.direction = rfm::core::TransferDirection::Upload;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, upload)));
    QVERIFY(debounce->isActive());
    debounce->stop();
    QVERIFY(QMetaObject::invokeMethod(debounce, "timeout", Qt::DirectConnection));
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().constFirst().toString(), QStringLiteral("/srv"));

    QVERIFY(QMetaObject::invokeMethod(&window, "showRemoteDirectory", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("/srv")),
                                      Q_ARG(QList<rfm::core::RemoteEntry>, entries)));
    requested.clear();
    auto download = progress(502, rfm::core::TransferState::Completed, 10, 10);
    download.direction = rfm::core::TransferDirection::Download;
    QVERIFY(QMetaObject::invokeMethod(&window, "handleTransferProgress", Qt::DirectConnection,
                                      Q_ARG(rfm::core::TransferProgress, download)));
    QVERIFY(!debounce->isActive());
    QCOMPARE(requested.size(), 0);
}

QTEST_MAIN(MainWindowTest)

#include "test_main_window.moc"
