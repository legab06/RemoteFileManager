#include "remotefilemanager/core/ServerProfileStore.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <limits>
#include <optional>
#include <utility>

namespace rfm::core
{
namespace
{

constexpr qint64 maximumFileSize = 1024 * 1024;

QJsonObject serialize(const ConnectionProfile& profile)
{
    return {{QStringLiteral("id"), profile.id.trimmed()},
            {QStringLiteral("name"), profile.displayName.trimmed()},
            {QStringLiteral("host"), profile.host.trimmed()},
            {QStringLiteral("username"), profile.username.trimmed()},
            {QStringLiteral("port"), static_cast<int>(profile.port)},
            {QStringLiteral("allowPasswordFallback"), profile.allowPasswordFallback}};
}

std::optional<ConnectionProfile> deserialize(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();
    const QJsonValue portValue = object.value(QStringLiteral("port"));
    const QJsonValue fallbackValue = object.value(QStringLiteral("allowPasswordFallback"));
    if (!portValue.isDouble() || !fallbackValue.isBool()) {
        return std::nullopt;
    }
    const int port = portValue.toInt(0);
    if (port <= 0 || port > std::numeric_limits<quint16>::max()) {
        return std::nullopt;
    }

    ConnectionProfile profile;
    profile.id = object.value(QStringLiteral("id")).toString().trimmed();
    profile.displayName = object.value(QStringLiteral("name")).toString().trimmed();
    profile.host = object.value(QStringLiteral("host")).toString().trimmed();
    profile.username = object.value(QStringLiteral("username")).toString().trimmed();
    profile.port = static_cast<quint16>(port);
    profile.allowPasswordFallback = fallbackValue.toBool();
    return profile.isValidSavedProfile() ? std::optional{profile} : std::nullopt;
}

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

ServerProfileStore::ServerProfileStore(QString storageDirectory)
    : m_storageDirectory(storageDirectory.isEmpty()
                             ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                             : std::move(storageDirectory))
{
}

QString ServerProfileStore::filePath() const
{
    return QDir(m_storageDirectory).filePath(QStringLiteral("server-profiles.json"));
}

QList<ConnectionProfile> ServerProfileStore::load(QString* error) const
{
    setError(error, {});
    QFile file(filePath());
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Unable to read the server profiles file."));
        return {};
    }
    if (file.size() == 0 || file.size() > maximumFileSize) {
        setError(error, QStringLiteral("The server profiles file has an invalid size."));
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("The server profiles file contains invalid JSON."));
        return {};
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt(-1) != formatVersion) {
        setError(error, QStringLiteral("The server profiles file uses an unsupported version."));
        return {};
    }
    if (!root.value(QStringLiteral("servers")).isArray()) {
        setError(error, QStringLiteral("The server profiles file has an invalid structure."));
        return {};
    }

    QList<ConnectionProfile> profiles;
    QSet<QString> identifiers;
    for (const QJsonValue& value : root.value(QStringLiteral("servers")).toArray()) {
        const auto profile = deserialize(value);
        if (profile.has_value() && !identifiers.contains(profile->id)) {
            identifiers.insert(profile->id);
            profiles.push_back(*profile);
        }
    }
    return profiles;
}

bool ServerProfileStore::save(const QList<ConnectionProfile>& profiles, QString* error) const
{
    setError(error, {});
    QJsonArray serialized;
    QSet<QString> identifiers;
    for (const ConnectionProfile& profile : profiles) {
        if (!profile.isValidSavedProfile() || identifiers.contains(profile.id.trimmed())) {
            setError(error, QStringLiteral("A server profile is invalid or duplicated."));
            return false;
        }
        identifiers.insert(profile.id.trimmed());
        serialized.push_back(serialize(profile));
    }
    if (m_storageDirectory.isEmpty() || !QDir().mkpath(m_storageDirectory)) {
        setError(error, QStringLiteral("Unable to create the server profiles directory."));
        return false;
    }

    const QJsonDocument document(
        QJsonObject{{QStringLiteral("version"), formatVersion},
                    {QStringLiteral("servers"), serialized}});
    const QByteArray contents = document.toJson(QJsonDocument::Indented);
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() ||
        !file.commit()) {
        setError(error, QStringLiteral("Unable to save the server profiles."));
        return false;
    }
    return true;
}

bool ServerProfileStore::upsert(const ConnectionProfile& profile, QString* error) const
{
    if (!profile.isValidSavedProfile()) {
        setError(error, QStringLiteral("The server profile is invalid."));
        return false;
    }
    QString loadError;
    QList<ConnectionProfile> profiles = load(&loadError);
    if (!loadError.isEmpty()) {
        setError(error, loadError);
        return false;
    }
    bool replaced = false;
    for (ConnectionProfile& existing : profiles) {
        if (existing.id == profile.id) {
            existing = profile;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        profiles.push_back(profile);
    }
    return save(profiles, error);
}

bool ServerProfileStore::remove(const QString& id, QString* error) const
{
    const QString normalizedId = id.trimmed();
    if (normalizedId.isEmpty()) {
        setError(error, QStringLiteral("The server profile identifier is invalid."));
        return false;
    }
    QString loadError;
    QList<ConnectionProfile> profiles = load(&loadError);
    if (!loadError.isEmpty()) {
        setError(error, loadError);
        return false;
    }
    const qsizetype originalSize = profiles.size();
    profiles.removeIf([&normalizedId](const ConnectionProfile& profile) {
        return profile.id == normalizedId;
    });
    if (profiles.size() == originalSize) {
        setError(error, QStringLiteral("The server profile was not found."));
        return false;
    }
    return save(profiles, error);
}

} // namespace rfm::core
