#include "remotefilemanager/ssh/RemoteVolumeService.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include <QSignalSpy>
#include <QTest>

namespace
{

rfm::core::VolumeOperationRequest
requestFor(rfm::core::VolumeOperation operation, QString device = QStringLiteral("/dev/sdb1"),
           QString mountPoint = QStringLiteral("/mnt/usb"),
           rfm::core::StorageKind kind = rfm::core::StorageKind::External,
           QStringList knownMountPoints = {})
{
    if (operation == rfm::core::VolumeOperation::Unmount && knownMountPoints.isEmpty()) {
        knownMountPoints.push_back(mountPoint);
    }
    return {17,
            operation,
            {std::move(device), std::move(mountPoint), kind, std::move(knownMountPoints)}};
}

rfm::ssh::RemoteLinuxVolumeCapabilities allCapabilities() { return {true, true, true, true, true}; }

} // namespace

class RemoteVolumeServiceTest final : public QObject
{
    Q_OBJECT

  private slots:
    void parsesCapabilitiesAndUsesOnlyFixedProbe();
    void buildsAndAppliesLateUnmountTopologyProbe();
    void choosesUdisksctlAndFallbacks();
    void targetsSelectedMountPointForMultipleAttachments();
    void rejectsUnsafeOrInconsistentMountPoints_data();
    void rejectsUnsafeOrInconsistentMountPoints();
    void rejectsUnavailableToolsAndProtectedVolumes();
    void rejectsUnsafeDevicePaths_data();
    void rejectsUnsafeDevicePaths();
    void neverUsesPresentationMetadataOrPrivilegeEscalation();
    void buildsConstrainedInteractiveCommands();
    void schedulesRemotePollingCooperatively();
    void mapsStructuredFailures_data();
    void mapsStructuredFailures();
    void mapsTimeoutToStructuredFailure();
    void reportsUnavailableSessionCapabilities();
    void invalidatesCapabilitiesBetweenSessions();
    void parsesBoundedPolkitConversation();
    void detectsPolkitAuthenticationRetry();
    void preservesPolkitAuthenticationSuccess();
    void preservesStructuredInteractiveErrors_data();
    void preservesStructuredInteractiveErrors();
    void sessionLossBeforeOperationReturnsStructuredError();
};

void RemoteVolumeServiceTest::parsesCapabilitiesAndUsesOnlyFixedProbe()
{
    const auto capabilities = rfm::ssh::RemoteLinuxVolumeService::parseCapabilities(
        QByteArrayLiteral("lsblk\nudisksctl\numount\n"));
    QVERIFY(capabilities.known);
    QVERIFY(capabilities.lsblk);
    QVERIFY(capabilities.udisksctl);
    QVERIFY(!capabilities.mount);
    QVERIFY(capabilities.umount);
    const QString probe = rfm::ssh::RemoteLinuxVolumeService::capabilityProbeCommand();
    QVERIFY(probe.contains(QStringLiteral("lsblk udisksctl mount umount")));
    QVERIFY(!probe.contains(QStringLiteral("sudo")));
}

void RemoteVolumeServiceTest::buildsAndAppliesLateUnmountTopologyProbe()
{
    const auto initial = requestFor(rfm::core::VolumeOperation::Unmount);
    rfm::core::VolumeOperationResult immediate;
    const QString probe = *rfm::ssh::RemoteLinuxVolumeService::unmountTopologyCommand(
        initial, allCapabilities(), &immediate);
    QCOMPARE(probe, QStringLiteral("LC_ALL=C lsblk --json --paths --output PATH,MOUNTPOINTS -- "
                                   "'/dev/sdb1'"));
    QVERIFY(!probe.contains(QStringLiteral("sudo")));

    const auto inconsistentInitial =
        requestFor(rfm::core::VolumeOperation::Unmount, QStringLiteral("/dev/sdb1"),
                   QStringLiteral("/mnt/usb"), rfm::core::StorageKind::External,
                   {QStringLiteral("/mnt/usb"), QString{}});
    QVERIFY(!rfm::ssh::RemoteLinuxVolumeService::unmountTopologyCommand(
                 inconsistentInitial, allCapabilities(), &immediate)
                 .has_value());
    QCOMPARE(immediate.error, rfm::core::VolumeOperationError::DeviceNotFound);

    const rfm::core::VolumeCommandResult unchanged{
        true,
        false,
        false,
        0,
        QStringLiteral(
            "{\"blockdevices\":[{\"path\":\"/dev/sdb1\",\"mountpoints\":[\"/mnt/usb\"]}]}"),
        {},
        false};
    const auto revalidatedSingle =
        rfm::core::revalidatedVolumeUnmountRequest(initial, unchanged, &immediate);
    QVERIFY(revalidatedSingle.has_value());
    QCOMPARE(*rfm::ssh::RemoteLinuxVolumeService::operationCommand(*revalidatedSingle,
                                                                   allCapabilities(), &immediate),
             QStringLiteral("LC_ALL=C udisksctl unmount -b /dev/sdb1 --no-user-interaction"));

    rfm::core::VolumeCommandResult changed = unchanged;
    changed.standardOutput =
        QStringLiteral("{\"blockdevices\":[{\"path\":\"/dev/sdb1\",\"mountpoints\":[\"/mnt/usb\","
                       "\"/mnt/Other's disk;$(id)\"]}]}");
    const auto revalidatedMultiple =
        rfm::core::revalidatedVolumeUnmountRequest(initial, changed, &immediate);
    QVERIFY(revalidatedMultiple.has_value());
    const QString targeted = *rfm::ssh::RemoteLinuxVolumeService::operationCommand(
        *revalidatedMultiple, allCapabilities(), &immediate);
    QCOMPARE(targeted, QStringLiteral("LC_ALL=C umount -- '/mnt/usb'"));
    QVERIFY(!targeted.contains(QStringLiteral("/dev/sdb1")));

    rfm::core::VolumeCommandResult rootAppeared = unchanged;
    rootAppeared.standardOutput =
        QStringLiteral("{\"blockdevices\":[{\"path\":\"/dev/sdb1\",\"mountpoints\":[\"/\","
                       "\"/mnt/usb\"]}]}");
    const auto revalidatedRoot =
        rfm::core::revalidatedVolumeUnmountRequest(initial, rootAppeared, &immediate);
    QVERIFY(revalidatedRoot.has_value());
    QCOMPARE(*rfm::ssh::RemoteLinuxVolumeService::operationCommand(*revalidatedRoot,
                                                                   allCapabilities(), &immediate),
             QStringLiteral("LC_ALL=C umount -- '/mnt/usb'"));

    rfm::core::VolumeCommandResult disappeared = unchanged;
    disappeared.standardOutput = QStringLiteral(
        "{\"blockdevices\":[{\"path\":\"/dev/sdb1\",\"mountpoints\":[\"/mnt/other\"]}]}");
    QVERIFY(
        !rfm::core::revalidatedVolumeUnmountRequest(initial, disappeared, &immediate).has_value());
    QCOMPARE(immediate.error, rfm::core::VolumeOperationError::DeviceNotFound);

    rfm::core::VolumeCommandResult failed = unchanged;
    failed.exitCode = 1;
    failed.standardOutput.clear();
    failed.standardError = QStringLiteral("lsblk: /dev/sdb1: not found");
    QVERIFY(!rfm::core::revalidatedVolumeUnmountRequest(initial, failed, &immediate).has_value());
    QCOMPARE(immediate.error, rfm::core::VolumeOperationError::DeviceNotFound);
}

void RemoteVolumeServiceTest::buildsConstrainedInteractiveCommands()
{
    for (const rfm::core::VolumeOperation operation :
         {rfm::core::VolumeOperation::Mount, rfm::core::VolumeOperation::Unmount}) {
        const QString command = *rfm::ssh::RemoteLinuxVolumeService::interactiveOperationCommand(
            requestFor(operation), allCapabilities());
        const QString verb = operation == rfm::core::VolumeOperation::Mount
                                 ? QStringLiteral("mount")
                                 : QStringLiteral("unmount");
        QCOMPARE(command, QStringLiteral("LC_ALL=C udisksctl %1 -b /dev/sdb1").arg(verb));
        QVERIFY(!command.contains(QStringLiteral("--no-user-interaction")));
        QVERIFY(!command.contains(QStringLiteral("sudo")));
        QVERIFY(!command.contains(QStringLiteral("su ")));
        QVERIFY(!command.contains(QStringLiteral("password"), Qt::CaseInsensitive));
    }
}

void RemoteVolumeServiceTest::schedulesRemotePollingCooperatively()
{
    rfm::ssh::SshCommandPollScheduler scheduler;
    QCOMPARE(rfm::ssh::SshCommandPollScheduler::commandTimeoutMilliseconds, qint64{60'000});
    QVERIFY(!scheduler.pending());

    const auto idle = scheduler.schedule(false);
    QVERIFY(idle.has_value());
    QCOMPARE(idle->delayMilliseconds, rfm::ssh::SshCommandPollScheduler::idleDelayMilliseconds);
    QVERIFY(idle->delayMilliseconds > 0);
    QVERIFY(scheduler.pending());
    QVERIFY(!scheduler.schedule(false).has_value());
    QVERIFY(!scheduler.schedule(true).has_value());

    QVERIFY(scheduler.consume(idle->generation));
    QVERIFY(!scheduler.pending());
    const auto active = scheduler.schedule(true);
    QVERIFY(active.has_value());
    QCOMPARE(active->delayMilliseconds, 0);

    scheduler.cancel();
    QVERIFY(!scheduler.pending());
    QVERIFY(!scheduler.consume(active->generation));
    const auto replacement = scheduler.schedule(false);
    QVERIFY(replacement.has_value());
    QVERIFY(replacement->generation != active->generation);
    QVERIFY(!scheduler.consume(active->generation));
    QVERIFY(scheduler.consume(replacement->generation));
}

void RemoteVolumeServiceTest::choosesUdisksctlAndFallbacks()
{
    rfm::core::VolumeOperationResult immediate;
    QCOMPARE(*rfm::ssh::RemoteLinuxVolumeService::operationCommand(
                 requestFor(rfm::core::VolumeOperation::Mount), allCapabilities(), &immediate),
             QStringLiteral("LC_ALL=C udisksctl mount -b /dev/sdb1 --no-user-interaction"));
    QCOMPARE(*rfm::ssh::RemoteLinuxVolumeService::operationCommand(
                 requestFor(rfm::core::VolumeOperation::Unmount), allCapabilities(), &immediate),
             QStringLiteral("LC_ALL=C udisksctl unmount -b /dev/sdb1 --no-user-interaction"));

    rfm::ssh::RemoteLinuxVolumeCapabilities fallback{true, true, false, true, true};
    QCOMPARE(*rfm::ssh::RemoteLinuxVolumeService::operationCommand(
                 requestFor(rfm::core::VolumeOperation::Mount), fallback, &immediate),
             QStringLiteral("LC_ALL=C mount -- /dev/sdb1"));
    QCOMPARE(*rfm::ssh::RemoteLinuxVolumeService::operationCommand(
                 requestFor(rfm::core::VolumeOperation::Unmount), fallback, &immediate),
             QStringLiteral("LC_ALL=C umount -- '/mnt/usb'"));
}

void RemoteVolumeServiceTest::targetsSelectedMountPointForMultipleAttachments()
{
    rfm::core::VolumeOperationResult immediate;
    const auto request =
        requestFor(rfm::core::VolumeOperation::Unmount, QStringLiteral("/dev/sdb1"),
                   QStringLiteral("/mnt/My Backup's disk;$(id)"), rfm::core::StorageKind::External,
                   {QStringLiteral("/mnt/My Backup's disk;$(id)"), QStringLiteral("/mnt/other")});

    const QString command = *rfm::ssh::RemoteLinuxVolumeService::operationCommand(
        request, allCapabilities(), &immediate);
    QCOMPARE(command, QStringLiteral("LC_ALL=C umount -- '/mnt/My Backup'\\''s disk;$(id)'"));
    QVERIFY(!command.contains(QStringLiteral("/dev/sdb1")));
    QVERIFY(!command.contains(QStringLiteral("--all-targets")));
    QVERIFY(!command.contains(QStringLiteral("sudo")));
    QVERIFY(!command.contains(QStringLiteral("pkexec")));

    const auto rootSibling =
        requestFor(rfm::core::VolumeOperation::Unmount, QStringLiteral("/dev/sda2"),
                   QStringLiteral("/mnt/data"), rfm::core::StorageKind::External,
                   {QStringLiteral("/"), QStringLiteral("/mnt/data")});
    QCOMPARE(*rfm::ssh::RemoteLinuxVolumeService::operationCommand(rootSibling, allCapabilities(),
                                                                   &immediate),
             QStringLiteral("LC_ALL=C umount -- '/mnt/data'"));

    const rfm::ssh::RemoteLinuxVolumeCapabilities udisksOnly{true, true, true, false, false};
    QVERIFY(!rfm::ssh::RemoteLinuxVolumeService::operationCommand(request, udisksOnly, &immediate)
                 .has_value());
    QCOMPARE(immediate.error, rfm::core::VolumeOperationError::ToolUnavailable);

    QVERIFY(!rfm::ssh::RemoteLinuxVolumeService::interactiveOperationCommand(
                 request, allCapabilities(), &immediate)
                 .has_value());
    QCOMPARE(immediate.error, rfm::core::VolumeOperationError::PermissionDenied);

    const auto authorizationResult = rfm::ssh::RemoteLinuxVolumeService::operationResult(
        request, {true,
                  false,
                  false,
                  1,
                  {},
                  QStringLiteral("NotAuthorizedCanObtain: Authentication is required"),
                  false});
    QCOMPARE(authorizationResult.error, rfm::core::VolumeOperationError::PermissionDenied);
}

void RemoteVolumeServiceTest::rejectsUnsafeOrInconsistentMountPoints_data()
{
    QTest::addColumn<QString>("mountPoint");
    QTest::addColumn<QStringList>("knownMountPoints");

    QTest::newRow("empty") << QString{} << QStringList{};
    QTest::newRow("root") << QStringLiteral("/") << QStringList{QStringLiteral("/")};
    QTest::newRow("relative") << QStringLiteral("mnt/data")
                              << QStringList{QStringLiteral("mnt/data")};
    QTest::newRow("not-normalized") << QStringLiteral("/mnt/data/../other")
                                    << QStringList{QStringLiteral("/mnt/data/../other")};
    QTest::newRow("line-break") << QStringLiteral("/mnt/data\nother")
                                << QStringList{QStringLiteral("/mnt/data\nother")};
    QTest::newRow("missing-from-snapshot")
        << QStringLiteral("/mnt/data") << QStringList{QStringLiteral("/mnt/other")};
}

void RemoteVolumeServiceTest::rejectsUnsafeOrInconsistentMountPoints()
{
    QFETCH(QString, mountPoint);
    QFETCH(QStringList, knownMountPoints);
    rfm::core::VolumeOperationResult immediate;
    const rfm::core::VolumeOperationRequest request{17,
                                                    rfm::core::VolumeOperation::Unmount,
                                                    {QStringLiteral("/dev/sdb1"), mountPoint,
                                                     rfm::core::StorageKind::External,
                                                     knownMountPoints}};

    QVERIFY(!rfm::ssh::RemoteLinuxVolumeService::operationCommand(request, allCapabilities(),
                                                                  &immediate)
                 .has_value());
    QVERIFY(immediate.error == rfm::core::VolumeOperationError::DeviceNotFound ||
            immediate.error == rfm::core::VolumeOperationError::NotSupported);
}

void RemoteVolumeServiceTest::rejectsUnavailableToolsAndProtectedVolumes()
{
    rfm::core::VolumeOperationResult immediate;
    const rfm::ssh::RemoteLinuxVolumeCapabilities none{true, true, false, false, false};
    QVERIFY(!rfm::ssh::RemoteLinuxVolumeService::operationCommand(
                 requestFor(rfm::core::VolumeOperation::Mount), none, &immediate)
                 .has_value());
    QCOMPARE(immediate.error, rfm::core::VolumeOperationError::ToolUnavailable);

    QVERIFY(!rfm::ssh::RemoteLinuxVolumeService::operationCommand(
                 requestFor(rfm::core::VolumeOperation::Unmount, QStringLiteral("/dev/sda1"),
                            QStringLiteral("/"), rfm::core::StorageKind::System),
                 allCapabilities(), &immediate)
                 .has_value());
    QCOMPARE(immediate.error, rfm::core::VolumeOperationError::NotSupported);

    QVERIFY(!rfm::ssh::RemoteLinuxVolumeService::operationCommand(
                 requestFor(rfm::core::VolumeOperation::Unmount, QStringLiteral("/dev/sda2"),
                            QStringLiteral("/boot"), rfm::core::StorageKind::System),
                 allCapabilities(), &immediate)
                 .has_value());
    QCOMPARE(immediate.error, rfm::core::VolumeOperationError::NotSupported);
}

void RemoteVolumeServiceTest::rejectsUnsafeDevicePaths_data()
{
    QTest::addColumn<QString>("device");
    QTest::newRow("semicolon") << QStringLiteral("/dev/sdb1;touch /tmp/pwned");
    QTest::newRow("substitution") << QStringLiteral("/dev/$(id)");
    QTest::newRow("space") << QStringLiteral("/dev/disk/by-label/My Disk");
    QTest::newRow("quote") << QStringLiteral("/dev/sdb1'");
    QTest::newRow("traversal") << QStringLiteral("/dev/disk/../../tmp/x");
    QTest::newRow("not-dev") << QStringLiteral("/tmp/device");
}

void RemoteVolumeServiceTest::rejectsUnsafeDevicePaths()
{
    QFETCH(QString, device);
    rfm::core::VolumeOperationResult immediate;
    QVERIFY(
        !rfm::ssh::RemoteLinuxVolumeService::operationCommand(
             requestFor(rfm::core::VolumeOperation::Mount, device), allCapabilities(), &immediate)
             .has_value());
    QCOMPARE(immediate.error, rfm::core::VolumeOperationError::DeviceNotFound);
}

void RemoteVolumeServiceTest::neverUsesPresentationMetadataOrPrivilegeEscalation()
{
    for (const rfm::core::VolumeOperation operation :
         {rfm::core::VolumeOperation::Mount, rfm::core::VolumeOperation::Unmount}) {
        const auto request = requestFor(operation);
        const QString command =
            *rfm::ssh::RemoteLinuxVolumeService::operationCommand(request, allCapabilities());
        QVERIFY(command.contains(request.target.device));
        QCOMPARE(command.count(QStringLiteral("--no-user-interaction")), 1);
        QVERIFY(!command.contains(QStringLiteral("Backup; rm -rf")));
        QVERIFY(!command.contains(QStringLiteral("sudo")));
        QVERIFY(!command.contains(QStringLiteral("su ")));
        QVERIFY(!command.contains(QStringLiteral("password"), Qt::CaseInsensitive));
        QVERIFY(!command.contains(QStringLiteral("--interactive")));
    }
}

void RemoteVolumeServiceTest::mapsStructuredFailures_data()
{
    QTest::addColumn<QString>("diagnostic");
    QTest::addColumn<rfm::core::VolumeOperationError>("error");
    QTest::newRow("permission") << QStringLiteral("permission denied")
                                << rfm::core::VolumeOperationError::PermissionDenied;
    QTest::newRow("polkit-not-authorized")
        << QStringLiteral("Error mounting /dev/sdb1: GDBus.Error:org.freedesktop.UDisks2.Error."
                          "NotAuthorized: Not authorized to perform operation")
        << rfm::core::VolumeOperationError::PermissionDenied;
    QTest::newRow("polkit-authentication-required")
        << QStringLiteral("Authentication is required to mount TOSHIBA (/dev/sdb1)")
        << rfm::core::VolumeOperationError::AuthenticationRequired;
    QTest::newRow("polkit-can-obtain")
        << QStringLiteral("GDBus.Error:org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain: "
                          "Not authorized to perform operation")
        << rfm::core::VolumeOperationError::AuthenticationRequired;
    QTest::newRow("authentication-failed") << QStringLiteral("Authentication failed")
                                           << rfm::core::VolumeOperationError::AuthenticationFailed;
    QTest::newRow("busy") << QStringLiteral("target is busy")
                          << rfm::core::VolumeOperationError::VolumeBusy;
    QTest::newRow("device") << QStringLiteral("no such file")
                            << rfm::core::VolumeOperationError::DeviceNotFound;
    QTest::newRow("tool-unavailable") << QStringLiteral("udisksctl: command not found")
                                      << rfm::core::VolumeOperationError::ToolUnavailable;
}

void RemoteVolumeServiceTest::parsesBoundedPolkitConversation()
{
    rfm::ssh::RemotePolkitPromptParser parser;
    QCOMPARE(parser.timedOut(), rfm::ssh::RemotePolkitPromptEvent::TimedOutBeforePrompt);
    QCOMPARE(parser.consume(QByteArrayLiteral("==== AUTHENTICATING FOR org.freedesktop.")),
             rfm::ssh::RemotePolkitPromptEvent::None);
    QCOMPARE(parser.consume(QByteArrayLiteral("UDisks2 ====\r\nPass")),
             rfm::ssh::RemotePolkitPromptEvent::None);
    QCOMPARE(parser.consume(QByteArrayLiteral("word:\x1b[0m")),
             rfm::ssh::RemotePolkitPromptEvent::PasswordPrompt);
    QCOMPARE(parser.timedOut(), rfm::ssh::RemotePolkitPromptEvent::TimedOutAfterPrompt);
    parser.passwordSent();
    QCOMPARE(parser.consume(QByteArrayLiteral("\r\n==== AUTHENTICATION COMPLETE ====\r\n")),
             rfm::ssh::RemotePolkitPromptEvent::None);
    QVERIFY(parser.authenticationCompleted());

    parser.clear();
    QCOMPARE(parser.consume(QByteArrayLiteral("Password:")),
             rfm::ssh::RemotePolkitPromptEvent::PasswordPrompt);
    parser.passwordSent();
    QCOMPARE(parser.consume(QByteArrayLiteral("Sorry, try again.\r\nPassword:")),
             rfm::ssh::RemotePolkitPromptEvent::AuthenticationFailed);
    QVERIFY(!parser.authenticationCompleted());
    QCOMPARE(parser.operationError(), rfm::core::VolumeOperationError::AuthenticationFailed);

    parser.clear();
    QCOMPARE(parser.consume(QByteArrayLiteral("GDBus.Error: Not authorized")),
             rfm::ssh::RemotePolkitPromptEvent::None);
    QVERIFY(parser.permissionDenied());
    QCOMPARE(parser.operationError(), rfm::core::VolumeOperationError::PermissionDenied);
    parser.clear();
    QCOMPARE(parser.consume(QByteArrayLiteral("target is busy")),
             rfm::ssh::RemotePolkitPromptEvent::None);
    QVERIFY(parser.volumeBusy());
    QCOMPARE(parser.operationError(), rfm::core::VolumeOperationError::VolumeBusy);
    parser.clear();
    QCOMPARE(parser.consume(QByteArrayLiteral("device is busy")),
             rfm::ssh::RemotePolkitPromptEvent::None);
    QCOMPARE(parser.operationError(), rfm::core::VolumeOperationError::VolumeBusy);
    parser.clear();
    QCOMPARE(parser.consume(QByteArrayLiteral("Error looking up object for device")),
             rfm::ssh::RemotePolkitPromptEvent::None);
    QVERIFY(parser.deviceNotFound());
    QCOMPARE(parser.operationError(), rfm::core::VolumeOperationError::DeviceNotFound);
}

void RemoteVolumeServiceTest::detectsPolkitAuthenticationRetry()
{
    rfm::ssh::RemotePolkitPromptParser parser;
    QCOMPARE(parser.consume(QByteArrayLiteral("Password:")),
             rfm::ssh::RemotePolkitPromptEvent::PasswordPrompt);
    QCOMPARE(parser.authenticationState(),
             rfm::ssh::RemotePolkitAuthenticationState::PasswordPromptReceived);
    parser.passwordSent();
    QCOMPARE(parser.authenticationState(),
             rfm::ssh::RemotePolkitAuthenticationState::WaitingForAuthenticationResult);
    QCOMPARE(parser.consume(QByteArrayLiteral("Password:")),
             rfm::ssh::RemotePolkitPromptEvent::AuthenticationFailed);
    QCOMPARE(parser.authenticationState(),
             rfm::ssh::RemotePolkitAuthenticationState::AuthenticationFailed);
    QCOMPARE(parser.operationError(), rfm::core::VolumeOperationError::AuthenticationFailed);

    parser.clear();
    QCOMPARE(parser.consume(QByteArrayLiteral("Pass")), rfm::ssh::RemotePolkitPromptEvent::None);
    QCOMPARE(parser.consume(QByteArrayLiteral("word:")),
             rfm::ssh::RemotePolkitPromptEvent::PasswordPrompt);
    parser.passwordSent();
    QCOMPARE(parser.consume(QByteArrayLiteral("Pa")), rfm::ssh::RemotePolkitPromptEvent::None);
    QCOMPARE(parser.consume(QByteArrayLiteral("ss\x1b[31mwo")),
             rfm::ssh::RemotePolkitPromptEvent::None);
    QCOMPARE(parser.consume(QByteArrayLiteral("rd\x1b[0m:")),
             rfm::ssh::RemotePolkitPromptEvent::AuthenticationFailed);

    parser.clear();
    QCOMPARE(parser.consume(QByteArrayLiteral("Password for alice:")),
             rfm::ssh::RemotePolkitPromptEvent::PasswordPrompt);
    parser.passwordSent();
    QCOMPARE(parser.consume(QByteArrayLiteral("Password for alice:")),
             rfm::ssh::RemotePolkitPromptEvent::AuthenticationFailed);

    parser.clear();
    QCOMPARE(parser.consume(QByteArrayLiteral("Password:")),
             rfm::ssh::RemotePolkitPromptEvent::PasswordPrompt);
    parser.passwordSent();
    QCOMPARE(parser.consume(QByteArrayLiteral("Sorry, try again.\r\nPass")),
             rfm::ssh::RemotePolkitPromptEvent::AuthenticationFailed);
}

void RemoteVolumeServiceTest::preservesPolkitAuthenticationSuccess()
{
    rfm::ssh::RemotePolkitPromptParser parser;
    QCOMPARE(parser.consume(QByteArrayLiteral("Password:")),
             rfm::ssh::RemotePolkitPromptEvent::PasswordPrompt);
    parser.passwordSent();
    QCOMPARE(parser.consume(QByteArrayLiteral("==== AUTHENTICATION COMPLETE ====")),
             rfm::ssh::RemotePolkitPromptEvent::None);
    QCOMPARE(parser.authenticationState(),
             rfm::ssh::RemotePolkitAuthenticationState::AuthenticationSucceeded);
    QVERIFY(parser.authenticationCompleted());
    QVERIFY(!parser.operationError().has_value());

    const auto result = rfm::ssh::RemoteLinuxVolumeService::interactiveOperationResult(
        requestFor(rfm::core::VolumeOperation::Mount), {true, false, false, 0, {}, {}, false},
        parser.operationError());
    QVERIFY(result.succeeded());
}

void RemoteVolumeServiceTest::preservesStructuredInteractiveErrors_data()
{
    QTest::addColumn<QByteArray>("output");
    QTest::addColumn<bool>("credentialsWereSent");
    QTest::addColumn<rfm::core::VolumeOperationError>("protocolError");
    QTest::newRow("authentication-failed-explicit")
        << QByteArrayLiteral("Sorry, try again.\r\nPassword:") << true
        << rfm::core::VolumeOperationError::AuthenticationFailed;
    QTest::newRow("authentication-retry") << QByteArrayLiteral("Password:") << true
                                          << rfm::core::VolumeOperationError::AuthenticationFailed;
    QTest::newRow("target-busy") << QByteArrayLiteral("target is busy") << false
                                 << rfm::core::VolumeOperationError::VolumeBusy;
    QTest::newRow("device-busy") << QByteArrayLiteral("device is busy") << false
                                 << rfm::core::VolumeOperationError::VolumeBusy;
    QTest::newRow("permission-denied") << QByteArrayLiteral("GDBus.Error: Not authorized") << false
                                       << rfm::core::VolumeOperationError::PermissionDenied;
    QTest::newRow("device-not-found")
        << QByteArrayLiteral("Error looking up object for device /dev/sdz1") << false
        << rfm::core::VolumeOperationError::DeviceNotFound;
}

void RemoteVolumeServiceTest::preservesStructuredInteractiveErrors()
{
    QFETCH(QByteArray, output);
    QFETCH(bool, credentialsWereSent);
    QFETCH(rfm::core::VolumeOperationError, protocolError);
    rfm::ssh::RemotePolkitPromptParser parser;
    if (credentialsWereSent) {
        QCOMPARE(parser.consume(QByteArrayLiteral("Password:")),
                 rfm::ssh::RemotePolkitPromptEvent::PasswordPrompt);
        parser.passwordSent();
    }
    static_cast<void>(parser.consume(output));
    QCOMPARE(parser.operationError(), protocolError);
    const auto result = rfm::ssh::RemoteLinuxVolumeService::interactiveOperationResult(
        requestFor(rfm::core::VolumeOperation::Unmount), {true, false, false, 1, {}, {}, false},
        parser.operationError());

    QCOMPARE(result.error, protocolError);
    QVERIFY(result.error != rfm::core::VolumeOperationError::SystemError);
    QVERIFY(result.technicalMessage.isEmpty());
}

void RemoteVolumeServiceTest::mapsStructuredFailures()
{
    QFETCH(QString, diagnostic);
    QFETCH(rfm::core::VolumeOperationError, error);
    const int exitCode = error == rfm::core::VolumeOperationError::ToolUnavailable ? 127 : 1;
    const auto result = rfm::ssh::RemoteLinuxVolumeService::operationResult(
        requestFor(rfm::core::VolumeOperation::Unmount),
        {true, false, false, exitCode, {}, diagnostic, false});
    QCOMPARE(result.error, error);
    QCOMPARE(result.technicalMessage, diagnostic);
}

void RemoteVolumeServiceTest::mapsTimeoutToStructuredFailure()
{
    const auto result = rfm::ssh::RemoteLinuxVolumeService::operationResult(
        requestFor(rfm::core::VolumeOperation::Mount),
        {true, true, false, -1, {}, QStringLiteral("bounded timeout fixture"), false});

    QCOMPARE(result.error, rfm::core::VolumeOperationError::SystemError);
    QCOMPARE(result.technicalMessage, QStringLiteral("bounded timeout fixture"));
}

void RemoteVolumeServiceTest::reportsUnavailableSessionCapabilities()
{
    rfm::core::VolumeOperationResult immediate;
    QVERIFY(!rfm::ssh::RemoteLinuxVolumeService::operationCommand(
                 requestFor(rfm::core::VolumeOperation::Mount), {}, &immediate)
                 .has_value());
    QCOMPARE(immediate.error, rfm::core::VolumeOperationError::ConnectionLost);
}

void RemoteVolumeServiceTest::invalidatesCapabilitiesBetweenSessions()
{
    rfm::ssh::RemoteLinuxVolumeCapabilityCache cache;
    QVERIFY(!cache.value().known);
    cache.update({true, true, true, false, true});
    QVERIFY(cache.value().known);
    QVERIFY(cache.value().lsblk);
    QVERIFY(cache.value().udisksctl);
    cache.reset();
    QVERIFY(!cache.value().known);
    QVERIFY(!cache.value().lsblk);
    QVERIFY(!cache.value().udisksctl);
}

void RemoteVolumeServiceTest::sessionLossBeforeOperationReturnsStructuredError()
{
    rfm::ssh::SshSession session;
    QSignalSpy results(&session, &rfm::ssh::SshSession::volumeOperationFinished);
    session.operateVolume(requestFor(rfm::core::VolumeOperation::Mount));
    QCOMPARE(results.size(), 1);
    const auto result = results.constFirst().constFirst().value<rfm::core::VolumeOperationResult>();
    QCOMPARE(result.error, rfm::core::VolumeOperationError::ConnectionLost);
    QCOMPARE(result.id, quint64{17});
}

QTEST_GUILESS_MAIN(RemoteVolumeServiceTest)

#include "test_remote_volume_service.moc"
