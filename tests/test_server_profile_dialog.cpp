#include "remotefilemanager/app/ServerProfileDialog.hpp"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>

#include <utility>

class ServerProfileDialogTest final : public QObject
{
    Q_OBJECT

  private slots:
    void exposesGeneralAndCapabilitiesTabs();
    void startsWithCapabilitiesNotDetected();
    void displaysSupportedCopyDataAndExtensionsReadOnly();
    void displaysUnsupportedCopyData_data();
    void displaysUnsupportedCopyData();
};

void ServerProfileDialogTest::exposesGeneralAndCapabilitiesTabs()
{
    rfm::app::ServerProfileDialog dialog;
    auto* const tabs = dialog.findChild<QTabWidget*>(QStringLiteral("serverProfileTabs"));
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->tabText(0), QStringLiteral("General"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("Capabilities"));
    QVERIFY(dialog.findChild<QWidget*>(QStringLiteral("serverProfileGeneralTab")) != nullptr);
    QVERIFY(dialog.findChild<QWidget*>(QStringLiteral("serverProfileCapabilitiesTab")) != nullptr);

    dialog.setProfile({QStringLiteral("Server A"), QStringLiteral("a.example.test"),
                       QStringLiteral("alice"), 2222, QStringLiteral("profile-a")});
    dialog.findChild<QLineEdit*>(QStringLiteral("serverNameEdit"))
        ->setText(QStringLiteral("Updated server"));
    dialog.findChild<QLineEdit*>(QStringLiteral("serverHostEdit"))
        ->setText(QStringLiteral("updated.example.test"));

    QCOMPARE(dialog.profile().displayName, QStringLiteral("Updated server"));
    QCOMPARE(dialog.profile().host, QStringLiteral("updated.example.test"));
    QCOMPARE(dialog.profile().id, QStringLiteral("profile-a"));
    QVERIFY(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
}

void ServerProfileDialogTest::startsWithCapabilitiesNotDetected()
{
    rfm::app::ServerProfileDialog dialog;

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataStatusLabel"))->text(),
             QStringLiteral("Not detected"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("sftpProtocolVersionLabel"))->text(),
             QStringLiteral("Not detected"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("capabilitiesLastDetectedLabel"))->text(),
             QStringLiteral("Not detected"));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("capabilitiesContextLabel"))
                ->text()
                .startsWith(QStringLiteral("Not detected")));
    auto* const protocol =
        dialog.findChild<QLabel*>(QStringLiteral("copyDataProtocolLabel"));
    QVERIFY(protocol != nullptr);
    auto* const remoteCopyLayout = qobject_cast<QFormLayout*>(protocol->parentWidget()->layout());
    QVERIFY(remoteCopyLayout != nullptr);
    auto* const extensionLabel = qobject_cast<QLabel*>(remoteCopyLayout->labelForField(protocol));
    QVERIFY(extensionLabel != nullptr);
    QCOMPARE(extensionLabel->text(), QStringLiteral("SFTP extension:"));
    QCOMPARE(dialog.findChild<QTableWidget*>(QStringLiteral("sftpExtensionsTable"))->rowCount(), 0);
}

void ServerProfileDialogTest::displaysSupportedCopyDataAndExtensionsReadOnly()
{
    rfm::app::ServerProfileDialog dialog;
    const auto capabilities = rfm::core::detectedServerCapabilities(
        {{QStringLiteral("copy-data"), QStringLiteral("1")},
         {QStringLiteral("statvfs@openssh.com"), QStringLiteral("2")}},
        QDateTime::currentDateTimeUtc(), 3);
    dialog.setServerCapabilities(capabilities);

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataStatusLabel"))->text(),
             QStringLiteral("Supported"));
    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("sftpProtocolVersionLabel"))->text(),
             QStringLiteral("3"));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("capabilitiesContextLabel"))
                ->text()
                .startsWith(QStringLiteral("Last known")));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("copyDataDescriptionLabel"))
                ->text()
                .contains(QStringLiteral("without passing through this computer")));

    auto* const extensions = dialog.findChild<QTableWidget*>(QStringLiteral("sftpExtensionsTable"));
    QVERIFY(extensions != nullptr);
    QCOMPARE(extensions->editTriggers(), QAbstractItemView::NoEditTriggers);
    QCOMPARE(extensions->rowCount(), 2);
    QCOMPARE(extensions->item(0, 0)->text(), QStringLiteral("copy-data"));
    QCOMPARE(extensions->item(0, 1)->text(), QStringLiteral("1"));
    QCOMPARE(extensions->item(1, 0)->text(), QStringLiteral("statvfs@openssh.com"));
    QCOMPARE(extensions->item(1, 1)->text(), QStringLiteral("2"));
    QVERIFY(!extensions->item(0, 0)->flags().testFlag(Qt::ItemIsEditable));

    dialog.setServerCapabilities(capabilities, true);
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("capabilitiesContextLabel"))
                ->text()
                .startsWith(QStringLiteral("Currently detected")));
}

void ServerProfileDialogTest::displaysUnsupportedCopyData_data()
{
    QTest::addColumn<QList<rfm::core::SftpExtensionCapability>>("extensions");
    QTest::newRow("not-announced") << QList<rfm::core::SftpExtensionCapability>{
        {QStringLiteral("fsync@openssh.com"), QStringLiteral("1")}};
    QTest::newRow("other-revision") << QList<rfm::core::SftpExtensionCapability>{
        {QStringLiteral("copy-data"), QStringLiteral("2")}};
}

void ServerProfileDialogTest::displaysUnsupportedCopyData()
{
    QFETCH(QList<rfm::core::SftpExtensionCapability>, extensions);
    rfm::app::ServerProfileDialog dialog;
    dialog.setServerCapabilities(rfm::core::detectedServerCapabilities(
        std::move(extensions), QDateTime::currentDateTimeUtc(), 3));

    QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("copyDataStatusLabel"))->text(),
             QStringLiteral("Not supported"));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("copyDataDescriptionLabel"))
                ->text()
                .contains(QStringLiteral("does not advertise")));
}

QTEST_MAIN(ServerProfileDialogTest)

#include "test_server_profile_dialog.moc"
