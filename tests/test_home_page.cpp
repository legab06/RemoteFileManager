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
    void adaptsActionsToAvailableWidth();
    void propagatesMinimumHeightThroughStack();
    void growsServerListWithinBounds();
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

void HomePageTest::adaptsActionsToAvailableWidth()
{
    QStackedWidget stack;
    auto* const page = new rfm::app::HomePage(&stack);
    stack.addWidget(page);
    stack.setCurrentWidget(page);
    auto* const list = page->findChild<QListWidget*>(QStringLiteral("homeServerList"));
    auto* const contentLayout = qobject_cast<QVBoxLayout*>(list->parentWidget()->layout());
    auto* const actions = contentLayout->itemAt(contentLayout->count() - 1)->layout();
    const auto buttons = page->findChildren<QPushButton*>();
    QCOMPARE(buttons.size(), 3);
    stack.resize(1200, 700);
    stack.show();
    QCoreApplication::processEvents();

    auto* const actionLayout = qobject_cast<QHBoxLayout*>(actions);
    QVERIFY(actionLayout != nullptr);
    QCOMPARE(actionLayout->direction(), QBoxLayout::LeftToRight);
    QCOMPARE(buttons[0]->text(), QStringLiteral("Connect"));
    QCOMPARE(buttons[1]->text(), QStringLiteral("Edit…"));
    QCOMPARE(buttons[2]->text(), QStringLiteral("New connection…"));
    const int actionHeight = actions->geometry().height();
    const int serverListWidth = list->width();

    stack.resize(1, 700);
    QCoreApplication::processEvents();
    QCOMPARE(actionLayout->direction(), QBoxLayout::LeftToRight);
    QVERIFY(buttons[0]->text().isEmpty());
    QVERIFY(buttons[1]->text().isEmpty());
    QVERIFY(buttons[2]->text().isEmpty());
    QCOMPARE(actions->geometry().height(), actionHeight);
    for (int index = 0; index < buttons.size(); ++index) {
        QVERIFY(buttons[index]->geometry().top() > list->geometry().bottom());
        QVERIFY(actions->geometry().contains(buttons[index]->geometry()));
        if (index > 0) {
            QCOMPARE(buttons[index]->geometry().top(), buttons[index - 1]->geometry().top());
        }
    }

    stack.resize(1200, 700);
    QCoreApplication::processEvents();
    QCOMPARE(buttons[0]->text(), QStringLiteral("Connect"));
    QCOMPARE(buttons[1]->text(), QStringLiteral("Edit…"));
    QCOMPARE(buttons[2]->text(), QStringLiteral("New connection…"));
    QCOMPARE(list->width(), serverListWidth);
    QCOMPARE(buttons[0]->toolTip(), QStringLiteral("Connect"));
    QCOMPARE(buttons[0]->accessibleName(), QStringLiteral("Connect"));
    QCOMPARE(buttons[1]->toolTip(), QStringLiteral("Edit server"));
    QCOMPARE(buttons[1]->accessibleName(), QStringLiteral("Edit server"));
    QCOMPARE(buttons[2]->toolTip(), QStringLiteral("New connection"));
    QCOMPARE(buttons[2]->accessibleName(), QStringLiteral("New connection"));
}

void HomePageTest::propagatesMinimumHeightThroughStack()
{
    QStackedWidget stack;
    auto* const page = new rfm::app::HomePage(&stack);
    stack.addWidget(page);
    stack.setCurrentWidget(page);
    stack.resize(744, 700);
    stack.show();
    QCoreApplication::processEvents();

    const int minimumHeight = stack.minimumSizeHint().height();
    QVERIFY(minimumHeight >= 395);
    stack.resize(744, minimumHeight);
    QCoreApplication::processEvents();

    const auto* const list = page->findChild<QListWidget*>(QStringLiteral("homeServerList"));
    auto* const content = list->parentWidget();
    page->layout()->activate();
    content->layout()->activate();
    const auto* const actions = content->layout()->itemAt(content->layout()->count() - 1);
    QVERIFY(actions != nullptr);
    QVERIFY(actions->geometry().top() - list->geometry().bottom() - 1 >= 0);

    stack.resize(744, 1);
    QCoreApplication::processEvents();
    QVERIFY(stack.height() >= minimumHeight);
    QVERIFY(actions->geometry().top() - list->geometry().bottom() - 1 >= 0);
}

void HomePageTest::growsServerListWithinBounds()
{
    QStackedWidget stack;
    auto* const page = new rfm::app::HomePage(&stack);
    stack.addWidget(page);
    stack.setCurrentWidget(page);
    stack.resize(900, 700);
    stack.show();
    QCoreApplication::processEvents();

    auto* const list = page->findChild<QListWidget*>(QStringLiteral("homeServerList"));
    const int compactWidth = list->width();
    const int compactHeight = list->height();
    const int frameStyle = list->frameStyle();
    const QString styleSheet = list->styleSheet();
    const QSize minimumSize = list->minimumSize();
    const QSize maximumSize = list->maximumSize();
    const QSizePolicy sizePolicy = list->sizePolicy();
    const QMargins contentsMargins = list->contentsMargins();
    const Qt::FocusPolicy focusPolicy = list->focusPolicy();

    stack.resize(1920, 1080);
    QCoreApplication::processEvents();
    QVERIFY(list->width() > compactWidth);
    QVERIFY(list->width() <= 702);
    QVERIFY(list->height() >= compactHeight);
    QVERIFY(list->height() <= 260);
    QCOMPARE(list->frameStyle(), frameStyle);
    QCOMPARE(list->styleSheet(), styleSheet);
    QCOMPARE(list->minimumSize(), minimumSize);
    QCOMPARE(list->maximumSize(), maximumSize);
    QCOMPARE(list->sizePolicy(), sizePolicy);
    QCOMPARE(list->contentsMargins(), contentsMargins);
    QCOMPARE(list->focusPolicy(), focusPolicy);
    const auto buttons = page->findChildren<QPushButton*>();
    QCOMPARE(buttons[0]->text(), QStringLiteral("Connect"));
    QCOMPARE(buttons[1]->text(), QStringLiteral("Edit…"));
    QCOMPARE(buttons[2]->text(), QStringLiteral("New connection…"));

    const auto* const content = list->parentWidget();
    content->layout()->activate();
    const auto* const actions = content->layout()->itemAt(content->layout()->count() - 1);
    QVERIFY(actions != nullptr);
    QVERIFY(actions->geometry().top() - list->geometry().bottom() - 1 >= 0);
}

QTEST_MAIN(HomePageTest)

#include "test_home_page.moc"
