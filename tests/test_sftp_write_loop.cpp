#include "../src/ssh/SftpWriteLoop.hpp"

#include <QList>
#include <QTest>

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

} // namespace

QTEST_GUILESS_MAIN(SftpWriteLoopTest)
#include "test_sftp_write_loop.moc"
