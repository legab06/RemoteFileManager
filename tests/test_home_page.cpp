#include "remotefilemanager/app/HomePage.hpp"

#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

class HomePageTest final : public QObject
{
    Q_OBJECT

  private slots:
    void showsProfilesAndEmitsConnectionIntent();
    void exposesManualConnectionIntent();
};

void HomePageTest::showsProfilesAndEmitsConnectionIntent()
{
    rfm::app::HomePage page;
    const QList profiles{
        rfm::core::ConnectionProfile{QStringLiteral("NAS"), QStringLiteral("nas.example.test"),
                                     QStringLiteral("alice"), 2222, QStringLiteral("nas-id")},
        rfm::core::ConnectionProfile{QStringLiteral("VM"), QStringLiteral("vm.example.test"),
                                     QStringLiteral("bob"), 22, QStringLiteral("vm-id")}};
    page.setProfiles(profiles);

    auto* const list = page.findChild<QListWidget*>(QStringLiteral("homeServerList"));
    auto* const connect = page.findChild<QPushButton*>(QStringLiteral("homeConnectButton"));
    QVERIFY(list != nullptr);
    QVERIFY(connect != nullptr);
    QCOMPARE(list->count(), 2);
    QVERIFY(list->item(0)->text().contains(QStringLiteral("NAS")));
    QVERIFY(list->item(0)->text().contains(QStringLiteral("alice@nas.example.test:2222")));
    QVERIFY(!connect->isEnabled());

    QSignalSpy requested(&page, &rfm::app::HomePage::connectProfileRequested);
    list->setCurrentRow(0);
    QVERIFY(connect->isEnabled());
    connect->click();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().constFirst().toString(), QStringLiteral("nas-id"));
}

void HomePageTest::exposesManualConnectionIntent()
{
    rfm::app::HomePage page;
    auto* const newConnection =
        page.findChild<QPushButton*>(QStringLiteral("homeNewConnectionButton"));
    QVERIFY(newConnection != nullptr);
    QSignalSpy requested(&page, &rfm::app::HomePage::newConnectionRequested);
    newConnection->click();
    QCOMPARE(requested.size(), 1);
}

QTEST_MAIN(HomePageTest)

#include "test_home_page.moc"
