#include "remotefilemanager/core/ServerProfileStore.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace
{

rfm::core::ConnectionProfile profile(const QString& id, const QString& name,
                                     const QString& host, quint16 port = 22)
{
    return {name, host, QStringLiteral("test-user"), port, id, true};
}

void writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(contents), contents.size());
}

} // namespace

class ServerProfileStoreTest final : public QObject
{
    Q_OBJECT

  private slots:
    void missingFileIsAnEmptyStore();
    void savesAndLoadsMultipleProfilesWithoutSecrets();
    void reportsMalformedJsonAndUnsupportedVersions();
    void skipsInvalidEntries();
    void replacesAndRemovesProfiles();
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
    const QList profiles{profile(QStringLiteral("home-id"), QStringLiteral("Home"),
                                 QStringLiteral("home.example.test")),
                         profile(QStringLiteral("vps-id"), {},
                                 QStringLiteral("vps.example.test"), 2222)};
    QString error;
    QVERIFY2(store.save(profiles, &error), qPrintable(error));

    const QList loaded = store.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.size(), 2);
    QCOMPARE(loaded.at(0).id, QStringLiteral("home-id"));
    QCOMPARE(loaded.at(1).port, quint16{2222});
    QVERIFY(loaded.at(1).allowPasswordFallback);

    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray serialized = file.readAll();
    QVERIFY(!serialized.contains("password"));
    QVERIFY(!serialized.contains("passphrase"));
    QVERIFY(!serialized.contains("privateKey"));
    QVERIFY(!serialized.contains("credential"));
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
    writeFile(store.filePath(),
              QByteArrayLiteral(R"({"version":1,"servers":[
                {"id":"valid","name":"NAS","host":"nas.test","username":"me","port":22,"allowPasswordFallback":false},
                {"id":"missing-host","name":"Broken","username":"me","port":22,"allowPasswordFallback":false},
                {"id":"bad-port","name":"Broken","host":"bad.test","username":"me","port":70000,"allowPasswordFallback":false},
                {"id":"valid","name":"Duplicate","host":"duplicate.test","username":"me","port":22,"allowPasswordFallback":false}
              ]})"));
    QString error;
    const QList loaded = store.load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.constFirst().host, QStringLiteral("nas.test"));
}

void ServerProfileStoreTest::replacesAndRemovesProfiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    QString error;
    QVERIFY(store.upsert(profile(QStringLiteral("one"), QStringLiteral("Old"),
                                 QStringLiteral("old.test")),
                         &error));
    QVERIFY(store.upsert(profile(QStringLiteral("two"), QStringLiteral("Second"),
                                 QStringLiteral("second.test")),
                         &error));
    QVERIFY(store.upsert(profile(QStringLiteral("one"), QStringLiteral("Updated"),
                                 QStringLiteral("new.test"), 2200),
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

void ServerProfileStoreTest::rejectsInvalidProfiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerProfileStore store(temporary.path());
    QString error;
    QVERIFY(!store.save({profile({}, QStringLiteral("No id"), QStringLiteral("host.test"))},
                        &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(store.filePath()));
}

QTEST_APPLESS_MAIN(ServerProfileStoreTest)

#include "test_server_profile_store.moc"
