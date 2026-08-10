#include "remotefilemanager/core/OperationHistoryStore.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <optional>
#include <utility>

namespace rfm::core
{
namespace
{

constexpr qint64 maximumFileSize = 4 * 1024 * 1024;

QString kindName(OperationKind kind)
{
    switch (kind) {
    case OperationKind::Upload:
        return QStringLiteral("upload");
    case OperationKind::Download:
        return QStringLiteral("download");
    case OperationKind::RemoteCopy:
        return QStringLiteral("remote-copy");
    case OperationKind::RemoteMove:
        return QStringLiteral("remote-move");
    }
    return {};
}

std::optional<OperationKind> parseKind(const QString& name)
{
    if (name == QStringLiteral("upload")) {
        return OperationKind::Upload;
    }
    if (name == QStringLiteral("download")) {
        return OperationKind::Download;
    }
    if (name == QStringLiteral("remote-copy")) {
        return OperationKind::RemoteCopy;
    }
    if (name == QStringLiteral("remote-move")) {
        return OperationKind::RemoteMove;
    }
    return std::nullopt;
}

QString stateName(OperationState state)
{
    switch (state) {
    case OperationState::Completed:
        return QStringLiteral("completed");
    case OperationState::Cancelled:
        return QStringLiteral("cancelled");
    case OperationState::Failed:
        return QStringLiteral("failed");
    default:
        return {};
    }
}

std::optional<OperationState> parseTerminalState(const QString& name)
{
    if (name == QStringLiteral("completed")) {
        return OperationState::Completed;
    }
    if (name == QStringLiteral("cancelled")) {
        return OperationState::Cancelled;
    }
    if (name == QStringLiteral("failed")) {
        return OperationState::Failed;
    }
    return std::nullopt;
}

QString numberString(quint64 value)
{
    return QString::number(value);
}

std::optional<quint64> parseNumber(const QJsonValue& value)
{
    bool valid = false;
    const quint64 result = value.toString().toULongLong(&valid);
    if (!valid) {
        return std::nullopt;
    }
    return result;
}

QJsonObject serialize(const OperationProgress& operation)
{
    QJsonArray sources;
    for (const QString& source : operation.sources) {
        sources.push_back(source);
    }
    return {{QStringLiteral("id"), numberString(operation.id)},
            {QStringLiteral("kind"), kindName(operation.kind)},
            {QStringLiteral("sources"), sources},
            {QStringLiteral("destination"), operation.destination},
            {QStringLiteral("state"), stateName(operation.state)},
            {QStringLiteral("error"), operation.error},
            {QStringLiteral("transferredBytes"), numberString(operation.transferredBytes)},
            {QStringLiteral("totalBytes"), numberString(operation.totalBytes)},
            {QStringLiteral("completedItems"), numberString(operation.completedItems)},
            {QStringLiteral("totalItems"), numberString(operation.totalItems)},
            {QStringLiteral("finishedAt"),
             operation.finishedAt.toUTC().toString(Qt::ISODateWithMs)}};
}

std::optional<OperationProgress> deserialize(const QJsonValue& value)
{
    if (!value.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = value.toObject();
    const auto id = parseNumber(object.value(QStringLiteral("id")));
    const auto kind = parseKind(object.value(QStringLiteral("kind")).toString());
    const auto state =
        parseTerminalState(object.value(QStringLiteral("state")).toString());
    const QDateTime finishedAt = QDateTime::fromString(
        object.value(QStringLiteral("finishedAt")).toString(), Qt::ISODateWithMs);
    if (!id.has_value() || *id == 0 || !kind.has_value() || !state.has_value() ||
        !finishedAt.isValid()) {
        return std::nullopt;
    }

    OperationProgress operation;
    operation.id = *id;
    operation.kind = *kind;
    operation.state = *state;
    const QJsonArray sources = object.value(QStringLiteral("sources")).toArray();
    for (const QJsonValue& source : sources) {
        if (source.isString()) {
            operation.sources.push_back(source.toString());
        }
    }
    operation.destination = object.value(QStringLiteral("destination")).toString();
    operation.error = object.value(QStringLiteral("error")).toString();
    operation.transferredBytes =
        parseNumber(object.value(QStringLiteral("transferredBytes"))).value_or(0);
    operation.totalBytes =
        parseNumber(object.value(QStringLiteral("totalBytes"))).value_or(0);
    operation.completedItems =
        parseNumber(object.value(QStringLiteral("completedItems"))).value_or(0);
    operation.totalItems =
        parseNumber(object.value(QStringLiteral("totalItems"))).value_or(0);
    operation.finishedAt = finishedAt.toUTC();
    const bool transfer = operation.kind == OperationKind::Upload ||
                          operation.kind == OperationKind::Download;
    operation.byteProgressAvailable = transfer;
    operation.pauseResumeSupported = transfer;
    operation.cancellationSupported = transfer;
    return operation;
}

} // namespace

OperationHistoryStore::OperationHistoryStore(QString storageDirectory)
    : m_storageDirectory(storageDirectory.isEmpty()
                             ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                             : std::move(storageDirectory))
{
}

QString OperationHistoryStore::filePath() const
{
    return QDir(m_storageDirectory).filePath(QStringLiteral("operation-history.json"));
}

QList<OperationProgress> OperationHistoryStore::load() const
{
    QFile file(filePath());
    if (!file.exists() || !file.open(QIODevice::ReadOnly) || file.size() == 0 ||
        file.size() > maximumFileSize) {
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt(-1) != formatVersion ||
        !root.value(QStringLiteral("operations")).isArray()) {
        return {};
    }

    QList<OperationProgress> operations;
    for (const QJsonValue& value : root.value(QStringLiteral("operations")).toArray()) {
        if (const auto operation = deserialize(value); operation.has_value()) {
            operations.push_back(*operation);
        }
    }
    return retainedTerminalOperations(operations);
}

bool OperationHistoryStore::save(const QList<OperationProgress>& operations, QString* error) const
{
    if (m_storageDirectory.isEmpty() || !QDir().mkpath(m_storageDirectory)) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to create the operation history directory.");
        }
        return false;
    }

    QJsonArray serialized;
    for (OperationProgress operation : retainedTerminalOperations(operations)) {
        if (!operation.finishedAt.isValid()) {
            operation.finishedAt = QDateTime::currentDateTimeUtc();
        }
        serialized.push_back(serialize(operation));
    }
    const QJsonDocument document(
        QJsonObject{{QStringLiteral("version"), formatVersion},
                    {QStringLiteral("operations"), serialized}});
    const QByteArray contents = document.toJson(QJsonDocument::Compact);

    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() ||
        !file.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("Unable to save the operation history.");
        }
        return false;
    }
    return true;
}

QList<OperationProgress> OperationHistoryStore::retainedTerminalOperations(
    const QList<OperationProgress>& operations)
{
    QList<OperationProgress> retained;
    for (const OperationProgress& operation : operations) {
        if (operation.id != 0 && isTerminal(operation.state)) {
            retained.push_back(operation);
        }
    }
    std::stable_sort(retained.begin(), retained.end(), [](const auto& first, const auto& second) {
        return first.finishedAt < second.finishedAt;
    });
    if (retained.size() > maximumEntries) {
        retained.erase(retained.begin(), retained.end() - maximumEntries);
    }
    return retained;
}

} // namespace rfm::core
