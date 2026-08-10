#include "remotefilemanager/core/OperationHistoryStore.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <algorithm>

namespace
{

rfm::core::OperationProgress operation(quint64 id, rfm::core::OperationKind kind,
                                       rfm::core::OperationState state, qint64 seconds)
{
    rfm::core::OperationProgress result;
    result.id = id;
    result.kind = kind;
    result.state = state;
    result.sources = {QStringLiteral("/source/item-%1").arg(id)};
    result.destination = QStringLiteral("/destination");
    result.transferredBytes = id * 10;
    result.totalBytes = id * 20;
    result.completedItems = state == rfm::core::OperationState::Completed ? 1 : 0;
    result.totalItems = 1;
    result.error = state == rfm::core::OperationState::Failed
                       ? QStringLiteral("permission denied")
                       : QString{};
    result.finishedAt = QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC);
    return result;
}

void writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(contents), contents.size());
}

} // namespace

class OperationHistoryTest final : public QObject
{
    Q_OBJECT

  private slots:
    void missingEmptyCorruptAndUnknownFilesAreIgnored();
    void savesAndLoadsOnlyTerminalOperations();
    void appliesRetentionToNewestEntries();
    void serializesOnlyTheDocumentedFields();
};

void OperationHistoryTest::missingEmptyCorruptAndUnknownFilesAreIgnored()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const rfm::core::OperationHistoryStore store(directory.path());
    QVERIFY(store.load().isEmpty());

    writeFile(store.filePath(), {});
    QVERIFY(store.load().isEmpty());
    writeFile(store.filePath(), QByteArrayLiteral("not json"));
    QVERIFY(store.load().isEmpty());
    writeFile(store.filePath(),
              QJsonDocument(QJsonObject{{QStringLiteral("version"), 999},
                                        {QStringLiteral("operations"), QJsonArray{}}})
                  .toJson());
    QVERIFY(store.load().isEmpty());

    const QJsonObject active{{QStringLiteral("id"), QStringLiteral("12")},
                             {QStringLiteral("kind"), QStringLiteral("remote-copy")},
                             {QStringLiteral("sources"),
                              QJsonArray{QStringLiteral("/source/item")}},
                             {QStringLiteral("destination"), QStringLiteral("/destination")},
                             {QStringLiteral("state"), QStringLiteral("running")},
                             {QStringLiteral("finishedAt"),
                              QStringLiteral("2026-08-10T10:00:00.000Z")}};
    writeFile(store.filePath(),
              QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                        {QStringLiteral("operations"), QJsonArray{active}}})
                  .toJson());
    QVERIFY(store.load().isEmpty());
}

void OperationHistoryTest::savesAndLoadsOnlyTerminalOperations()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const rfm::core::OperationHistoryStore store(directory.path());
    const QList<rfm::core::OperationProgress> operations{
        operation(1, rfm::core::OperationKind::Upload,
                  rfm::core::OperationState::Completed, 1),
        operation(2, rfm::core::OperationKind::Download,
                  rfm::core::OperationState::Failed, 2),
        operation(3, rfm::core::OperationKind::RemoteCopy,
                  rfm::core::OperationState::Cancelled, 3),
        operation(4, rfm::core::OperationKind::RemoteMove,
                  rfm::core::OperationState::Completed, 4),
        operation(5, rfm::core::OperationKind::Upload, rfm::core::OperationState::Queued, 5),
        operation(6, rfm::core::OperationKind::Download,
                  rfm::core::OperationState::Preparing, 6),
        operation(7, rfm::core::OperationKind::RemoteCopy,
                  rfm::core::OperationState::Running, 7),
        operation(8, rfm::core::OperationKind::Upload, rfm::core::OperationState::Paused, 8),
        operation(9, rfm::core::OperationKind::Upload,
                  rfm::core::OperationState::Finalizing, 9),
        operation(10, rfm::core::OperationKind::Upload,
                  rfm::core::OperationState::Cancelling, 10)};

    QString error;
    QVERIFY2(store.save(operations, &error), qPrintable(error));
    const QList<rfm::core::OperationProgress> restored = store.load();
    QCOMPARE(restored.size(), 4);
    QCOMPARE(restored.at(0).kind, rfm::core::OperationKind::Upload);
    QCOMPARE(restored.at(0).state, rfm::core::OperationState::Completed);
    QCOMPARE(restored.at(1).kind, rfm::core::OperationKind::Download);
    QCOMPARE(restored.at(1).state, rfm::core::OperationState::Failed);
    QCOMPARE(restored.at(1).error, QStringLiteral("permission denied"));
    QCOMPARE(restored.at(2).kind, rfm::core::OperationKind::RemoteCopy);
    QCOMPARE(restored.at(2).state, rfm::core::OperationState::Cancelled);
    QCOMPARE(restored.at(3).kind, rfm::core::OperationKind::RemoteMove);
    QCOMPARE(restored.at(3).sources, QStringList{QStringLiteral("/source/item-4")});
    QCOMPARE(restored.at(3).destination, QStringLiteral("/destination"));
    QCOMPARE(restored.at(3).completedItems, quint64{1});
    QCOMPARE(restored.at(3).totalItems, quint64{1});
    QVERIFY(std::ranges::all_of(restored, [](const auto& item) {
        return rfm::core::isTerminal(item.state);
    }));
}

void OperationHistoryTest::appliesRetentionToNewestEntries()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const rfm::core::OperationHistoryStore store(directory.path());
    QList<rfm::core::OperationProgress> operations;
    for (quint64 id = 1;
         id <= static_cast<quint64>(rfm::core::OperationHistoryStore::maximumEntries + 5); ++id) {
        operations.push_back(operation(id, rfm::core::OperationKind::Upload,
                                       rfm::core::OperationState::Completed,
                                       static_cast<qint64>(id)));
    }
    QVERIFY(store.save(operations));
    const QList<rfm::core::OperationProgress> restored = store.load();
    QCOMPARE(restored.size(), rfm::core::OperationHistoryStore::maximumEntries);
    QCOMPARE(restored.constFirst().id, quint64{6});
    QCOMPARE(restored.constLast().id,
             static_cast<quint64>(rfm::core::OperationHistoryStore::maximumEntries + 5));
}

void OperationHistoryTest::serializesOnlyTheDocumentedFields()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const rfm::core::OperationHistoryStore store(directory.path());
    auto terminal = operation(42, rfm::core::OperationKind::Download,
                              rfm::core::OperationState::Completed, 42);
    terminal.currentItem = QStringLiteral("PRIVATE_KEY_TOKEN_PASSWORD_COMMAND");
    terminal.bytesPerSecond = 12345;
    terminal.pauseResumeSupported = true;
    terminal.cancellationSupported = true;
    QVERIFY(store.save({terminal}));

    QFile file(store.filePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    QVERIFY(!contents.contains("PRIVATE_KEY_TOKEN_PASSWORD_COMMAND"));
    QVERIFY(!contents.contains("currentItem"));
    QVERIFY(!contents.contains("bytesPerSecond"));
    QVERIFY(!contents.contains("pauseResumeSupported"));
    QVERIFY(!contents.contains("cancellationSupported"));
    QVERIFY(!contents.contains("sshCommand"));
    QVERIFY(contents.contains("\"version\":1"));
}

QTEST_MAIN(OperationHistoryTest)

#include "test_operation_history.moc"
