#include "remotefilemanager/app/MainWindow.hpp"

#include <QLineEdit>
#include <QPushButton>
#include <QTest>

class MainWindowTest final : public QObject {
    Q_OBJECT

private slots:
    void exposesInitialDisconnectedShell();
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

QTEST_MAIN(MainWindowTest)

#include "test_main_window.moc"
