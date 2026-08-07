#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <QTest>

class ConnectionProfileTest final : public QObject {
    Q_OBJECT

private slots:
    void rejectsMissingRequiredFields();
    void acceptsStandardProfile();
    void buildsFallbackDisplayName();
};

void ConnectionProfileTest::rejectsMissingRequiredFields()
{
    rfm::core::ConnectionProfile profile;
    QVERIFY(!profile.isValid());

    profile.host = QStringLiteral("server.example.test");
    QVERIFY(!profile.isValid());

    profile.username = QStringLiteral("gabriel");
    profile.port = 0;
    QVERIFY(!profile.isValid());
}

void ConnectionProfileTest::acceptsStandardProfile()
{
    const rfm::core::ConnectionProfile profile{
        QStringLiteral("Home server"),
        QStringLiteral("server.example.test"),
        QStringLiteral("gabriel"),
        22,
    };

    QVERIFY(profile.isValid());
    QCOMPARE(profile.effectiveDisplayName(), QStringLiteral("Home server"));
}

void ConnectionProfileTest::buildsFallbackDisplayName()
{
    const rfm::core::ConnectionProfile profile{
        {},
        QStringLiteral("server.example.test"),
        QStringLiteral("gabriel"),
        22,
    };

    QCOMPARE(profile.effectiveDisplayName(), QStringLiteral("gabriel@server.example.test"));
}

QTEST_APPLESS_MAIN(ConnectionProfileTest)

#include "test_connection_profile.moc"

