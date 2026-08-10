#include "remotefilemanager/app/ConnectionDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>

class ConnectionDialogTest final : public QObject
{
    Q_OBJECT

  private slots:
    void validatesRequiredFields();
    void requestDoesNotCloseDialogAndShowsConnectingState();
    void errorRestoresFieldsAndPreservesValues();
    void successAcceptsOnlyAnActiveAttempt();
};

void ConnectionDialogTest::validatesRequiredFields()
{
    rfm::app::ConnectionDialog dialog;
    auto* const host = dialog.findChild<QLineEdit*>(QStringLiteral("hostEdit"));
    auto* const user = dialog.findChild<QLineEdit*>(QStringLiteral("usernameEdit"));
    auto* const connect = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    auto* const save = dialog.findChild<QCheckBox*>(QStringLiteral("saveServerCheck"));
    QVERIFY(host != nullptr);
    QVERIFY(user != nullptr);
    QVERIFY(connect != nullptr);
    QVERIFY(save != nullptr);
    QVERIFY(!connect->isEnabled());
    QVERIFY(!save->isChecked());
    QVERIFY(!dialog.saveServerRequested());
    host->setText(QStringLiteral("server.example.test"));
    user->setText(QStringLiteral("alice"));
    QVERIFY(connect->isEnabled());
}

void ConnectionDialogTest::requestDoesNotCloseDialogAndShowsConnectingState()
{
    rfm::app::ConnectionDialog dialog;
    dialog.show();
    dialog.findChild<QLineEdit*>(QStringLiteral("hostEdit"))
        ->setText(QStringLiteral("server.example.test"));
    dialog.findChild<QLineEdit*>(QStringLiteral("usernameEdit"))->setText(QStringLiteral("alice"));
    auto* const buttons = dialog.findChild<QDialogButtonBox*>();
    auto* const save = dialog.findChild<QCheckBox*>(QStringLiteral("saveServerCheck"));
    save->setChecked(true);
    QSignalSpy requested(&dialog, &rfm::app::ConnectionDialog::connectionRequested);
    buttons->button(QDialogButtonBox::Ok)->click();

    QCOMPARE(requested.size(), 1);
    QVERIFY(dialog.isVisible());
    QCOMPARE(dialog.result(), 0);
    QCOMPARE(dialog.state(), rfm::app::ConnectionDialog::State::Connecting);
    QVERIFY(!dialog.findChild<QLineEdit*>(QStringLiteral("hostEdit"))->isEnabled());
    QVERIFY(!dialog.findChild<QLineEdit*>(QStringLiteral("usernameEdit"))->isEnabled());
    QVERIFY(!dialog.findChild<QSpinBox*>(QStringLiteral("portSpin"))->isEnabled());
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(!buttons->button(QDialogButtonBox::Cancel)->isEnabled());
    QVERIFY(!save->isEnabled());
    QVERIFY(dialog.saveServerRequested());
    QVERIFY(dialog.findChild<QProgressBar*>(QStringLiteral("connectionActivityIndicator"))
                ->isVisible());
}

void ConnectionDialogTest::errorRestoresFieldsAndPreservesValues()
{
    rfm::app::ConnectionDialog dialog;
    dialog.show();
    auto* const host = dialog.findChild<QLineEdit*>(QStringLiteral("hostEdit"));
    auto* const user = dialog.findChild<QLineEdit*>(QStringLiteral("usernameEdit"));
    auto* const port = dialog.findChild<QSpinBox*>(QStringLiteral("portSpin"));
    auto* const fallback =
        dialog.findChild<QCheckBox*>(QStringLiteral("passwordFallbackCheck"));
    auto* const password = dialog.findChild<QLineEdit*>(QStringLiteral("passwordEdit"));
    auto* const buttons = dialog.findChild<QDialogButtonBox*>();
    auto* const save = dialog.findChild<QCheckBox*>(QStringLiteral("saveServerCheck"));
    host->setText(QStringLiteral("wrong.example.test"));
    user->setText(QStringLiteral("alice"));
    port->setValue(2222);
    fallback->setChecked(true);
    const QString ephemeralSecret(16, QChar{'x'});
    password->setText(ephemeralSecret);
    save->setChecked(true);
    buttons->button(QDialogButtonBox::Ok)->click();

    dialog.showConnectionError(QStringLiteral("Connection refused"));
    QCOMPARE(dialog.state(), rfm::app::ConnectionDialog::State::Error);
    QVERIFY(dialog.isVisible());
    QCOMPARE(dialog.result(), 0);
    QVERIFY(host->isEnabled());
    QVERIFY(user->isEnabled());
    QVERIFY(port->isEnabled());
    QVERIFY(fallback->isEnabled());
    QVERIFY(password->isEnabled());
    QVERIFY(save->isEnabled());
    QVERIFY(save->isChecked());
    QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QCOMPARE(host->text(), QStringLiteral("wrong.example.test"));
    QCOMPARE(user->text(), QStringLiteral("alice"));
    QCOMPARE(port->value(), 2222);
    QCOMPARE(password->text(), ephemeralSecret);
    const auto* const status =
        dialog.findChild<QLabel*>(QStringLiteral("connectionStatusLabel"));
    QCOMPARE(status->text(), QStringLiteral("Connection refused"));
    QVERIFY(!status->text().contains(password->text()));
}

void ConnectionDialogTest::successAcceptsOnlyAnActiveAttempt()
{
    rfm::app::ConnectionDialog dialog;
    dialog.connectionSucceeded();
    QCOMPARE(dialog.result(), 0);

    dialog.findChild<QLineEdit*>(QStringLiteral("hostEdit"))
        ->setText(QStringLiteral("server.example.test"));
    dialog.findChild<QLineEdit*>(QStringLiteral("usernameEdit"))->setText(QStringLiteral("alice"));
    dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    dialog.connectionSucceeded();
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
}

QTEST_MAIN(ConnectionDialogTest)

#include "test_connection_dialog.moc"
