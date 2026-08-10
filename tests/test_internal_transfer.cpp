#include "remotefilemanager/core/InternalTransfer.hpp"

#include <QTest>

class InternalTransferTest final : public QObject
{
    Q_OBJECT

  private slots:
    void serializesOnlyBoundedInternalData();
    void clipboardReplacesAndClearsIntentions();
    void validatesSessionAndRemoteDestinations();
};

namespace
{

rfm::core::InternalTransferPayload payload()
{
    return {QStringLiteral("application-instance"),
            {QStringLiteral("server.example.test"), 22, 7},
            2,
            {{QStringLiteral("/srv/file.txt"), false},
             {QStringLiteral("/srv/folder"), true}}};
}

} // namespace

void InternalTransferTest::serializesOnlyBoundedInternalData()
{
    const QByteArray encoded = rfm::core::encodeInternalTransfer(payload());
    QVERIFY(!encoded.isEmpty());
    QVERIFY(encoded.contains("server.example.test"));
    QVERIFY(encoded.contains("/srv/file.txt"));
    QVERIFY(!encoded.contains("username"));
    QVERIFY(!encoded.contains("password"));
    QVERIFY(!encoded.contains("private"));

    const auto decoded = rfm::core::decodeInternalTransfer(encoded);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->applicationInstanceId, QStringLiteral("application-instance"));
    QCOMPARE(decoded->connection.host, QStringLiteral("server.example.test"));
    QCOMPARE(decoded->connection.port, quint16{22});
    QCOMPARE(decoded->connection.generation, quint64{7});
    QCOMPARE(decoded->sourcePaneId, quint64{2});
    QCOMPARE(decoded->sources.size(), 2);
    QVERIFY(decoded->sources.constLast().directory);

    QVERIFY(!rfm::core::decodeInternalTransfer(QByteArrayLiteral("not-json")).has_value());
    QByteArray unsupported = encoded;
    unsupported.replace("\"version\":1", "\"version\":2");
    QVERIFY(!rfm::core::decodeInternalTransfer(unsupported).has_value());
}

void InternalTransferTest::clipboardReplacesAndClearsIntentions()
{
    rfm::core::InternalClipboard clipboard;
    QVERIFY(!clipboard.hasContent());

    clipboard.set(rfm::core::InternalTransferAction::Move, payload());
    QVERIFY(clipboard.hasContent());
    QVERIFY(clipboard.isCut());

    auto replacement = payload();
    replacement.sources = {{QStringLiteral("/srv/replacement.txt"), false}};
    clipboard.set(rfm::core::InternalTransferAction::Copy, replacement);
    QVERIFY(clipboard.hasContent());
    QVERIFY(!clipboard.isCut());
    QCOMPARE(clipboard.content()->payload.sources.constFirst().path,
             QStringLiteral("/srv/replacement.txt"));

    clipboard.clear();
    QVERIFY(!clipboard.hasContent());
}

void InternalTransferTest::validatesSessionAndRemoteDestinations()
{
    using Error = rfm::core::InternalTransferValidationError;
    const auto source = payload();
    const rfm::core::RemoteConnectionIdentity connection{
        QStringLiteral("server.example.test"), 22, 7};

    QVERIFY(rfm::core::validateInternalTransfer(source, QStringLiteral("application-instance"),
                                                connection, QStringLiteral("/destination"))
                .accepted());
    QCOMPARE(rfm::core::validateInternalTransfer(source, QStringLiteral("another-instance"),
                                                 connection, QStringLiteral("/destination"))
                 .error,
             Error::ForeignApplication);
    QCOMPARE(rfm::core::validateInternalTransfer(
                 source, QStringLiteral("application-instance"),
                 {QStringLiteral("other.example.test"), 22, 8},
                 QStringLiteral("/destination"))
                 .error,
             Error::IncompatibleConnection);
    QCOMPARE(rfm::core::validateInternalTransfer(source, QStringLiteral("application-instance"),
                                                 connection, QStringLiteral("relative"))
                 .error,
             Error::IncompatiblePathConvention);
    QCOMPARE(rfm::core::validateInternalTransfer(source, QStringLiteral("application-instance"),
                                                 connection, QStringLiteral("/srv"))
                 .error,
             Error::IdenticalSourceAndDestination);
    QCOMPARE(rfm::core::validateInternalTransfer(source, QStringLiteral("application-instance"),
                                                 connection, QStringLiteral("/srv/folder/child"))
                 .error,
             Error::DestinationInsideSource);
    QCOMPARE(rfm::core::validateInternalTransfer(source, QStringLiteral("application-instance"),
                                                 connection, QStringLiteral("../invalid"))
                 .error,
             Error::InvalidDestination);
}

QTEST_APPLESS_MAIN(InternalTransferTest)

#include "test_internal_transfer.moc"
