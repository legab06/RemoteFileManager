#include "../src/ssh/RemoteCopyTelemetryCounter.hpp"
#include "../src/ssh/SftpWriteLoop.hpp"

#include <QList>
#include <QTest>

#include <limits>

namespace
{

QByteArray concatenate(const QList<QByteArray>& chunks)
{
    QByteArray data;
    for (const QByteArray& chunk : chunks) {
        data.append(chunk);
    }
    return data;
}

class SftpWriteLoopTest final : public QObject
{
    Q_OBJECT

  private slots:
    void writesCompleteBufferInOneCall();
    void continuesAfterShortWrites();
    void reportsErrorAfterPartialWrite();
    void rejectsWriteCountLargerThanRemaining();
    void stopsWhenWriteMakesNoProgress();
    void skipsEmptyBuffer();
    void tracksKnownFileAcrossShortWrites();
    void preservesPartialProgressAcrossErrors();
    void representsEmptyAndRecursiveCopiesHonestly();
    void resetsBetweenCopies();
    void rejectsTelemetryCounterOverflow();
};

void SftpWriteLoopTest::writesCompleteBufferInOneCall()
{
    const QByteArray data = QByteArrayLiteral("abcdef");
    QList<QByteArray> arguments;
    QList<QByteArray> transmitted;
    const auto outcome =
        rfm::ssh::detail::writeSftpBuffer(data, [&](const char* bytes, qsizetype size) {
            arguments.push_back(QByteArray(bytes, size));
            transmitted.push_back(QByteArray(bytes, size));
            return size;
        });

    QVERIFY(outcome == rfm::ssh::detail::SftpWriteLoopResult::Completed);
    QCOMPARE(arguments, QList<QByteArray>{data});
    QCOMPARE(concatenate(transmitted), data);
}

void SftpWriteLoopTest::continuesAfterShortWrites()
{
    const QByteArray data = QByteArrayLiteral("abcdef");
    const QList<qsizetype> writes{3, 2, 1};
    QList<QByteArray> arguments;
    QList<QByteArray> transmitted;
    qsizetype writeIndex = 0;
    const auto outcome =
        rfm::ssh::detail::writeSftpBuffer(data, [&](const char* bytes, qsizetype size) {
            arguments.push_back(QByteArray(bytes, size));
            const qsizetype written = writes.at(writeIndex++);
            transmitted.push_back(QByteArray(bytes, written));
            return written;
        });

    QVERIFY(outcome == rfm::ssh::detail::SftpWriteLoopResult::Completed);
    QCOMPARE(arguments, (QList<QByteArray>{QByteArrayLiteral("abcdef"), QByteArrayLiteral("def"),
                                           QByteArrayLiteral("f")}));
    QCOMPARE(concatenate(transmitted), data);
}

void SftpWriteLoopTest::reportsErrorAfterPartialWrite()
{
    const QByteArray data = QByteArrayLiteral("abcdef");
    QList<QByteArray> arguments;
    QList<QByteArray> transmitted;
    const QList<qint64> writes{3, -1};
    qsizetype writeIndex = 0;
    const auto outcome =
        rfm::ssh::detail::writeSftpBuffer(data, [&](const char* bytes, qsizetype size) {
            arguments.push_back(QByteArray(bytes, size));
            const qint64 written = writes.at(writeIndex++);
            if (written > 0) {
                transmitted.push_back(QByteArray(bytes, written));
            }
            return written;
        });

    QVERIFY(outcome == rfm::ssh::detail::SftpWriteLoopResult::WriteError);
    QCOMPARE(arguments, (QList<QByteArray>{QByteArrayLiteral("abcdef"), QByteArrayLiteral("def")}));
    QCOMPARE(concatenate(transmitted), QByteArrayLiteral("abc"));
}

void SftpWriteLoopTest::rejectsWriteCountLargerThanRemaining()
{
    int calls = 0;
    const auto outcome = rfm::ssh::detail::writeSftpBuffer(
        QByteArrayLiteral("abc"), [&](const char*, qsizetype) {
            ++calls;
            return qint64{4};
        });

    QVERIFY(outcome == rfm::ssh::detail::SftpWriteLoopResult::WriteError);
    QCOMPARE(calls, 1);
}

void SftpWriteLoopTest::stopsWhenWriteMakesNoProgress()
{
    const QByteArray data = QByteArrayLiteral("abcdef");
    QList<QByteArray> arguments;
    const QList<qint64> writes{3, 0};
    qsizetype writeIndex = 0;
    const auto outcome =
        rfm::ssh::detail::writeSftpBuffer(data, [&](const char* bytes, qsizetype size) {
            arguments.push_back(QByteArray(bytes, size));
            return writes.at(writeIndex++);
        });

    QVERIFY(outcome == rfm::ssh::detail::SftpWriteLoopResult::NoProgress);
    QCOMPARE(arguments, (QList<QByteArray>{QByteArrayLiteral("abcdef"), QByteArrayLiteral("def")}));
}

void SftpWriteLoopTest::skipsEmptyBuffer()
{
    bool called = false;
    const auto outcome = rfm::ssh::detail::writeSftpBuffer({}, [&](const char*, qsizetype) {
        called = true;
        return qint64{0};
    });

    QVERIFY(outcome == rfm::ssh::detail::SftpWriteLoopResult::Completed);
    QVERIFY(!called);
}

void SftpWriteLoopTest::tracksKnownFileAcrossShortWrites()
{
    rfm::ssh::detail::RemoteCopyTelemetryCounter counter;
    counter.reset(65'536);

    QVERIFY(counter.recordWrite(20'000, 65'536));
    QCOMPARE(counter.telemetry().transferredBytes, quint64{20'000});
    QVERIFY(counter.recordWrite(30'000, 45'536));
    QCOMPARE(counter.telemetry().transferredBytes, quint64{50'000});
    QVERIFY(counter.recordWrite(15'536, 15'536));

    QCOMPARE(counter.telemetry().transferredBytes, quint64{65'536});
    QCOMPARE(counter.telemetry().totalBytes, quint64{65'536});
    QVERIFY(counter.telemetry().byteProgressAvailable);
}

void SftpWriteLoopTest::preservesPartialProgressAcrossErrors()
{
    rfm::ssh::detail::RemoteCopyTelemetryCounter counter;
    counter.reset(12);
    QVERIFY(counter.recordWrite(4, 12));

    QVERIFY(!counter.recordWrite(-1, 8));
    QCOMPARE(counter.telemetry().transferredBytes, quint64{4});
    QVERIFY(!counter.recordWrite(0, 8));
    QCOMPARE(counter.telemetry().transferredBytes, quint64{4});
    QVERIFY(!counter.recordWrite(9, 8));
    QCOMPARE(counter.telemetry().transferredBytes, quint64{4});

    // A failed read never reaches recordWrite(), so the last observed write remains authoritative.
    QCOMPARE(counter.telemetry().transferredBytes, quint64{4});
}

void SftpWriteLoopTest::representsEmptyAndRecursiveCopiesHonestly()
{
    rfm::ssh::detail::RemoteCopyTelemetryCounter counter;
    counter.reset(0);
    QCOMPARE(counter.telemetry().transferredBytes, quint64{0});
    QCOMPARE(counter.telemetry().totalBytes, quint64{0});
    QVERIFY(counter.telemetry().byteProgressAvailable);

    counter.reset();
    QVERIFY(counter.recordWrite(5, 5));
    QCOMPARE(counter.telemetry().transferredBytes, quint64{5});
    QCOMPARE(counter.telemetry().totalBytes, quint64{0});
    QVERIFY(!counter.telemetry().byteProgressAvailable);

    counter.reset(std::nullopt, true);
    QVERIFY(counter.recordWrite(5, 5));
    QCOMPARE(counter.telemetry().transferredBytes, quint64{5});
    QCOMPARE(counter.telemetry().totalBytes, quint64{0});
    QVERIFY(counter.telemetry().byteProgressAvailable);
}

void SftpWriteLoopTest::resetsBetweenCopies()
{
    rfm::ssh::detail::RemoteCopyTelemetryCounter counter;
    counter.reset(10);
    QVERIFY(counter.recordWrite(6, 10));

    counter.reset(3);
    QCOMPARE(counter.telemetry().transferredBytes, quint64{0});
    QCOMPARE(counter.telemetry().totalBytes, quint64{3});
    QVERIFY(counter.telemetry().byteProgressAvailable);

    counter.reset();
    QCOMPARE(counter.telemetry().transferredBytes, quint64{0});
    QCOMPARE(counter.telemetry().totalBytes, quint64{0});
    QVERIFY(!counter.telemetry().byteProgressAvailable);
}

void SftpWriteLoopTest::rejectsTelemetryCounterOverflow()
{
    if constexpr (sizeof(qsizetype) < sizeof(qint64)) {
        QSKIP("This overflow boundary requires a 64-bit qsizetype.");
    }

    rfm::ssh::detail::RemoteCopyTelemetryCounter counter;
    const qint64 largestWrite = std::numeric_limits<qint64>::max();
    const qsizetype largestRemaining = std::numeric_limits<qsizetype>::max();
    QVERIFY(counter.recordWrite(largestWrite, largestRemaining));
    QVERIFY(counter.recordWrite(largestWrite, largestRemaining));
    QVERIFY(counter.recordWrite(1, 1));
    QVERIFY(!counter.recordWrite(1, 1));
    QCOMPARE(counter.telemetry().transferredBytes, std::numeric_limits<quint64>::max());
}

} // namespace

QTEST_GUILESS_MAIN(SftpWriteLoopTest)
#include "test_sftp_write_loop.moc"
