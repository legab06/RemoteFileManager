#include "remotefilemanager/app/HomePage.hpp"

#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>
#include <QVBoxLayout>

class HomePageTest final : public QObject
{
    Q_OBJECT

  private slots:
    void showsProfilesAndEmitsConnectionIntent();
    void exposesManualConnectionIntent();
    void exposesSelectedProfileEditIntent();
    void adaptsOnlyActionsToAvailableWidth_data();
    void adaptsOnlyActionsToAvailableWidth();
    void propagatesMinimumHeightThroughStack();
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

void HomePageTest::exposesSelectedProfileEditIntent()
{
    rfm::app::HomePage page;
    page.setProfiles({{QStringLiteral("NAS"), QStringLiteral("nas.example.test"),
                       QStringLiteral("alice"), 22, QStringLiteral("nas-id")}});
    auto* const list = page.findChild<QListWidget*>(QStringLiteral("homeServerList"));
    auto* const edit = page.findChild<QPushButton*>(QStringLiteral("homeEditServerButton"));
    QVERIFY(edit != nullptr);
    QVERIFY(!edit->isEnabled());
    list->setCurrentRow(0);
    QVERIFY(edit->isEnabled());
    QSignalSpy requested(&page, &rfm::app::HomePage::editProfileRequested);
    edit->click();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.constFirst().constFirst().toString(), QStringLiteral("nas-id"));
}

void HomePageTest::adaptsOnlyActionsToAvailableWidth_data()
{
    QTest::addColumn<int>("height");
    QTest::newRow("normal-height") << 700;
    QTest::newRow("short-window") << 480;
}

void HomePageTest::adaptsOnlyActionsToAvailableWidth()
{
    QFETCH(int, height);
    rfm::app::HomePage page;
    rfm::app::HomePage reference;
    auto* const list = page.findChild<QListWidget*>(QStringLiteral("homeServerList"));
    auto* const referenceList =
        reference.findChild<QListWidget*>(QStringLiteral("homeServerList"));
    auto* const contentLayout = qobject_cast<QVBoxLayout*>(list->parentWidget()->layout());
    auto* const actions = contentLayout->itemAt(contentLayout->count() - 1)->layout();

    // Restore HEAD's ordinary QHBoxLayout in the reference page.
    auto* const referenceLayout =
        qobject_cast<QVBoxLayout*>(referenceList->parentWidget()->layout());
    auto* const oldActions = referenceLayout->takeAt(referenceLayout->count() - 1)->layout();
    auto* const historicalActions = new QHBoxLayout;
    while (oldActions->count() > 0) {
        historicalActions->addItem(oldActions->takeAt(0));
    }
    historicalActions->setStretch(2, 1);
    delete oldActions;
    referenceLayout->addLayout(historicalActions);

    const auto buttons = page.findChildren<QPushButton*>();
    const auto referenceButtons = reference.findChildren<QPushButton*>();
    QCOMPARE(buttons.size(), 3);
    QCOMPARE(referenceButtons.size(), 3);
    page.resize(900, height);
    reference.resize(900, height);
    page.show();
    reference.show();
    QCoreApplication::processEvents();

    const auto checkListProperties = [&] {
        QCOMPARE(list->minimumSize(), referenceList->minimumSize());
        QCOMPARE(list->maximumSize(), referenceList->maximumSize());
        QCOMPARE(list->sizePolicy(), referenceList->sizePolicy());
        QCOMPARE(list->frameStyle(), referenceList->frameStyle());
        QCOMPARE(list->lineWidth(), referenceList->lineWidth());
        QCOMPARE(list->midLineWidth(), referenceList->midLineWidth());
        QCOMPARE(list->frameWidth(), referenceList->frameWidth());
        QCOMPARE(list->styleSheet(), referenceList->styleSheet());
        QCOMPARE(list->contentsMargins(), referenceList->contentsMargins());
        QCOMPARE(list->focusPolicy(), referenceList->focusPolicy());
    };
    checkListProperties();
    QCOMPARE(list->geometry(), referenceList->geometry());
    for (int index = 0; index < buttons.size(); ++index) {
        QCOMPARE(buttons[index]->geometry(), referenceButtons[index]->geometry());
    }

    // Use the action row's actual threshold, independent of font and platform.
    const int threshold = historicalActions->minimumSize().width();
    const auto outerMargins = page.layout()->contentsMargins();
    const auto contentMargins = contentLayout->contentsMargins();
    page.resize(outerMargins.left() + outerMargins.right() + contentMargins.left() +
                    contentMargins.right() + threshold - 1,
                height);
    QCoreApplication::processEvents();
    QVERIFY(actions->geometry().width() < threshold);
    QVERIFY(actions->geometry().height() >= actions->heightForWidth(actions->geometry().width()));
    for (int index = 0; index < buttons.size(); ++index) {
        QVERIFY(buttons[index]->geometry().top() > list->geometry().bottom());
        QVERIFY(actions->geometry().contains(buttons[index]->geometry()));
        if (index > 0) {
            QVERIFY(buttons[index]->geometry().top() > buttons[index - 1]->geometry().bottom());
        }
    }
    checkListProperties();

    page.setProfiles({{QStringLiteral("NAS"), QStringLiteral("nas.example.test"),
                       QStringLiteral("alice"), 22, QStringLiteral("nas-id")}});
    list->setCurrentRow(0);
    QSignalSpy connectRequested(&page, &rfm::app::HomePage::connectProfileRequested);
    QSignalSpy editRequested(&page, &rfm::app::HomePage::editProfileRequested);
    QSignalSpy newRequested(&page, &rfm::app::HomePage::newConnectionRequested);
    for (auto* const button : buttons) {
        QTest::mouseClick(button, Qt::LeftButton);
    }
    QCOMPARE(connectRequested.size(), 1);
    QCOMPARE(editRequested.size(), 1);
    QCOMPARE(newRequested.size(), 1);

    page.resize(900, height);
    QCoreApplication::processEvents();
    checkListProperties();
    QCOMPARE(list->geometry(), referenceList->geometry());
    for (int index = 0; index < buttons.size(); ++index) {
        QCOMPARE(buttons[index]->geometry(), referenceButtons[index]->geometry());
    }
}

void HomePageTest::propagatesMinimumHeightThroughStack()
{
    QStackedWidget stack;
    auto* const page = new rfm::app::HomePage(&stack);
    stack.addWidget(page);
    stack.setCurrentWidget(page);
    stack.resize(744, 395);
    stack.show();
    QCoreApplication::processEvents();

    const int minimumHeight = stack.minimumSizeHint().height();
    QVERIFY(minimumHeight > 395);
    stack.resize(744, 1);
    QCoreApplication::processEvents();
    QVERIFY(stack.height() >= minimumHeight);

    const auto* const list = page->findChild<QListWidget*>(QStringLiteral("homeServerList"));
    auto* const content = list->parentWidget();
    content->layout()->activate();
    const auto* const actions = content->layout()->itemAt(content->layout()->count() - 1);
    QVERIFY(actions != nullptr);
    QVERIFY(actions->geometry().top() - list->geometry().bottom() - 1 >= 0);
}

QTEST_MAIN(HomePageTest)

#include "test_home_page.moc"
