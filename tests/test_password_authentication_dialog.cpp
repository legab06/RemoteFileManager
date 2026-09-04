#include "remotefilemanager/app/PasswordAuthenticationDialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

class PasswordAuthenticationDialogTest final : public QObject
{
    Q_OBJECT

  private slots:
    void showsOnlyIdentityAndPassword();
    void showsContextualAuthenticationMessage();
    void submitsAndWipesAnEphemeralPassword();
    void rejectionAllowsRetryAndCancelDoesNotSubmit();
};

void PasswordAuthenticationDialogTest::showsOnlyIdentityAndPassword()
{
    const rfm::core::ConnectionProfile profile{
        QStringLiteral("Home NAS"), QStringLiteral("nas.example.test"),
        QStringLiteral("alice"),    2222,
        QStringLiteral("nas-id"),   true};
    rfm::app::PasswordAuthenticationDialog dialog(profile);

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("authenticationServerNameLabel"))->text(),
             QStringLiteral("Home NAS"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("authenticationServerIdentityLabel"))->text(),
             QStringLiteral("alice@nas.example.test"));
    QVERIFY(dialog.findChild<QLineEdit*>(QStringLiteral("authenticationPasswordEdit")) != nullptr);
    QVERIFY(dialog.findChild<QLineEdit*>(QStringLiteral("serverHostEdit")) == nullptr);
    QVERIFY(dialog.findChild<QLineEdit*>(QStringLiteral("serverUsernameEdit")) == nullptr);
}

void PasswordAuthenticationDialogTest::submitsAndWipesAnEphemeralPassword()
{
    rfm::app::PasswordAuthenticationDialog dialog(
        {{}, QStringLiteral("host.test"), QStringLiteral("alice"), 22});
    auto* const password =
        dialog.findChild<QLineEdit*>(QStringLiteral("authenticationPasswordEdit"));
    auto* const buttons =
        dialog.findChild<QDialogButtonBox*>(QStringLiteral("authenticationButtons"));
    QSignalSpy requested(&dialog, &rfm::app::PasswordAuthenticationDialog::authenticationRequested);
    password->setText(QStringLiteral("ephemeral secret"));
    buttons->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(requested.size(), 1);
    QVERIFY(dialog.isAuthenticating());

    auto secret = dialog.takePassword();
    QVERIFY(!secret.isEmpty());
    QVERIFY(password->text().isEmpty());
    secret.clear();
}

void PasswordAuthenticationDialogTest::showsContextualAuthenticationMessage()
{
    rfm::app::PasswordAuthenticationDialog dialog(
        {{}, QStringLiteral("host.test"), QStringLiteral("alice"), 22});
    dialog.setAuthenticationMessage(
        QStringLiteral("SSH key or agent authentication failed. Enter your password to continue."));
    QCOMPARE(
        dialog.findChild<QLabel*>(QStringLiteral("authenticationPromptLabel"))->text(),
        QStringLiteral("SSH key or agent authentication failed. Enter your password to continue."));
}

void PasswordAuthenticationDialogTest::rejectionAllowsRetryAndCancelDoesNotSubmit()
{
    rfm::app::PasswordAuthenticationDialog dialog(
        {{}, QStringLiteral("host.test"), QStringLiteral("alice"), 22});
    auto* const password =
        dialog.findChild<QLineEdit*>(QStringLiteral("authenticationPasswordEdit"));
    auto* const buttons =
        dialog.findChild<QDialogButtonBox*>(QStringLiteral("authenticationButtons"));
    QSignalSpy requested(&dialog, &rfm::app::PasswordAuthenticationDialog::authenticationRequested);
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    password->setText(QStringLiteral("wrong"));
    buttons->button(QDialogButtonBox::Ok)->click();
    dialog.showAuthenticationError(QStringLiteral("Incorrect password. Please try again."));
    QVERIFY(!dialog.isAuthenticating());
    QVERIFY(password->isEnabled());
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("authenticationStatusLabel"))->text(),
             QStringLiteral("Incorrect password. Please try again."));

    buttons->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(rejected.size(), 1);
    QVERIFY(password->text().isEmpty());
}

QTEST_MAIN(PasswordAuthenticationDialogTest)

#include "test_password_authentication_dialog.moc"
