#include "remotefilemanager/core/ServerCapabilitiesStore.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace rfm::core
{
namespace
{

constexpr qint64 MaximumFileSize = 4 * 1024 * 1024;
constexpr qsizetype MaximumSnapshots = 1024;
constexpr qsizetype MaximumExtensions = 1024;
constexpr qsizetype MaximumIdentityLength = 1024;
constexpr qsizetype MaximumExtensionNameLength = 1024;
constexpr qsizetype MaximumExtensionDataLength = 16 * 1024;

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

bool validIdentityField(const QString& value)
{
    return !value.trimmed().isEmpty() && value == value.trimmed() &&
           value.size() <= MaximumIdentityLength;
}

bool validExtension(const SftpExtensionCapability& extension)
{
    return !extension.name.isEmpty() && extension.name.size() <= MaximumExtensionNameLength &&
           extension.data.size() <= MaximumExtensionDataLength;
}

QJsonObject serialize(const PersistedServerCapabilities& snapshot)
{
    QJsonArray extensions;
    for (const SftpExtensionCapability& extension : snapshot.capabilities.sftpExtensions) {
        extensions.push_back(QJsonObject{{QStringLiteral("name"), extension.name},
                                         {QStringLiteral("data"), extension.data}});
    }
    QJsonObject object{{QStringLiteral("profileId"), snapshot.profileId},
                       {QStringLiteral("host"), snapshot.host},
                       {QStringLiteral("username"), snapshot.username},
                       {QStringLiteral("port"), static_cast<int>(snapshot.port)},
                       {QStringLiteral("detectedAt"),
                        snapshot.capabilities.detectedAt.toUTC().toString(Qt::ISODateWithMs)},
                       {QStringLiteral("extensions"), extensions}};
    if (snapshot.capabilities.sftpProtocolVersion.has_value()) {
        object.insert(QStringLiteral("sftpProtocolVersion"),
                      *snapshot.capabilities.sftpProtocolVersion);
    }
    return object;
}

std::optional<PersistedServerCapabilities> deserialize(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();
    const QJsonValue portValue = object.value(QStringLiteral("port"));
    const QJsonValue detectedAtValue = object.value(QStringLiteral("detectedAt"));
    const QJsonValue extensionsValue = object.value(QStringLiteral("extensions"));
    if (!portValue.isDouble() || !detectedAtValue.isString() || !extensionsValue.isArray()) {
        return std::nullopt;
    }
    const int port = portValue.toInt(0);
    if (port <= 0 || port > std::numeric_limits<quint16>::max()) {
        return std::nullopt;
    }
    QDateTime detectedAt = QDateTime::fromString(detectedAtValue.toString(), Qt::ISODateWithMs);
    if (!detectedAt.isValid()) {
        detectedAt = QDateTime::fromString(detectedAtValue.toString(), Qt::ISODate);
    }
    if (!detectedAt.isValid()) {
        return std::nullopt;
    }

    std::optional<int> sftpProtocolVersion;
    const QJsonValue versionValue = object.value(QStringLiteral("sftpProtocolVersion"));
    if (!versionValue.isUndefined()) {
        if (!versionValue.isDouble()) {
            return std::nullopt;
        }
        const int version = versionValue.toInt(-1);
        if (version < 0) {
            return std::nullopt;
        }
        sftpProtocolVersion = version;
    }

    const QJsonArray serializedExtensions = extensionsValue.toArray();
    if (serializedExtensions.size() > MaximumExtensions) {
        return std::nullopt;
    }
    QList<SftpExtensionCapability> extensions;
    extensions.reserve(serializedExtensions.size());
    for (const QJsonValue& extensionValue : serializedExtensions) {
        if (!extensionValue.isObject()) {
            return std::nullopt;
        }
        const QJsonObject extensionObject = extensionValue.toObject();
        const QJsonValue nameValue = extensionObject.value(QStringLiteral("name"));
        QJsonValue dataValue = extensionObject.value(QStringLiteral("data"));
        if (dataValue.isUndefined()) {
            dataValue = QString{};
        }
        if (!nameValue.isString() || !dataValue.isString()) {
            return std::nullopt;
        }
        SftpExtensionCapability extension{nameValue.toString(), dataValue.toString()};
        if (!validExtension(extension)) {
            return std::nullopt;
        }
        extensions.push_back(std::move(extension));
    }

    PersistedServerCapabilities snapshot;
    snapshot.profileId = object.value(QStringLiteral("profileId")).toString();
    snapshot.host = object.value(QStringLiteral("host")).toString();
    snapshot.username = object.value(QStringLiteral("username")).toString();
    snapshot.port = static_cast<quint16>(port);
    snapshot.capabilities =
        detectedServerCapabilities(std::move(extensions), detectedAt.toUTC(), sftpProtocolVersion);
    return snapshot.isValid() ? std::optional{std::move(snapshot)} : std::nullopt;
}

struct CapabilitiesLoadResult {
    QList<PersistedServerCapabilities> snapshots;
    QString error;
    bool rejectedEntries{false};
};

CapabilitiesLoadResult loadSnapshots(const QString& path)
{
    QFile file(path);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return {{}, QStringLiteral("Unable to read the server capabilities file."), false};
    }
    if (file.size() == 0 || file.size() > MaximumFileSize) {
        return {{}, QStringLiteral("The server capabilities file has an invalid size."), false};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {{}, QStringLiteral("The server capabilities file contains invalid JSON."), false};
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt(-1) != ServerCapabilitiesStore::formatVersion) {
        return {
            {}, QStringLiteral("The server capabilities file uses an unsupported version."), false};
    }
    const QJsonValue snapshotsValue = root.value(QStringLiteral("snapshots"));
    if (!snapshotsValue.isArray() || snapshotsValue.toArray().size() > MaximumSnapshots) {
        return {
            {}, QStringLiteral("The server capabilities file has an invalid structure."), false};
    }

    CapabilitiesLoadResult result;
    QSet<QString> profileIds;
    for (const QJsonValue& value : snapshotsValue.toArray()) {
        auto snapshot = deserialize(value);
        if (!snapshot.has_value() || profileIds.contains(snapshot->profileId)) {
            result.rejectedEntries = true;
            continue;
        }
        profileIds.insert(snapshot->profileId);
        result.snapshots.push_back(std::move(*snapshot));
    }
    return result;
}

} // namespace

bool PersistedServerCapabilities::isValid() const
{
    return validIdentityField(profileId) && validIdentityField(host) &&
           validIdentityField(username) && port != 0 &&
           capabilities.detectionState == CapabilityDetectionState::Detected &&
           capabilities.detectedAt.isValid() &&
           (!capabilities.sftpProtocolVersion.has_value() ||
            *capabilities.sftpProtocolVersion >= 0) &&
           capabilities.sftpExtensions.size() <= MaximumExtensions &&
           std::ranges::all_of(capabilities.sftpExtensions, validExtension);
}

ServerCapabilitiesStore::ServerCapabilitiesStore(QString storageDirectory)
    : m_storageDirectory(storageDirectory.isEmpty()
                             ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                             : std::move(storageDirectory))
{}

QString ServerCapabilitiesStore::filePath() const
{
    return QDir(m_storageDirectory).filePath(QStringLiteral("server-capabilities.json"));
}

QList<PersistedServerCapabilities> ServerCapabilitiesStore::load(QString* error) const
{
    CapabilitiesLoadResult result = loadSnapshots(filePath());
    setError(error, !result.error.isEmpty()
                        ? result.error
                        : (result.rejectedEntries
                               ? QStringLiteral("The server capabilities file contains invalid or "
                                                "duplicate entries; those entries were ignored.")
                               : QString{}));
    return std::move(result.snapshots);
}

bool ServerCapabilitiesStore::save(const QList<PersistedServerCapabilities>& snapshots,
                                   QString* error) const
{
    setError(error, {});
    if (snapshots.size() > MaximumSnapshots) {
        setError(error, QStringLiteral("Too many server capability snapshots."));
        return false;
    }
    QJsonArray serialized;
    QSet<QString> profileIds;
    for (const PersistedServerCapabilities& snapshot : snapshots) {
        if (!snapshot.isValid() || profileIds.contains(snapshot.profileId)) {
            setError(error,
                     QStringLiteral("A server capability snapshot is invalid or duplicated."));
            return false;
        }
        profileIds.insert(snapshot.profileId);
        serialized.push_back(serialize(snapshot));
    }
    const QJsonDocument document(QJsonObject{{QStringLiteral("version"), formatVersion},
                                             {QStringLiteral("snapshots"), serialized}});
    const QByteArray contents = document.toJson(QJsonDocument::Indented);
    if (contents.isEmpty()) {
        setError(error, QStringLiteral("The server capabilities file serialization is empty."));
        return false;
    }
    if (contents.size() > MaximumFileSize) {
        setError(error,
                 QStringLiteral("The server capabilities file would exceed the maximum allowed "
                                "size."));
        return false;
    }
    if (m_storageDirectory.isEmpty() || !QDir().mkpath(m_storageDirectory)) {
        setError(error, QStringLiteral("Unable to create the server capabilities directory."));
        return false;
    }
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() ||
        !file.commit()) {
        setError(error, QStringLiteral("Unable to save the server capabilities."));
        return false;
    }
    return true;
}

bool ServerCapabilitiesStore::upsert(const PersistedServerCapabilities& snapshot,
                                     QString* error) const
{
    if (!snapshot.isValid()) {
        setError(error, QStringLiteral("The server capability snapshot is invalid."));
        return false;
    }
    CapabilitiesLoadResult loaded = loadSnapshots(filePath());
    if (!loaded.error.isEmpty()) {
        setError(error, loaded.error);
        return false;
    }
    bool replaced = false;
    for (PersistedServerCapabilities& existing : loaded.snapshots) {
        if (existing.profileId == snapshot.profileId) {
            existing = snapshot;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        loaded.snapshots.push_back(snapshot);
    }
    return save(loaded.snapshots, error);
}

bool ServerCapabilitiesStore::remove(const QString& profileId, QString* error) const
{
    const QString normalizedId = profileId.trimmed();
    if (!validIdentityField(normalizedId)) {
        setError(error, QStringLiteral("The server profile identifier is invalid."));
        return false;
    }
    CapabilitiesLoadResult loaded = loadSnapshots(filePath());
    if (!loaded.error.isEmpty()) {
        setError(error, loaded.error);
        return false;
    }
    const qsizetype originalSize = loaded.snapshots.size();
    loaded.snapshots.removeIf([&normalizedId](const PersistedServerCapabilities& snapshot) {
        return snapshot.profileId == normalizedId;
    });
    if (loaded.snapshots.size() == originalSize && !loaded.rejectedEntries) {
        setError(error, {});
        return true;
    }
    return save(loaded.snapshots, error);
}

} // namespace rfm::core
