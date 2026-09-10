#include "remotefilemanager/core/ServerProfileStore.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace
{

rfm::core::ConnectionProfile profile(const QString& id, const QString& name, const QString& host,
                                     quint16 port = 22)
{
    return {name, host, QStringLiteral("test-user"), port, id, true};
}

void writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(contents), contents.size());
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

} // namespace

class ServerProfileStoreTest final : public QObject
{
    Q_OBJECT

  private slots:
    void missingFileIsAnEmptyStore();
    void savesAndLoadsMultipleProfilesWithoutSecrets();
    void savesAndLoadsOptionalPrivateKeyPath();
    void savesAndLoadsPasswordOnlyMode();
    void reportsMalformedJsonAndUnsupportedVersions();
    void skipsInvalidEntries();
    void loadsLegacyPasswordFallbackSetting();
    void replacesAndRemovesProfiles();
    void refusesUpsertWhenStoreContainsInvalidEntry();
    void refusesRemoveWhenStoreContainsInvalidEntry();
    void refusesMutationWhenStoreContainsDuplicateIdentifier();
    void rejectsInvalidProfiles();
};

void ServerProfileStoreTest::missingFileIsAnEmptyStore()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    QString error = QStringLiteral("stale");
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(error.isEmpty());
}

void ServerProfileStoreTest::savesAndLoadsMultipleProfilesWithoutSecrets()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    const QList profiles{
        profile(QStringLiteral("home-id"), QStringLiteral("Home"),
                QStringLiteral("home.example.test")),
        profile(QStringLiteral("vps-id"), {}, QStringLiteral("vps.example.test"), 2222)};
    QString error;
    QVERIFY2(store.save(profiles, &error), qPrintable(error));

    const QList loaded = store.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.size(), 2);
    QCOMPARE(loaded.at(0).id, QStringLiteral("home-id"));
    QCOMPARE(loaded.at(1).port, quint16{2222});
    QVERIFY(loaded.at(1).allowPasswordAuthentication);

    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray serialized = file.readAll();
    QVERIFY(serialized.contains("allowPasswordFallback"));
    QVERIFY(!serialized.contains("allowPasswordAuthentication"));
    QVERIFY(!serialized.contains("password"));
    QVERIFY(!serialized.contains("passphrase"));
    QVERIFY(!serialized.contains("privateKey"));
    QVERIFY(!serialized.contains("credential"));
    QVERIFY(!serialized.contains("privateKeyPath"));
}

void ServerProfileStoreTest::savesAndLoadsOptionalPrivateKeyPath()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    auto windows = profile(QStringLiteral("windows-id"), QStringLiteral("Windows Server"),
                           QStringLiteral("192.0.2.10"));
    windows.privateKeyPath = QStringLiteral("~/.ssh/rfm_windows_server");
    QString error;
    QVERIFY2(store.save({windows}, &error), qPrintable(error));

    const QList loaded = store.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.constFirst().privateKeyPath, QStringLiteral("~/.ssh/rfm_windows_server"));

    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray serialized = file.readAll();
    QVERIFY(serialized.contains("privateKeyPath"));
    QVERIFY(serialized.contains("~/.ssh/rfm_windows_server"));
    QVERIFY(!serialized.contains("PRIVATE KEY-----"));
    QVERIFY(!serialized.contains("passphrase"));
}

void ServerProfileStoreTest::savesAndLoadsPasswordOnlyMode()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    auto passwordOnly = profile(QStringLiteral("password-id"), QStringLiteral("Password server"),
                                QStringLiteral("password.example.test"));
    passwordOnly.authenticationMode = rfm::core::AuthenticationMode::PasswordOnly;
    passwordOnly.privateKeyPath = QStringLiteral("~/.ssh/ignored-key");
    passwordOnly.allowPasswordAuthentication = true;
    QString error;
    QVERIFY2(store.save({passwordOnly}, &error), qPrintable(error));

    const QList loaded = store.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.constFirst().authenticationMode, rfm::core::AuthenticationMode::PasswordOnly);
    QVERIFY(!loaded.constFirst().allowPasswordAuthentication);
    QVERIFY(loaded.constFirst().privateKeyPath.isEmpty());

    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray serialized = file.readAll();
    QVERIFY(serialized.contains("authenticationMode"));
    QVERIFY(serialized.contains("passwordOnly"));
    QVERIFY(!serialized.contains("privateKeyPath"));
    QVERIFY(!serialized.contains("password\":\""));
}

void ServerProfileStoreTest::reportsMalformedJsonAndUnsupportedVersions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    writeFile(store.filePath(), QByteArrayLiteral("{not-json"));
    QString error;
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(!error.isEmpty());

    writeFile(store.filePath(), QByteArrayLiteral("{\"version\":99,\"servers\":[]}"));
    error.clear();
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("version"), Qt::CaseInsensitive));
}

void ServerProfileStoreTest::skipsInvalidEntries()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    writeFile(store.filePath(), QByteArrayLiteral(R"({"version":1,"servers":[
                {"id":"valid","name":"NAS","host":"nas.test","username":"me","port":22,"allowPasswordAuthentication":false},
                {"id":"missing-host","name":"Broken","username":"me","port":22,"allowPasswordAuthentication":false},
                {"id":"bad-port","name":"Broken","host":"bad.test","username":"me","port":70000,"allowPasswordAuthentication":false},
                {"id":"valid","name":"Duplicate","host":"duplicate.test","username":"me","port":22,"allowPasswordAuthentication":false}
              ]})"));
    QString error;
    const QList loaded = store.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.constFirst().host, QStringLiteral("nas.test"));
}

void ServerProfileStoreTest::loadsLegacyPasswordFallbackSetting()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    writeFile(store.filePath(), QByteArrayLiteral(R"({"version":1,"servers":[
        {"id":"legacy","name":"Legacy","host":"legacy.test","username":"me","port":22,
         "allowPasswordFallback":true}
    ]})"));
    QString error;
    const QList loaded = store.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.size(), 1);
    QVERIFY(loaded.constFirst().allowPasswordAuthentication);
    QVERIFY(loaded.constFirst().privateKeyPath.isEmpty());
}

void ServerProfileStoreTest::replacesAndRemovesProfiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    QString error;
    QVERIFY(store.upsert(
        profile(QStringLiteral("one"), QStringLiteral("Old"), QStringLiteral("old.test")), &error));
    QVERIFY(store.upsert(
        profile(QStringLiteral("two"), QStringLiteral("Second"), QStringLiteral("second.test")),
        &error));
    QVERIFY(store.upsert(
        profile(QStringLiteral("one"), QStringLiteral("Updated"), QStringLiteral("new.test"), 2200),
        &error));
    QList loaded = store.load(&error);
    QCOMPARE(loaded.size(), 2);
    QCOMPARE(loaded.constFirst().displayName, QStringLiteral("Updated"));
    QCOMPARE(loaded.constFirst().host, QStringLiteral("new.test"));

    QVERIFY(store.remove(QStringLiteral("two"), &error));
    loaded = store.load(&error);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.constFirst().id, QStringLiteral("one"));
}

void ServerProfileStoreTest::refusesUpsertWhenStoreContainsInvalidEntry()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    const QByteArray contents = QByteArrayLiteral(R"({"version":1,"servers":[
        {"id":"valid","name":"NAS","host":"nas.test","username":"me","port":22,"allowPasswordAuthentication":false},
        {"id":"invalid","name":"Broken","username":"me","port":22,"allowPasswordAuthentication":false}
    ]})");
    writeFile(store.filePath(), contents);

    QString error;
    const QList loaded = store.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.constFirst().id, QStringLiteral("valid"));

    QVERIFY(!store.upsert(
        profile(QStringLiteral("valid"), QStringLiteral("Updated"), QStringLiteral("new.test")),
        &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(store.filePath()), contents);
}

void ServerProfileStoreTest::refusesRemoveWhenStoreContainsInvalidEntry()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    const QByteArray contents = QByteArrayLiteral(R"({"version":1,"servers":[
        {"id":"valid","name":"NAS","host":"nas.test","username":"me","port":22,"allowPasswordAuthentication":false},
        {"id":"invalid","name":"Broken","host":"bad.test","username":"me","port":70000,"allowPasswordAuthentication":false}
    ]})");
    writeFile(store.filePath(), contents);

    QString error;
    QVERIFY(!store.remove(QStringLiteral("valid"), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(store.filePath()), contents);
}

void ServerProfileStoreTest::refusesMutationWhenStoreContainsDuplicateIdentifier()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    const QByteArray contents = QByteArrayLiteral(R"({"version":1,"servers":[
        {"id":"duplicate","name":"First","host":"first.test","username":"me","port":22,"allowPasswordAuthentication":false},
        {"id":"duplicate","name":"Second","host":"second.test","username":"me","port":22,"allowPasswordAuthentication":false}
    ]})");
    writeFile(store.filePath(), contents);

    QString error;
    QVERIFY(!store.remove(QStringLiteral("duplicate"), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readFile(store.filePath()), contents);
}

void ServerProfileStoreTest::rejectsInvalidProfiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    QString error;
    QVERIFY(
        !store.save({profile({}, QStringLiteral("No id"), QStringLiteral("host.test"))}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(store.filePath()));
}

QTEST_APPLESS_MAIN(ServerProfileStoreTest)

#include "test_server_profile_store.moc"
