#include "remotefilemanager/core/InternalTransfer.hpp"

#include "remotefilemanager/core/LocalFileSystem.hpp"
#include "remotefilemanager/core/RemotePath.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <initializer_list>
#include <limits>
#include <utility>

namespace rfm::core
{
namespace
{

constexpr int SchemaVersion = 2;
constexpr qsizetype MaximumPayloadBytes = 1024 * 1024;
constexpr qsizetype MaximumSources = 10000;

QString unsignedString(quint64 value) { return QString::number(value); }

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

QString normalizedLocalPath(const QString& path)
{
    if (path.trimmed().isEmpty() || path.contains(QChar{'\0'}) || !QFileInfo(path).isAbsolute()) {
        return {};
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool localPathsEqual(const QString& first, const QString& second)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    constexpr Qt::CaseSensitivity CaseSensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity CaseSensitivity = Qt::CaseSensitive;
#endif
    return first.compare(second, CaseSensitivity) == 0;
}

bool localEntryExists(const QFileInfo& info) { return info.exists() || info.isSymbolicLink(); }

QString physicalLocalEntryLocation(const QFileInfo& info)
{
    const QString canonicalParent = QFileInfo(info.absolutePath()).canonicalFilePath();
    return canonicalParent.isEmpty() ? QString{} : QDir(canonicalParent).filePath(info.fileName());
}

bool hasExactKeys(const QJsonObject& object, std::initializer_list<QString> keys)
{
    if (object.size() != static_cast<qsizetype>(keys.size())) {
        return false;
    }
    for (const QString& key : keys) {
        if (!object.contains(key)) {
            return false;
        }
    }
    return true;
}

QString sourceName(FileSource source)
{
    switch (source) {
    case FileSource::Local:
        return QStringLiteral("local");
    case FileSource::Ssh:
        return QStringLiteral("ssh");
    case FileSource::None:
        return {};
    }
    return {};
}

std::optional<FileSource> parsedSource(const QJsonValue& value)
{
    if (!value.isString()) {
        return std::nullopt;
    }
    if (value.toString() == QStringLiteral("local")) {
        return FileSource::Local;
    }
    if (value.toString() == QStringLiteral("ssh")) {
        return FileSource::Ssh;
    }
    return std::nullopt;
}

QString normalizedTransferPath(FileSource source, const QString& path)
{
    return source == FileSource::Local ? normalizedLocalPath(path) : RemotePath::normalize(path);
}

InternalTransferValidation validateLocalTransfer(const InternalTransferPayload& payload,
                                                 const BrowserLocation& destination)
{
    const QString destinationPath = normalizedLocalPath(destination.path);
    const QFileInfo destinationInfo(destinationPath);
    const QString canonicalDestination = destinationInfo.canonicalFilePath();
    if (destinationPath.isEmpty() || !destinationInfo.exists() || !destinationInfo.isDir() ||
        !destinationInfo.isWritable() || canonicalDestination.isEmpty()) {
        return {InternalTransferValidationError::InvalidDestination};
    }

    for (const RemoteSelection& source : payload.sources) {
        const QString sourcePath = normalizedLocalPath(source.path);
        const QFileInfo sourceInfo(sourcePath);
        if (sourcePath.isEmpty() || !localEntryExists(sourceInfo) ||
            source.directory != sourceInfo.isDir()) {
            return {InternalTransferValidationError::InvalidSource};
        }
        const QString finalPath = QDir(destinationPath).filePath(sourceInfo.fileName());
        if (localPathsEqual(sourcePath, QDir::cleanPath(finalPath))) {
            return {InternalTransferValidationError::IdenticalSourceAndDestination};
        }

        const QString canonicalSource = physicalLocalEntryLocation(sourceInfo);
        const QFileInfo finalInfo(finalPath);
        const QString canonicalFinal =
            localEntryExists(finalInfo)
                ? physicalLocalEntryLocation(finalInfo)
                : QDir(canonicalDestination).filePath(sourceInfo.fileName());
        if (canonicalSource.isEmpty()) {
            return {InternalTransferValidationError::InvalidSource};
        }
        if (canonicalFinal.isEmpty()) {
            return {InternalTransferValidationError::InvalidDestination};
        }
        if (localPathsEqual(canonicalSource, canonicalFinal)) {
            return {InternalTransferValidationError::IdenticalSourceAndDestination};
        }
        if (source.directory && !sourceInfo.isSymbolicLink() &&
            rfm::core::localPathIsAtOrBelow(canonicalDestination, canonicalSource)) {
            return {InternalTransferValidationError::DestinationInsideSource};
        }
    }
    return {};
}

} // namespace

bool RemoteConnectionIdentity::isValid() const
{
    return !host.trimmed().isEmpty() && port != 0 && generation != 0;
}

bool InternalTransferPayload::isValid() const
{
    const bool validIdentity = source == FileSource::Local
                                   ? sourceMachineId == QString::fromLatin1(LocalMachineId) &&
                                         connection == RemoteConnectionIdentity{}
                                   : source == FileSource::Ssh && !sourceMachineId.isEmpty() &&
                                         sourceMachineId == sourceMachineId.trimmed() &&
                                         sourceMachineId.size() <= 256 && connection.isValid();
    if (!validIdentity || applicationInstanceId.isEmpty() || applicationInstanceId.size() > 128 ||
        sourcePaneId == 0 || sources.isEmpty() || sources.size() > MaximumSources) {
        return false;
    }
    for (const RemoteSelection& source : sources) {
        const bool validPath =
            this->source == FileSource::Local
                ? !normalizedLocalPath(source.path).isEmpty()
                : validRemotePath(source.path) && !RemotePath::isProtected(source.path);
        if (!validPath) {
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
        value.insert(QStringLiteral("path"), normalizedTransferPath(payload.source, source.path));
        value.insert(QStringLiteral("directory"), source.directory);
        sources.push_back(value);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), SchemaVersion);
    root.insert(QStringLiteral("source"), sourceName(payload.source));
    root.insert(QStringLiteral("machine"), payload.sourceMachineId);
    root.insert(QStringLiteral("instance"), payload.applicationInstanceId);
    if (payload.source == FileSource::Ssh) {
        QJsonObject connection;
        connection.insert(QStringLiteral("host"), payload.connection.host.trimmed());
        connection.insert(QStringLiteral("port"), static_cast<int>(payload.connection.port));
        connection.insert(QStringLiteral("generation"),
                          unsignedString(payload.connection.generation));
        root.insert(QStringLiteral("connection"), connection);
    } else {
        root.insert(QStringLiteral("connection"), QJsonValue::Null);
    }
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
    if (!hasExactKeys(root, {QStringLiteral("version"), QStringLiteral("source"),
                             QStringLiteral("machine"), QStringLiteral("instance"),
                             QStringLiteral("connection"), QStringLiteral("sourcePane"),
                             QStringLiteral("sources")}) ||
        root.value(QStringLiteral("version")).toInt(-1) != SchemaVersion ||
        !root.value(QStringLiteral("machine")).isString() ||
        !root.value(QStringLiteral("instance")).isString() ||
        !root.value(QStringLiteral("sources")).isArray()) {
        return std::nullopt;
    }
    const auto source = parsedSource(root.value(QStringLiteral("source")));
    const auto sourcePaneId = parseUnsigned(root.value(QStringLiteral("sourcePane")));
    if (!source.has_value() || !sourcePaneId.has_value()) {
        return std::nullopt;
    }

    InternalTransferPayload payload;
    payload.source = *source;
    payload.sourceMachineId = root.value(QStringLiteral("machine")).toString();
    payload.applicationInstanceId = root.value(QStringLiteral("instance")).toString();
    if (payload.source == FileSource::Ssh) {
        if (!root.value(QStringLiteral("connection")).isObject()) {
            return std::nullopt;
        }
        const QJsonObject connection = root.value(QStringLiteral("connection")).toObject();
        if (!hasExactKeys(connection, {QStringLiteral("host"), QStringLiteral("port"),
                                       QStringLiteral("generation")})) {
            return std::nullopt;
        }
        const int port = connection.value(QStringLiteral("port")).toInt(-1);
        const auto generation = parseUnsigned(connection.value(QStringLiteral("generation")));
        if (!connection.value(QStringLiteral("host")).isString() || port <= 0 ||
            port > std::numeric_limits<quint16>::max() || !generation.has_value()) {
            return std::nullopt;
        }
        payload.connection = {connection.value(QStringLiteral("host")).toString(),
                              static_cast<quint16>(port), *generation};
    } else if (!root.value(QStringLiteral("connection")).isNull()) {
        return std::nullopt;
    }
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
        if (!hasExactKeys(source, {QStringLiteral("path"), QStringLiteral("directory")}) ||
            !source.value(QStringLiteral("path")).isString() ||
            !source.value(QStringLiteral("directory")).isBool()) {
            return std::nullopt;
        }
        payload.sources.push_back({source.value(QStringLiteral("path")).toString(),
                                   source.value(QStringLiteral("directory")).toBool()});
    }
    return payload.isValid() ? std::optional<InternalTransferPayload>{payload} : std::nullopt;
}

InternalTransferValidation
validateInternalTransfer(const InternalTransferPayload& payload,
                         const QString& applicationInstanceId, const BrowserLocation& destination,
                         const RemoteConnectionIdentity& destinationConnection)
{
    if (!payload.isValid()) {
        return {InternalTransferValidationError::InvalidPayload};
    }
    if (payload.applicationInstanceId != applicationInstanceId) {
        return {InternalTransferValidationError::ForeignApplication};
    }
    if (!destination.isValid()) {
        return {InternalTransferValidationError::InvalidDestination};
    }
    if (payload.source != destination.source || payload.sourceMachineId != destination.machineId) {
        return {InternalTransferValidationError::IncompatibleSource};
    }
    if (payload.source == FileSource::Local) {
        return validateLocalTransfer(payload, destination);
    }
    if (!destinationConnection.isValid() || payload.connection != destinationConnection) {
        return {InternalTransferValidationError::IncompatibleConnection};
    }
    const QString destinationPath = RemotePath::normalize(destination.path);
    if (!validRemotePath(destinationPath)) {
        return {InternalTransferValidationError::InvalidDestination};
    }
    for (const RemoteSelection& source : payload.sources) {
        const QString sourcePath = RemotePath::normalize(source.path);
        if (sourcePath.startsWith(QChar{'/'}) != destinationPath.startsWith(QChar{'/'})) {
            return {InternalTransferValidationError::IncompatiblePathConvention};
        }
        const QString finalPath =
            RemotePath::join(destinationPath, RemotePath::fileName(sourcePath));
        if (sourcePath == RemotePath::normalize(finalPath)) {
            return {InternalTransferValidationError::IdenticalSourceAndDestination};
        }
        if (source.directory && (destinationPath == sourcePath ||
                                 destinationPath.startsWith(sourcePath + QChar{'/'}))) {
            return {InternalTransferValidationError::DestinationInsideSource};
        }
    }
    return {};
}

} // namespace rfm::core
