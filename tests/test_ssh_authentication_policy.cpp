#include "remotefilemanager/ssh/SshAuthenticationPolicy.hpp"

#include <QDir>
#include <QFile>
#include <QTest>

class SshAuthenticationPolicyTest final : public QObject
{
    Q_OBJECT

  private slots:
    void choosesTheNextStep_data();
    void choosesTheNextStep();
    void resolvesPrivateKeyPathsPortably();
    void choosesPasswordPromptReason();
    void explicitIdentityIsConfiguredBeforeAuthentication();
    void hostKeyVerificationStillPrecedesAuthentication();
};

void SshAuthenticationPolicyTest::choosesTheNextStep_data()
{
    using Result = rfm::ssh::AuthenticationResult;
    using Step = rfm::ssh::AuthenticationNextStep;
    QTest::addColumn<Result>("result");
    QTest::addColumn<bool>("allowed");
    QTest::addColumn<bool>("offered");
    QTest::addColumn<Step>("expected");

    QTest::newRow("key succeeds") << Result::Success << true << true << Step::OpenSession;
    QTest::newRow("key absent and password offered")
        << Result::Denied << true << true << Step::RequestPassword;
    QTest::newRow("second factor password")
        << Result::Partial << true << true << Step::RequestPassword;
    QTest::newRow("password disabled") << Result::Denied << false << true << Step::Reject;
    QTest::newRow("password unavailable") << Result::Denied << true << false << Step::Reject;
    QTest::newRow("transport error is not a password prompt")
        << Result::Error << true << true << Step::Fail;
}

void SshAuthenticationPolicyTest::hostKeyVerificationStillPrecedesAuthentication()
{
    QFile implementation(QStringLiteral(RFM_SOURCE_DIR "/src/ssh/SshSession.cpp"));
    QVERIFY(implementation.open(QIODevice::ReadOnly));
    const QByteArray source = implementation.readAll();
    const qsizetype verification = source.indexOf("ssh_session_is_known_server");
    const qsizetype changedKey = source.indexOf("SSH_KNOWN_HOSTS_CHANGED");
    const qsizetype explicitConfirmation = source.indexOf("hostKeyConfirmationRequired");
    const qsizetype automaticKeys = source.indexOf("ssh_userauth_publickey_auto");
    QVERIFY(verification >= 0);
    QVERIFY(changedKey > verification);
    QVERIFY(explicitConfirmation > verification);
    QVERIFY(automaticKeys > explicitConfirmation);
    QVERIFY(source.contains("ssh_session_update_known_hosts"));
}

void SshAuthenticationPolicyTest::resolvesPrivateKeyPathsPortably()
{
    QCOMPARE(rfm::ssh::SshAuthenticationPolicy::resolvePrivateKeyPath({}), QString{});
    QCOMPARE(rfm::ssh::SshAuthenticationPolicy::resolvePrivateKeyPath(QStringLiteral("~")),
             QDir::toNativeSeparators(QDir::homePath()));
    QCOMPARE(
        rfm::ssh::SshAuthenticationPolicy::resolvePrivateKeyPath(
            QStringLiteral("~/.ssh/rfm_windows_server")),
        QDir::toNativeSeparators(
            QDir(QDir::homePath()).absoluteFilePath(QStringLiteral(".ssh/rfm_windows_server"))));
    QVERIFY(QDir::isAbsolutePath(QDir::fromNativeSeparators(
        rfm::ssh::SshAuthenticationPolicy::resolvePrivateKeyPath(QStringLiteral("relative_key")))));
}

void SshAuthenticationPolicyTest::explicitIdentityIsConfiguredBeforeAuthentication()
{
    QFile implementation(QStringLiteral(RFM_SOURCE_DIR "/src/ssh/SshSession.cpp"));
    QVERIFY(implementation.open(QIODevice::ReadOnly));
    const QByteArray source = implementation.readAll();
    const qsizetype identityOption = source.indexOf("SSH_OPTIONS_IDENTITY");
    const qsizetype connection = source.indexOf("ssh_connect");
    const qsizetype knownHosts = source.indexOf("ssh_session_is_known_server");
    const qsizetype privateKeyCheck = source.indexOf("ssh_pki_import_privkey_file");
    const qsizetype automaticKeys = source.indexOf("ssh_userauth_publickey_auto");
    QVERIFY(identityOption >= 0);
    QVERIFY(connection > identityOption);
    QVERIFY(knownHosts > connection);
    QVERIFY(privateKeyCheck > knownHosts);
    QVERIFY(automaticKeys > privateKeyCheck);
    QVERIFY(source.contains("ssh_userauth_publickey_auto(m_impl->session, nullptr, \"\")"));
    QVERIFY(source.contains("RFM cannot use a passphrase-protected explicit key"));
    QVERIFY(source.contains("RFM does not request or"));
}

void SshAuthenticationPolicyTest::choosesPasswordPromptReason()
{
    using Mode = rfm::core::AuthenticationMode;
    using Reason = rfm::ssh::PasswordAuthenticationReason;
    QCOMPARE(rfm::ssh::SshAuthenticationPolicy::passwordPromptReason(
                 Mode::KeyOrAgent, rfm::ssh::AuthenticationResult::Denied, true),
             Reason::ExplicitKeyFailed);
    QCOMPARE(rfm::ssh::SshAuthenticationPolicy::passwordPromptReason(
                 Mode::KeyOrAgent, rfm::ssh::AuthenticationResult::Denied, false),
             Reason::KeyOrAgentFailed);
    QCOMPARE(rfm::ssh::SshAuthenticationPolicy::passwordPromptReason(
                 Mode::KeyOrAgent, rfm::ssh::AuthenticationResult::Partial, true),
             Reason::AdditionalPasswordRequired);
    QCOMPARE(rfm::ssh::SshAuthenticationPolicy::passwordPromptReason(
                 Mode::PasswordOnly, rfm::ssh::AuthenticationResult::Error, false),
             Reason::PasswordOnly);

    QFile implementation(QStringLiteral(RFM_SOURCE_DIR "/src/ssh/SshSession.cpp"));
    QVERIFY(implementation.open(QIODevice::ReadOnly));
    const QByteArray source = implementation.readAll();
    const qsizetype passwordOnly =
        source.indexOf("authenticationMode == rfm::core::AuthenticationMode::PasswordOnly");
    const qsizetype automaticKeys = source.indexOf("ssh_userauth_publickey_auto");
    QVERIFY(passwordOnly >= 0);
    QVERIFY(automaticKeys > passwordOnly);
    QVERIFY(source.contains("The server does not offer password authentication."));
}

void SshAuthenticationPolicyTest::choosesTheNextStep()
{
    QFETCH(rfm::ssh::AuthenticationResult, result);
    QFETCH(bool, allowed);
    QFETCH(bool, offered);
    QFETCH(rfm::ssh::AuthenticationNextStep, expected);
    QCOMPARE(rfm::ssh::SshAuthenticationPolicy::afterPasswordless(result, allowed, offered),
             expected);
}

QTEST_APPLESS_MAIN(SshAuthenticationPolicyTest)

#include "test_ssh_authentication_policy.moc"
