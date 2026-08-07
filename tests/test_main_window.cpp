#include "remotefilemanager/app/ConnectionDialog.hpp"
#include "remotefilemanager/app/MainWindow.hpp"

#include <QAction>
#include <QDialogButtonBox>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTest>

class MainWindowTest final : public QObject {
    Q_OBJECT

private slots:
    void exposesInitialDisconnectedShell();
    void validatesSecureConnectionForm();
    void enablesMultipleRemoteSelection();
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
}

void MainWindowTest::enablesMultipleRemoteSelection()
{
    rfm::app::MainWindow window;
    const QList<rfm::core::RemoteEntry> entries{
        {QStringLiteral("first.txt"), 10, {}, false, false},
        {QStringLiteral("folder"), 0, {}, true, false},
    };
    QVERIFY(QMetaObject::invokeMethod(
        &window, "showRemoteDirectory", Qt::DirectConnection,
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
    auto* const createAction =
        window.findChild<QAction*>(QStringLiteral("createDirectoryAction"));
    auto* const renameAction = window.findChild<QAction*>(QStringLiteral("renameAction"));
    auto* const moveAction = window.findChild<QAction*>(QStringLiteral("moveAction"));
    auto* const copyAction = window.findChild<QAction*>(QStringLiteral("copyAction"));
    auto* const removeAction = window.findChild<QAction*>(QStringLiteral("removeAction"));
    QVERIFY(createAction != nullptr);
    QVERIFY(renameAction != nullptr);
    QVERIFY(moveAction != nullptr);
    QVERIFY(copyAction != nullptr);
    QVERIFY(removeAction != nullptr);
    QVERIFY(createAction->isEnabled());
    QVERIFY(!renameAction->isEnabled());
    QVERIFY(moveAction->isEnabled());
    QVERIFY(copyAction->isEnabled());
    QVERIFY(removeAction->isEnabled());
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

QTEST_MAIN(MainWindowTest)

#include "test_main_window.moc"
