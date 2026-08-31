#include "remotefilemanager/core/InternalTransfer.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <functional>

class InternalTransferTest final : public QObject
{
    Q_OBJECT

  private slots:
    void serializesSshPayloadAndRejectsMalformedData();
    void serializesMultipleLocalSourcesAndRejectsAmbiguousData();
    void clipboardReplacesAndClearsIntentions();
    void newerCutDoesNotMatchAnOlderPasteGeneration();
    void validatesSessionAndRemoteDestinations();
    void validatesLocalDestinationsAndSourceCompatibility();
    void rejectsUnsafeLocalDestinations();
};

namespace
{

constexpr auto ApplicationInstance = "application-instance";
constexpr auto RemoteMachine = "ssh:fixture";

rfm::core::RemoteConnectionIdentity sshConnection()
{
    return {QStringLiteral("server.example.test"), 22, 7};
}

rfm::core::InternalTransferPayload sshPayload()
{
    return {rfm::core::FileSource::Ssh,
            QString::fromLatin1(RemoteMachine),
            QString::fromLatin1(ApplicationInstance),
            sshConnection(),
            2,
            {{QStringLiteral("/srv/file.txt"), false}, {QStringLiteral("/srv/folder"), true}}};
}

rfm::core::BrowserLocation sshDestination(const QString& path)
{
    return {rfm::core::FileSource::Ssh, QString::fromLatin1(RemoteMachine), path};
}

rfm::core::InternalTransferPayload localPayload(const QList<rfm::core::RemoteSelection>& sources)
{
    return {rfm::core::FileSource::Local,
            QString::fromLatin1(rfm::core::LocalMachineId),
            QString::fromLatin1(ApplicationInstance),
            {},
            3,
            sources};
}

rfm::core::BrowserLocation localDestination(const QString& path)
{
    return {rfm::core::FileSource::Local, QString::fromLatin1(rfm::core::LocalMachineId), path};
}

QByteArray withRootMutation(const QByteArray& encoded,
                            const std::function<void(QJsonObject&)>& mutation)
{
    QJsonObject root = QJsonDocument::fromJson(encoded).object();
    mutation(root);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

} // namespace

void InternalTransferTest::serializesSshPayloadAndRejectsMalformedData()
{
    const QByteArray encoded = rfm::core::encodeInternalTransfer(sshPayload());
    QVERIFY(!encoded.isEmpty());
    QVERIFY(encoded.contains("server.example.test"));
    QVERIFY(encoded.contains("/srv/file.txt"));
    QVERIFY(!encoded.contains("username"));
    QVERIFY(!encoded.contains("password"));
    QVERIFY(!encoded.contains("private"));

    const auto decoded = rfm::core::decodeInternalTransfer(encoded);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->source, rfm::core::FileSource::Ssh);
    QCOMPARE(decoded->sourceMachineId, QString::fromLatin1(RemoteMachine));
    QCOMPARE(decoded->applicationInstanceId, QString::fromLatin1(ApplicationInstance));
    QCOMPARE(decoded->connection, sshConnection());
    QCOMPARE(decoded->sourcePaneId, quint64{2});
    QCOMPARE(decoded->sources.size(), 2);
    QVERIFY(decoded->sources.constLast().directory);

    QVERIFY(!rfm::core::decodeInternalTransfer(QByteArrayLiteral("not-json")).has_value());
    QVERIFY(!rfm::core::decodeInternalTransfer(withRootMutation(encoded, [](QJsonObject& root) {
                 root.insert(QStringLiteral("version"), 1);
             })).has_value());
    QVERIFY(!rfm::core::decodeInternalTransfer(withRootMutation(encoded, [](QJsonObject& root) {
                 root.insert(QStringLiteral("connection"), QJsonValue::Null);
             })).has_value());
    QVERIFY(!rfm::core::decodeInternalTransfer(withRootMutation(encoded, [](QJsonObject& root) {
                 root.remove(QStringLiteral("machine"));
             })).has_value());
    QVERIFY(!rfm::core::decodeInternalTransfer(withRootMutation(encoded, [](QJsonObject& root) {
                 root.insert(QStringLiteral("unexpected"), true);
             })).has_value());
}

void InternalTransferTest::serializesMultipleLocalSourcesAndRejectsAmbiguousData()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("folder")));
    QFile file(root.filePath(QStringLiteral("file.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    const auto source =
        localPayload({{file.fileName(), false}, {root.filePath(QStringLiteral("folder")), true}});
    const QByteArray encoded = rfm::core::encodeInternalTransfer(source);
    QVERIFY(!encoded.isEmpty());
    const auto decoded = rfm::core::decodeInternalTransfer(encoded);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->source, rfm::core::FileSource::Local);
    QCOMPARE(decoded->sourceMachineId, QString::fromLatin1(rfm::core::LocalMachineId));
    QCOMPARE(decoded->connection, rfm::core::RemoteConnectionIdentity{});
    QCOMPARE(decoded->sources.size(), source.sources.size());
    QCOMPARE(decoded->sources.at(0).path, source.sources.at(0).path);
    QCOMPARE(decoded->sources.at(0).directory, source.sources.at(0).directory);
    QCOMPARE(decoded->sources.at(1).path, source.sources.at(1).path);
    QCOMPARE(decoded->sources.at(1).directory, source.sources.at(1).directory);

    QVERIFY(
        !rfm::core::decodeInternalTransfer(withRootMutation(encoded, [](QJsonObject& rootObject) {
             rootObject.insert(QStringLiteral("source"), QStringLiteral("ssh"));
         })).has_value());
    QVERIFY(
        !rfm::core::decodeInternalTransfer(withRootMutation(encoded, [](QJsonObject& rootObject) {
             QJsonObject connection;
             connection.insert(QStringLiteral("host"), QStringLiteral("fake"));
             connection.insert(QStringLiteral("port"), 22);
             connection.insert(QStringLiteral("generation"), QStringLiteral("1"));
             rootObject.insert(QStringLiteral("connection"), connection);
         })).has_value());
    QVERIFY(
        !rfm::core::decodeInternalTransfer(withRootMutation(encoded, [](QJsonObject& rootObject) {
             rootObject.insert(QStringLiteral("machine"), QStringLiteral("not-local"));
         })).has_value());

    auto relative = source;
    relative.sources = {{QStringLiteral("relative.txt"), false}};
    QVERIFY(rfm::core::encodeInternalTransfer(relative).isEmpty());
}

void InternalTransferTest::clipboardReplacesAndClearsIntentions()
{
    rfm::core::InternalClipboard clipboard;
    QVERIFY(!clipboard.hasContent());
    const quint64 emptyGeneration = clipboard.generation();

    clipboard.set(rfm::core::InternalTransferAction::Move, sshPayload());
    QVERIFY(clipboard.hasContent());
    QVERIFY(clipboard.isCut());
    const quint64 moveGeneration = clipboard.generation();
    QVERIFY(moveGeneration != emptyGeneration);

    auto replacement = sshPayload();
    replacement.sources = {{QStringLiteral("/srv/replacement.txt"), false}};
    clipboard.set(rfm::core::InternalTransferAction::Copy, replacement);
    QVERIFY(clipboard.hasContent());
    QVERIFY(!clipboard.isCut());
    QVERIFY(clipboard.generation() != moveGeneration);
    const quint64 replacementGeneration = clipboard.generation();
    QCOMPARE(clipboard.content()->payload.sources.constFirst().path,
             QStringLiteral("/srv/replacement.txt"));

    clipboard.clear();
    QVERIFY(!clipboard.hasContent());
    QVERIFY(clipboard.generation() != replacementGeneration);
}

void InternalTransferTest::newerCutDoesNotMatchAnOlderPasteGeneration()
{
    rfm::core::InternalClipboard clipboard;
    clipboard.set(rfm::core::InternalTransferAction::Move, sshPayload());
    const quint64 pastedCutGeneration = clipboard.generation();

    auto replacement = sshPayload();
    replacement.sources = {{QStringLiteral("/srv/newer-cut.txt"), false}};
    clipboard.set(rfm::core::InternalTransferAction::Move, replacement);

    QVERIFY(clipboard.isCut());
    QVERIFY(!clipboard.matchesCutGeneration(pastedCutGeneration));
    QVERIFY(clipboard.matchesCutGeneration(clipboard.generation()));
    QCOMPARE(clipboard.content()->payload.sources.constFirst().path,
             QStringLiteral("/srv/newer-cut.txt"));
}

void InternalTransferTest::validatesSessionAndRemoteDestinations()
{
    using Error = rfm::core::InternalTransferValidationError;
    const auto source = sshPayload();
    const auto connection = sshConnection();

    QVERIFY(rfm::core::validateInternalTransfer(source, QString::fromLatin1(ApplicationInstance),
                                                sshDestination(QStringLiteral("/destination")),
                                                connection)
                .accepted());
    QCOMPARE(rfm::core::validateInternalTransfer(source, QStringLiteral("another-instance"),
                                                 sshDestination(QStringLiteral("/destination")),
                                                 connection)
                 .error,
             Error::ForeignApplication);
    QCOMPARE(rfm::core::validateInternalTransfer(source, QString::fromLatin1(ApplicationInstance),
                                                 sshDestination(QStringLiteral("/destination")),
                                                 {QStringLiteral("other.example.test"), 22, 8})
                 .error,
             Error::IncompatibleConnection);
    QCOMPARE(rfm::core::validateInternalTransfer(source, QString::fromLatin1(ApplicationInstance),
                                                 sshDestination(QStringLiteral("relative")),
                                                 connection)
                 .error,
             Error::IncompatiblePathConvention);
    QCOMPARE(rfm::core::validateInternalTransfer(source, QString::fromLatin1(ApplicationInstance),
                                                 sshDestination(QStringLiteral("/srv")), connection)
                 .error,
             Error::IdenticalSourceAndDestination);
    QCOMPARE(rfm::core::validateInternalTransfer(
                 source, QString::fromLatin1(ApplicationInstance),
                 sshDestination(QStringLiteral("/srv/folder/child")), connection)
                 .error,
             Error::DestinationInsideSource);
    QCOMPARE(rfm::core::validateInternalTransfer(source, QString::fromLatin1(ApplicationInstance),
                                                 sshDestination(QStringLiteral("../invalid")),
                                                 connection)
                 .error,
             Error::InvalidDestination);
}

void InternalTransferTest::validatesLocalDestinationsAndSourceCompatibility()
{
    using Error = rfm::core::InternalTransferValidationError;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkdir(QStringLiteral("source")));
    QVERIFY(root.mkdir(QStringLiteral("destination")));
    QFile file(root.filePath(QStringLiteral("source/file.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    const auto local = localPayload({{file.fileName(), false}});
    const auto destination = localDestination(root.filePath(QStringLiteral("destination")));
    QVERIFY(rfm::core::validateInternalTransfer(local, QString::fromLatin1(ApplicationInstance),
                                                destination)
                .accepted());
    QVERIFY(rfm::core::validateInternalTransfer(
                local, QString::fromLatin1(ApplicationInstance),
                localDestination(root.filePath(QStringLiteral("destination/../destination"))))
                .accepted());
    QCOMPARE(rfm::core::validateInternalTransfer(local, QString::fromLatin1(ApplicationInstance),
                                                 sshDestination(QStringLiteral("/destination")),
                                                 sshConnection())
                 .error,
             Error::IncompatibleSource);
    QCOMPARE(rfm::core::validateInternalTransfer(
                 sshPayload(), QString::fromLatin1(ApplicationInstance), destination)
                 .error,
             Error::IncompatibleSource);

    auto missing = local;
    missing.sources = {{root.filePath(QStringLiteral("source/missing.txt")), false}};
    QCOMPARE(rfm::core::validateInternalTransfer(missing, QString::fromLatin1(ApplicationInstance),
                                                 destination)
                 .error,
             Error::InvalidSource);
    auto wrongKind = local;
    wrongKind.sources.first().directory = true;
    QCOMPARE(rfm::core::validateInternalTransfer(
                 wrongKind, QString::fromLatin1(ApplicationInstance), destination)
                 .error,
             Error::InvalidSource);
}

void InternalTransferTest::rejectsUnsafeLocalDestinations()
{
    using Error = rfm::core::InternalTransferValidationError;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("source/tree/child")));
    QFile file(root.filePath(QStringLiteral("source/file.txt")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    const auto filePayload = localPayload({{file.fileName(), false}});
    QCOMPARE(rfm::core::validateInternalTransfer(
                 filePayload, QString::fromLatin1(ApplicationInstance),
                 localDestination(root.filePath(QStringLiteral("source"))))
                 .error,
             Error::IdenticalSourceAndDestination);
    QCOMPARE(rfm::core::validateInternalTransfer(
                 filePayload, QString::fromLatin1(ApplicationInstance),
                 localDestination(root.filePath(QStringLiteral("source/../source"))))
                 .error,
             Error::IdenticalSourceAndDestination);

    const auto directoryPayload =
        localPayload({{root.filePath(QStringLiteral("source/tree")), true}});
    QCOMPARE(rfm::core::validateInternalTransfer(
                 directoryPayload, QString::fromLatin1(ApplicationInstance),
                 localDestination(root.filePath(QStringLiteral("source/tree"))))
                 .error,
             Error::DestinationInsideSource);
    QCOMPARE(rfm::core::validateInternalTransfer(
                 directoryPayload, QString::fromLatin1(ApplicationInstance),
                 localDestination(root.filePath(QStringLiteral("source/tree/child"))))
                 .error,
             Error::DestinationInsideSource);
    QCOMPARE(rfm::core::validateInternalTransfer(
                 directoryPayload, QString::fromLatin1(ApplicationInstance),
                 localDestination(root.filePath(QStringLiteral("missing"))))
                 .error,
             Error::InvalidDestination);
}

QTEST_APPLESS_MAIN(InternalTransferTest)

#include "test_internal_transfer.moc"
