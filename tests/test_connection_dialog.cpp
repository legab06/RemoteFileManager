#include "remotefilemanager/app/ConnectionDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>

class ConnectionDialogTest final : public QObject
{
    Q_OBJECT

  private slots:
    void validatesSharedProfileFieldsIncludingName();
    void browsesForAnOptionalPrivateKey();
    void switchesAuthenticationModeAndControls();
    void requestDoesNotCloseDialogAndShowsConnectingState();
    void errorRestoresFieldsAndPreservesValues();
    void successAcceptsOnlyAnActiveAttempt();
};

void ConnectionDialogTest::validatesSharedProfileFieldsIncludingName()
{
    rfm::app::ConnectionDialog dialog;
    auto* const name = dialog.findChild<QLineEdit*>(QStringLiteral("serverNameEdit"));
    auto* const host = dialog.findChild<QLineEdit*>(QStringLiteral("serverHostEdit"));
    auto* const user = dialog.findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"));
    auto* const connect = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    auto* const save = dialog.findChild<QCheckBox*>(QStringLiteral("saveServerCheck"));
    auto* const passwordAuthentication =
        dialog.findChild<QCheckBox*>(QStringLiteral("passwordAuthenticationCheck"));
    auto* const privateKey = dialog.findChild<QLineEdit*>(QStringLiteral("privateKeyPathEdit"));
    auto* const browse = dialog.findChild<QPushButton*>(QStringLiteral("privateKeyBrowseButton"));
    QVERIFY(name != nullptr);
    QVERIFY(host != nullptr);
    QVERIFY(user != nullptr);
    QVERIFY(connect != nullptr);
    QVERIFY(save != nullptr);
    QVERIFY(passwordAuthentication != nullptr);
    QVERIFY(privateKey != nullptr);
    QVERIFY(browse != nullptr);
    QCOMPARE(passwordAuthentication->text(),
             QStringLiteral("Allow password authentication if key fails"));
    QVERIFY(!connect->isEnabled());
    QVERIFY(!save->isChecked());
    name->setText(QStringLiteral("Home server"));
    host->setText(QStringLiteral("server.example.test"));
    user->setText(QStringLiteral("alice"));
    privateKey->setText(QStringLiteral("~/.ssh/rfm_windows_server"));
    QVERIFY(connect->isEnabled());
    QCOMPARE(dialog.profile().displayName, QStringLiteral("Home server"));
    QCOMPARE(dialog.profile().privateKeyPath, QStringLiteral("~/.ssh/rfm_windows_server"));
}

void ConnectionDialogTest::browsesForAnOptionalPrivateKey()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString keyPath = temporary.filePath(QStringLiteral("dedicated_key"));
    QFile key(keyPath);
    QVERIFY(key.open(QIODevice::WriteOnly));
    key.close();

    rfm::app::ConnectionDialog dialog;
    dialog.findChild<QPushButton*>(QStringLiteral("privateKeyBrowseButton"))->click();
    auto* const fileDialog = dialog.findChild<QFileDialog*>(QStringLiteral("privateKeyFileDialog"));
    QVERIFY(fileDialog != nullptr);
    QVERIFY(QMetaObject::invokeMethod(fileDialog, "fileSelected", Qt::DirectConnection,
                                      Q_ARG(QString, keyPath)));
    QCOMPARE(dialog.findChild<QLineEdit*>(QStringLiteral("privateKeyPathEdit"))->text(), keyPath);
    fileDialog->reject();
}

void ConnectionDialogTest::switchesAuthenticationModeAndControls()
{
    rfm::app::ConnectionDialog dialog;
    auto* const keyMode = dialog.findChild<QRadioButton*>(QStringLiteral("keyOrAgentRadio"));
    auto* const passwordMode = dialog.findChild<QRadioButton*>(QStringLiteral("passwordOnlyRadio"));
    auto* const privateKey = dialog.findChild<QLineEdit*>(QStringLiteral("privateKeyPathEdit"));
    auto* const browse = dialog.findChild<QPushButton*>(QStringLiteral("privateKeyBrowseButton"));
    auto* const fallback =
        dialog.findChild<QCheckBox*>(QStringLiteral("passwordAuthenticationCheck"));
    QVERIFY(keyMode != nullptr);
    QVERIFY(passwordMode != nullptr);
    QVERIFY(keyMode->isChecked());
    QVERIFY(privateKey->isEnabled());
    QVERIFY(browse->isEnabled());
    QVERIFY(fallback->isEnabled());

    privateKey->setText(QStringLiteral("~/.ssh/custom"));
    fallback->setChecked(true);
    passwordMode->click();
    QVERIFY(passwordMode->isChecked());
    QVERIFY(!privateKey->isEnabled());
    QVERIFY(!browse->isEnabled());
    QVERIFY(!fallback->isEnabled());
    const auto passwordOnly = dialog.profile();
    QCOMPARE(passwordOnly.authenticationMode, rfm::core::AuthenticationMode::PasswordOnly);
    QVERIFY(passwordOnly.privateKeyPath.isEmpty());
    QVERIFY(!passwordOnly.allowPasswordAuthentication);

    keyMode->click();
    QVERIFY(privateKey->isEnabled());
    QVERIFY(browse->isEnabled());
    QVERIFY(fallback->isEnabled());
    QCOMPARE(dialog.profile().authenticationMode, rfm::core::AuthenticationMode::KeyOrAgent);
}

void ConnectionDialogTest::requestDoesNotCloseDialogAndShowsConnectingState()
{
    rfm::app::ConnectionDialog dialog;
    dialog.show();
    dialog.findChild<QLineEdit*>(QStringLiteral("serverHostEdit"))
        ->setText(QStringLiteral("server.example.test"));
    dialog.findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"))
        ->setText(QStringLiteral("alice"));
    auto* const buttons = dialog.findChild<QDialogButtonBox*>();
    auto* const save = dialog.findChild<QCheckBox*>(QStringLiteral("saveServerCheck"));
    save->setChecked(true);
    QSignalSpy requested(&dialog, &rfm::app::ConnectionDialog::connectionRequested);
    buttons->button(QDialogButtonBox::Ok)->click();

    QCOMPARE(requested.size(), 1);
    QVERIFY(dialog.isVisible());
    QCOMPARE(dialog.state(), rfm::app::ConnectionDialog::State::Connecting);
    QVERIFY(!dialog.findChild<QLineEdit*>(QStringLiteral("serverHostEdit"))->isEnabled());
    QVERIFY(!dialog.findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"))->isEnabled());
    QVERIFY(!dialog.findChild<QSpinBox*>(QStringLiteral("serverPortSpin"))->isEnabled());
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(!buttons->button(QDialogButtonBox::Cancel)->isEnabled());
    QVERIFY(dialog.findChild<QProgressBar*>(QStringLiteral("connectionActivityIndicator"))
                ->isVisible());
}

void ConnectionDialogTest::errorRestoresFieldsAndPreservesValues()
{
    rfm::app::ConnectionDialog dialog;
    dialog.show();
    auto* const host = dialog.findChild<QLineEdit*>(QStringLiteral("serverHostEdit"));
    auto* const user = dialog.findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"));
    auto* const port = dialog.findChild<QSpinBox*>(QStringLiteral("serverPortSpin"));
    auto* const buttons = dialog.findChild<QDialogButtonBox*>();
    host->setText(QStringLiteral("wrong.example.test"));
    user->setText(QStringLiteral("alice"));
    port->setValue(2222);
    buttons->button(QDialogButtonBox::Ok)->click();

    dialog.showConnectionError(QStringLiteral("Connection refused"));
    QCOMPARE(dialog.state(), rfm::app::ConnectionDialog::State::Error);
    QVERIFY(host->isEnabled());
    QVERIFY(user->isEnabled());
    QVERIFY(port->isEnabled());
    QCOMPARE(host->text(), QStringLiteral("wrong.example.test"));
    QCOMPARE(port->value(), 2222);
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("connectionStatusLabel"))->text(),
             QStringLiteral("Connection refused"));
}

void ConnectionDialogTest::successAcceptsOnlyAnActiveAttempt()
{
    rfm::app::ConnectionDialog dialog;
    dialog.connectionSucceeded();
    QCOMPARE(dialog.result(), 0);

    dialog.findChild<QLineEdit*>(QStringLiteral("serverHostEdit"))
        ->setText(QStringLiteral("server.example.test"));
    dialog.findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit"))
        ->setText(QStringLiteral("alice"));
    dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    dialog.connectionSucceeded();
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
}

QTEST_MAIN(ConnectionDialogTest)

#include "test_connection_dialog.moc"
