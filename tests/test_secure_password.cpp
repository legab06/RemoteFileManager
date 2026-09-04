#include "remotefilemanager/core/SecurePassword.hpp"
#include "remotefilemanager/ssh/SshSession.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QTest>

#include <string_view>
#include <type_traits>
#include <utility>

static_assert(!std::is_copy_constructible_v<rfm::core::SecurePassword>);
static_assert(!std::is_copy_assignable_v<rfm::core::SecurePassword>);
static_assert(std::is_nothrow_move_constructible_v<rfm::core::SecurePassword>);
static_assert(std::is_nothrow_move_assignable_v<rfm::core::SecurePassword>);

class SecurePasswordTest final : public QObject
{
    Q_OBJECT

  private slots:
    void encodesDirectlyIntoUniqueStorage();
    void moveTransfersTheAllocation();
    void clearWipesTheControlledCapacity();
    void partialWritesRetainThenWipeTheSecret();
    void queuedHandoffConsumesUniqueOwnership();
    void connectionPasswordHandoffConsumesUniqueOwnership();
    void destructorAndHandoffDoNotUseSharedPasswordPayloads();
};

void SecurePasswordTest::encodesDirectlyIntoUniqueStorage()
{
    auto password = rfm::core::SecurePassword::fromUtf16(QStringLiteral("p\u00e4ss\U0001f512"));
    QVERIFY(!password.isEmpty());
    const std::string_view bytes(password.remainingData(), password.remainingSize());
    QCOMPARE(bytes, std::string_view("p\xc3\xa4ss\xf0\x9f\x94\x92", 9));
    QCOMPARE(password.remainingData()[password.remainingSize()], '\0');
    QVERIFY(!password.storageIsWiped());
}

void SecurePasswordTest::moveTransfersTheAllocation()
{
    auto source = rfm::core::SecurePassword::fromUtf16(QStringLiteral("move-only fixture"));
    const char* const allocation = source.remainingData();
    auto destination = std::move(source);

    QVERIFY(source.isEmpty());
    QVERIFY(source.remainingData() == nullptr);
    QVERIFY(destination.remainingData() == allocation);
    QVERIFY(!destination.isEmpty());
}

void SecurePasswordTest::clearWipesTheControlledCapacity()
{
    auto password = rfm::core::SecurePassword::fromUtf16(QStringLiteral("wipe fixture"));
    QVERIFY(!password.storageIsWiped());
    password.clear();
    QVERIFY(password.isEmpty());
    QVERIFY(password.storageIsWiped());
}

void SecurePasswordTest::partialWritesRetainThenWipeTheSecret()
{
    auto password = rfm::core::SecurePassword::fromUtf16(QStringLiteral("partial"));
    QVERIFY(password.appendLineFeed());
    const std::size_t completeSize = password.remainingSize();
    QVERIFY(completeSize > 2);
    QVERIFY(!password.consumeWritten(2));
    QCOMPARE(password.remainingSize(), completeSize - 2);
    QVERIFY(!password.storageIsWiped());

    QVERIFY(password.consumeWritten(password.remainingSize()));
    QVERIFY(password.isEmpty());
    QVERIFY(password.storageIsWiped());
}

void SecurePasswordTest::queuedHandoffConsumesUniqueOwnership()
{
    rfm::ssh::SshSession session;
    auto password = rfm::core::SecurePassword::fromUtf16(QStringLiteral("queued fixture"));
    const char* const allocation = password.remainingData();
    QVERIFY(allocation != nullptr);

    session.postVolumeAuthentication(17, 23, std::move(password));
    QVERIFY(password.isEmpty());
    QVERIFY(password.remainingData() == nullptr);
    QCoreApplication::sendPostedEvents(&session);
}

void SecurePasswordTest::connectionPasswordHandoffConsumesUniqueOwnership()
{
    rfm::ssh::SshSession session;
    auto password = rfm::core::SecurePassword::fromUtf16(QStringLiteral("connection fixture"));
    session.postPasswordAuthentication(std::move(password));
    QVERIFY(password.isEmpty());
    QCoreApplication::sendPostedEvents(&session);
}

void SecurePasswordTest::destructorAndHandoffDoNotUseSharedPasswordPayloads()
{
    QFile implementation(QStringLiteral(RFM_SOURCE_DIR "/src/core/SecurePassword.cpp"));
    QVERIFY(implementation.open(QIODevice::ReadOnly));
    const QByteArray secureImplementation = implementation.readAll();
    QVERIFY(secureImplementation.contains("SecurePassword::~SecurePassword() { clear(); }"));
    QVERIFY(secureImplementation.contains("wipeMemory(m_storage.get(), m_capacity)"));

    QFile windowHeader(
        QStringLiteral(RFM_SOURCE_DIR "/include/remotefilemanager/app/MainWindow.hpp"));
    QVERIFY(windowHeader.open(QIODevice::ReadOnly));
    const QByteArray mainWindowApi = windowHeader.readAll();
    QVERIFY(!mainWindowApi.contains("remoteVolumeAuthenticationRequested"));

    QFile sessionHeader(
        QStringLiteral(RFM_SOURCE_DIR "/include/remotefilemanager/ssh/SshSession.hpp"));
    QVERIFY(sessionHeader.open(QIODevice::ReadOnly));
    const QByteArray sessionApi = sessionHeader.readAll();
    QVERIFY(sessionApi.contains("postVolumeAuthentication"));
    QVERIFY(sessionApi.contains("postPasswordAuthentication"));
    QVERIFY(sessionApi.contains("rfm::core::SecurePassword password"));
    QVERIFY(!sessionApi.contains("connectToHost(rfm::core::ConnectionProfile profile, QString"));
    QVERIFY(!sessionApi.contains("authenticationToken, QByteArray password"));
}

QTEST_GUILESS_MAIN(SecurePasswordTest)

#include "test_secure_password.moc"
