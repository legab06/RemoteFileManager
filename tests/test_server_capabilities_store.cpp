#include "remotefilemanager/core/ServerCapabilitiesStore.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <optional>
#include <utility>

namespace
{

rfm::core::PersistedServerCapabilities
snapshot(const QString& profileId, const QString& host,
         QList<rfm::core::SftpExtensionCapability> extensions, int version = 3)
{
    return {profileId, host, QStringLiteral("test-user"), 22,
            rfm::core::detectedServerCapabilities(std::move(extensions),
                                                  QDateTime::currentDateTimeUtc(), version)};
}

void writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(contents), contents.size());
}

} // namespace

class ServerCapabilitiesStoreTest final : public QObject
{
    Q_OBJECT

  private slots:
    void missingFileIsAnEmptyStore();
    void savesAndReloadsIndependentSnapshots();
    void replacesAndRemovesSnapshots();
    void rejectsTemporaryProfiles();
    void reportsMalformedJsonAndUnsupportedVersions();
    void skipsInvalidEntriesAndPreservesUnknownExtensions();
    void rejectsSerializedContentsThatExceedMaximumFileSize();
};

void ServerCapabilitiesStoreTest::missingFileIsAnEmptyStore()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerCapabilitiesStore store(temporary.path());
    QString error = QStringLiteral("stale");

    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(error.isEmpty());
}

void ServerCapabilitiesStoreTest::savesAndReloadsIndependentSnapshots()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerCapabilitiesStore store(temporary.path());
    const QList snapshots{snapshot(QStringLiteral("profile-a"), QStringLiteral("a.example.test"),
                                   {{QStringLiteral("copy-data"), QStringLiteral("1")}}),
                          snapshot(QStringLiteral("profile-b"), QStringLiteral("b.example.test"),
                                   {{QStringLiteral("fsync@openssh.com"), QStringLiteral("1")}},
                                   4)};
    QString error;

    QVERIFY2(store.save(snapshots, &error), qPrintable(error));
    const QList loaded = store.load(&error);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.size(), 2);
    QCOMPARE(loaded.at(0).profileId, QStringLiteral("profile-a"));
    QCOMPARE(loaded.at(0).capabilities.copyDataVersion1, rfm::core::CapabilitySupport::Supported);
    QCOMPARE(loaded.at(1).profileId, QStringLiteral("profile-b"));
    QCOMPARE(loaded.at(1).capabilities.sftpProtocolVersion, std::optional<int>{4});
    QCOMPARE(loaded.at(1).capabilities.copyDataVersion1, rfm::core::CapabilitySupport::Unsupported);
}

void ServerCapabilitiesStoreTest::replacesAndRemovesSnapshots()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerCapabilitiesStore store(temporary.path());
    QString error;
    QVERIFY(store.upsert(snapshot(QStringLiteral("profile-a"), QStringLiteral("a.example.test"),
                                  {{QStringLiteral("copy-data"), QStringLiteral("1")}}),
                         &error));
    QVERIFY(store.upsert(snapshot(QStringLiteral("profile-b"), QStringLiteral("b.example.test"),
                                  {{QStringLiteral("fsync@openssh.com"), QStringLiteral("1")}}),
                         &error));
    QVERIFY(store.upsert(snapshot(QStringLiteral("profile-a"), QStringLiteral("a.example.test"),
                                  {{QStringLiteral("copy-data"), QStringLiteral("2")}}, 4),
                         &error));

    QList loaded = store.load(&error);
    QCOMPARE(loaded.size(), 2);
    QCOMPARE(loaded.constFirst().capabilities.sftpProtocolVersion, std::optional<int>{4});
    QCOMPARE(loaded.constFirst().capabilities.copyDataVersion1,
             rfm::core::CapabilitySupport::Unsupported);

    QVERIFY(store.remove(QStringLiteral("profile-a"), &error));
    QVERIFY(store.remove(QStringLiteral("never-detected"), &error));
    loaded = store.load(&error);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.constFirst().profileId, QStringLiteral("profile-b"));
}

void ServerCapabilitiesStoreTest::rejectsTemporaryProfiles()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerCapabilitiesStore store(temporary.path());
    QString error;

    QVERIFY(!store.upsert(snapshot({}, QStringLiteral("temporary.example.test"),
                                   {{QStringLiteral("copy-data"), QStringLiteral("1")}}),
                          &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFile::exists(store.filePath()));
}

void ServerCapabilitiesStoreTest::reportsMalformedJsonAndUnsupportedVersions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerCapabilitiesStore store(temporary.path());
    QString error;

    writeFile(store.filePath(), QByteArrayLiteral("{not-json"));
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(!error.isEmpty());

    writeFile(store.filePath(), QByteArrayLiteral(R"({"version":99,"snapshots":[]})"));
    QVERIFY(store.load(&error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void ServerCapabilitiesStoreTest::skipsInvalidEntriesAndPreservesUnknownExtensions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerCapabilitiesStore store(temporary.path());
    writeFile(store.filePath(), QByteArrayLiteral(R"({"version":1,"snapshots":[
        {"profileId":"valid","host":"valid.test","username":"me","port":22,
         "detectedAt":"2026-09-10T15:30:00.000Z","sftpProtocolVersion":3,
         "extensions":[{"name":"future-extension@example.test","data":"revision-x"}]},
        {"profileId":"invalid","host":"","username":"me","port":22,
         "detectedAt":"invalid","extensions":[]}
    ]})"));
    QString error;

    const QList loaded = store.load(&error);

    QCOMPARE(loaded.size(), 1);
    QVERIFY(!error.isEmpty());
    QCOMPARE(loaded.constFirst().capabilities.sftpExtensions.constFirst().name,
             QStringLiteral("future-extension@example.test"));
    QCOMPARE(loaded.constFirst().capabilities.sftpExtensions.constFirst().data,
             QStringLiteral("revision-x"));

    QVERIFY2(
        store.upsert(snapshot(QStringLiteral("second"), QStringLiteral("second.test"), {}), &error),
        qPrintable(error));
    const QList recovered = store.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(recovered.size(), 2);
}

void ServerCapabilitiesStoreTest::rejectsSerializedContentsThatExceedMaximumFileSize()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const rfm::core::ServerCapabilitiesStore store(temporary.path());
    QString error;
    const auto original = snapshot(QStringLiteral("original"), QStringLiteral("original.test"), {});
    QVERIFY2(store.save({original}, &error), qPrintable(error));

    QFile existing(store.filePath());
    QVERIFY(existing.open(QIODevice::ReadOnly));
    const QByteArray originalContents = existing.readAll();
    existing.close();

    const QString maximumExtensionData(16 * 1024, QChar{'x'});
    QList<rfm::core::SftpExtensionCapability> extensions;
    extensions.reserve(512);
    for (int index = 0; index < 512; ++index) {
        extensions.push_back({QStringLiteral("extension-%1").arg(index), maximumExtensionData});
    }
    const auto oversized = snapshot(QStringLiteral("oversized"), QStringLiteral("oversized.test"),
                                    std::move(extensions));
    QVERIFY(oversized.isValid());

    QVERIFY(!store.save({oversized}, &error));
    QVERIFY(error.contains(QStringLiteral("maximum allowed size")));

    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), originalContents);
    existing.close();
    const QList loaded = store.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.constFirst().profileId, QStringLiteral("original"));
}

QTEST_APPLESS_MAIN(ServerCapabilitiesStoreTest)

#include "test_server_capabilities_store.moc"
