#include "remotefilemanager/core/InternalTransfer.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <limits>
#include <utility>

namespace rfm::core
{
namespace
{

constexpr int SchemaVersion = 1;
constexpr qsizetype MaximumPayloadBytes = 1024 * 1024;
constexpr qsizetype MaximumSources = 10000;

QString unsignedString(quint64 value)
{
    return QString::number(value);
}

std::optional<quint64> parseUnsigned(const QJsonValue& value)
{
    if (!value.isString()) {
        return std::nullopt;
    }
    bool ok = false;
    const quint64 parsed = value.toString().toULongLong(&ok);
    return ok ? std::optional<quint64>{parsed} : std::nullopt;
}

bool validRemotePath(const QString& path)
{
    const QString normalized = RemotePath::normalize(path);
    return !normalized.isEmpty() && normalized != QStringLiteral("..") &&
           !normalized.startsWith(QStringLiteral("../"));
}

} // namespace

bool RemoteConnectionIdentity::isValid() const
{
    return !host.trimmed().isEmpty() && port != 0 && generation != 0;
}

bool InternalTransferPayload::isValid() const
{
    const bool localPayload = applicationInstanceId.isEmpty() && !connection.isValid();
    if ((!localPayload && (applicationInstanceId.isEmpty() || applicationInstanceId.size() > 128 ||
                           !connection.isValid())) ||
        sourcePaneId == 0 || sources.isEmpty() || sources.size() > MaximumSources) {
        return false;
    }
    for (const RemoteSelection& source : sources) {
        if (!validRemotePath(source.path) || RemotePath::isProtected(source.path)) {
            return false;
        }
    }
    return true;
}

void InternalClipboard::set(InternalTransferAction action, InternalTransferPayload payload)
{
    if (!payload.isValid()) {
        clear();
        return;
    }
    ++m_generation;
    m_content = ClipboardEntry{action, std::move(payload)};
}

void InternalClipboard::clear()
{
    ++m_generation;
    m_content.reset();
}

bool InternalClipboard::hasContent() const { return m_content.has_value(); }

bool InternalClipboard::isCut() const
{
    return m_content.has_value() && m_content->action == InternalTransferAction::Move;
}

bool InternalClipboard::matchesCutGeneration(quint64 generation) const
{
    return isCut() && m_generation == generation;
}

quint64 InternalClipboard::generation() const { return m_generation; }

const std::optional<ClipboardEntry>& InternalClipboard::content() const { return m_content; }

QByteArray encodeInternalTransfer(const InternalTransferPayload& payload)
{
    if (!payload.isValid()) {
        return {};
    }
    QJsonArray sources;
    for (const RemoteSelection& source : payload.sources) {
        QJsonObject value;
        value.insert(QStringLiteral("path"), RemotePath::normalize(source.path));
        value.insert(QStringLiteral("directory"), source.directory);
        sources.push_back(value);
    }
    QJsonObject connection;
    connection.insert(QStringLiteral("host"), payload.connection.host.trimmed());
    connection.insert(QStringLiteral("port"), static_cast<int>(payload.connection.port));
    connection.insert(QStringLiteral("generation"),
                      unsignedString(payload.connection.generation));

    QJsonObject root;
    root.insert(QStringLiteral("version"), SchemaVersion);
    root.insert(QStringLiteral("instance"), payload.applicationInstanceId);
    root.insert(QStringLiteral("connection"), connection);
    root.insert(QStringLiteral("sourcePane"), unsignedString(payload.sourcePaneId));
    root.insert(QStringLiteral("sources"), sources);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::optional<InternalTransferPayload> decodeInternalTransfer(const QByteArray& data)
{
    if (data.isEmpty() || data.size() > MaximumPayloadBytes) {
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt(-1) != SchemaVersion ||
        !root.value(QStringLiteral("connection")).isObject() ||
        !root.value(QStringLiteral("sources")).isArray()) {
        return std::nullopt;
    }
    const QJsonObject connection = root.value(QStringLiteral("connection")).toObject();
    const int port = connection.value(QStringLiteral("port")).toInt(-1);
    const auto generation = parseUnsigned(connection.value(QStringLiteral("generation")));
    const auto sourcePaneId = parseUnsigned(root.value(QStringLiteral("sourcePane")));
    if (port <= 0 || port > std::numeric_limits<quint16>::max() || !generation.has_value() ||
        !sourcePaneId.has_value()) {
        return std::nullopt;
    }

    InternalTransferPayload payload;
    payload.applicationInstanceId = root.value(QStringLiteral("instance")).toString();
    payload.connection = {connection.value(QStringLiteral("host")).toString(),
                          static_cast<quint16>(port), *generation};
    payload.sourcePaneId = *sourcePaneId;
    const QJsonArray sources = root.value(QStringLiteral("sources")).toArray();
    if (sources.size() > MaximumSources) {
        return std::nullopt;
    }
    for (const QJsonValue& value : sources) {
        if (!value.isObject()) {
            return std::nullopt;
        }
        const QJsonObject source = value.toObject();
        if (!source.value(QStringLiteral("path")).isString() ||
            !source.value(QStringLiteral("directory")).isBool()) {
            return std::nullopt;
        }
        payload.sources.push_back({source.value(QStringLiteral("path")).toString(),
                                   source.value(QStringLiteral("directory")).toBool()});
    }
    return payload.isValid() ? std::optional<InternalTransferPayload>{payload} : std::nullopt;
}

InternalTransferValidation validateInternalTransfer(
    const InternalTransferPayload& payload, const QString& applicationInstanceId,
    const RemoteConnectionIdentity& destinationConnection, const QString& destinationDirectory)
{
    if (!payload.isValid()) {
        return {InternalTransferValidationError::InvalidPayload};
    }
    if (payload.applicationInstanceId != applicationInstanceId) {
        return {InternalTransferValidationError::ForeignApplication};
    }
    if (!destinationConnection.isValid() || payload.connection != destinationConnection) {
        return {InternalTransferValidationError::IncompatibleConnection};
    }
    const QString destination = RemotePath::normalize(destinationDirectory);
    if (!validRemotePath(destination)) {
        return {InternalTransferValidationError::InvalidDestination};
    }
    for (const RemoteSelection& source : payload.sources) {
        const QString sourcePath = RemotePath::normalize(source.path);
        if (sourcePath.startsWith(QChar{'/'}) != destination.startsWith(QChar{'/'})) {
            return {InternalTransferValidationError::IncompatiblePathConvention};
        }
        const QString finalPath = RemotePath::join(destination, RemotePath::fileName(sourcePath));
        if (sourcePath == RemotePath::normalize(finalPath)) {
            return {InternalTransferValidationError::IdenticalSourceAndDestination};
        }
        if (source.directory &&
            (destination == sourcePath || destination.startsWith(sourcePath + QChar{'/'}))) {
            return {InternalTransferValidationError::DestinationInsideSource};
        }
    }
    return {};
}

} // namespace rfm::core
