#include "remotefilemanager/core/RemoteMoveSafety.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QSet>

#include <algorithm>
#include <cctype>

namespace rfm::core
{
namespace
{

std::optional<QString> decodeMountInfoField(const QByteArray& field)
{
    QByteArray decoded;
    decoded.reserve(field.size());
    for (qsizetype index = 0; index < field.size(); ++index) {
        if (field.at(index) == '\\') {
            if (index + 3 >= field.size()) {
                return std::nullopt;
            }
            const QByteArray escape = field.sliced(index, 4);
            if (escape == QByteArrayLiteral("\\040")) {
                decoded.push_back(' ');
            } else if (escape == QByteArrayLiteral("\\011")) {
                decoded.push_back('\t');
            } else if (escape == QByteArrayLiteral("\\012")) {
                decoded.push_back('\n');
            } else if (escape == QByteArrayLiteral("\\134")) {
                decoded.push_back('\\');
            } else {
                return std::nullopt;
            }
            index += 3;
            continue;
        }
        decoded.push_back(field.at(index));
    }
    const QString path = QString::fromUtf8(decoded);
    if (path.toUtf8() != decoded || !path.startsWith(QChar{'/'}) ||
        RemotePath::normalize(path) != path) {
        return std::nullopt;
    }
    return path;
}

bool validUnsignedInteger(const QByteArray& field, quint64* value = nullptr)
{
    if (field.isEmpty()) {
        return false;
    }
    bool valid = false;
    const quint64 parsed = field.toULongLong(&valid);
    if (!valid) {
        return false;
    }
    if (value != nullptr) {
        *value = parsed;
    }
    return true;
}

bool validEscapes(const QByteArray& field)
{
    for (qsizetype index = 0; index < field.size(); ++index) {
        if (field.at(index) != '\\') {
            continue;
        }
        if (index + 3 >= field.size()) {
            return false;
        }
        const QByteArray escape = field.sliced(index, 4);
        if (escape != QByteArrayLiteral("\\040") && escape != QByteArrayLiteral("\\011") &&
            escape != QByteArrayLiteral("\\012") && escape != QByteArrayLiteral("\\134")) {
            return false;
        }
        index += 3;
    }
    return true;
}

bool validMountInfoToken(const QByteArray& field)
{
    if (field.isEmpty() || !validEscapes(field)) {
        return false;
    }
    return std::ranges::all_of(field, [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return value > 0x20 && value != 0x7f;
    });
}

bool validMountOptions(const QByteArray& field)
{
    if (field != QByteArrayLiteral("ro") && field != QByteArrayLiteral("rw") &&
        !field.startsWith(QByteArrayLiteral("ro,")) &&
        !field.startsWith(QByteArrayLiteral("rw,"))) {
        return false;
    }
    if (field.startsWith(',') || field.endsWith(',') || field.contains(",,")) {
        return false;
    }
    return std::ranges::all_of(field, [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '_' || character == ',' ||
               character == '.' || character == '=' || character == ':' || character == '+' ||
               character == '-';
    });
}

bool validOptionalField(const QByteArray& field)
{
    if (field == QByteArrayLiteral("unbindable")) {
        return true;
    }
    if (field.startsWith(QByteArrayLiteral("unbindable:"))) {
        return false;
    }
    for (const QByteArray& prefix : {QByteArrayLiteral("shared:"), QByteArrayLiteral("master:"),
                                     QByteArrayLiteral("propagate_from:")}) {
        if (field.startsWith(prefix)) {
            return validUnsignedInteger(field.sliced(prefix.size()));
        }
    }
    if (field == QByteArrayLiteral("shared") || field == QByteArrayLiteral("master") ||
        field == QByteArrayLiteral("propagate_from")) {
        return false;
    }

    const qsizetype separator = field.indexOf(':');
    if (separator == 0 || field.indexOf(':', separator + 1) >= 0 ||
        (separator >= 0 && separator + 1 == field.size())) {
        return false;
    }
    const auto validCharacter = [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '_' || character == '.' ||
               character == '+' || character == '-';
    };
    const QByteArray tag = separator < 0 ? field : field.first(separator);
    const QByteArray value = separator < 0 ? QByteArray{} : field.sliced(separator + 1);
    return !tag.isEmpty() && std::ranges::all_of(tag, validCharacter) &&
           (separator < 0 || std::ranges::all_of(value, validCharacter));
}

bool validFileSystemType(const QByteArray& field)
{
    return !field.isEmpty() && std::ranges::all_of(field, [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '.' || character == '_' ||
               character == '+' || character == '-';
    });
}

} // namespace

QString remoteMoveFileSystemContainer(const QString& entryPath)
{
    const QString normalized = RemotePath::normalize(entryPath);
    return normalized.startsWith(QChar{'/'}) ? RemotePath::parent(normalized) : QString{};
}

RemoteMountPointState linuxMountPointState(const QByteArray& mountInfo, const QString& entryPath)
{
    const QString normalized = RemotePath::normalize(entryPath);
    if (!normalized.startsWith(QChar{'/'}) || mountInfo.isEmpty() || !mountInfo.endsWith('\n')) {
        return RemoteMountPointState::Unknown;
    }

    QSet<quint64> mountIds;
    QSet<QString> mountPoints;
    const QList<QByteArray> lines = mountInfo.split('\n');
    for (qsizetype lineIndex = 0; lineIndex + 1 < lines.size(); ++lineIndex) {
        const QByteArray& line = lines.at(lineIndex);
        if (line.isEmpty() || line.contains('\0')) {
            return RemoteMountPointState::Unknown;
        }
        const qsizetype separator = line.indexOf(" - ");
        if (separator <= 0 || line.indexOf(" - ", separator + 3) >= 0) {
            return RemoteMountPointState::Unknown;
        }
        const QList<QByteArray> fields = line.first(separator).split(' ');
        const QList<QByteArray> trailingFields = line.sliced(separator + 3).split(' ');
        if (fields.size() < 6 || trailingFields.size() != 3 || fields.contains({}) ||
            trailingFields.contains({})) {
            return RemoteMountPointState::Unknown;
        }
        quint64 mountId = 0;
        quint64 parentId = 0;
        if (!validUnsignedInteger(fields.at(0), &mountId) || mountId == 0 ||
            !validUnsignedInteger(fields.at(1), &parentId) || parentId == 0 ||
            mountIds.contains(mountId)) {
            return RemoteMountPointState::Unknown;
        }
        const QList<QByteArray> device = fields.at(2).split(':');
        const std::optional<QString> mountPoint = decodeMountInfoField(fields.at(4));
        if (device.size() != 2 || !validUnsignedInteger(device.at(0)) ||
            !validUnsignedInteger(device.at(1)) || !validMountInfoToken(fields.at(3)) ||
            !mountPoint.has_value()) {
            return RemoteMountPointState::Unknown;
        }
        if (!validMountOptions(fields.at(5)) ||
            !std::ranges::all_of(fields.cbegin() + 6, fields.cend(), validOptionalField) ||
            !validFileSystemType(trailingFields.at(0)) || !validEscapes(trailingFields.at(1)) ||
            !validEscapes(trailingFields.at(2))) {
            return RemoteMountPointState::Unknown;
        }
        mountIds.insert(mountId);
        mountPoints.insert(*mountPoint);
    }
    if (mountIds.isEmpty()) {
        return RemoteMountPointState::Unknown;
    }
    return mountPoints.contains(normalized) ? RemoteMountPointState::MountPoint
                                            : RemoteMountPointState::NotMountPoint;
}

bool allowsRemoteMoveFallback(const RemoteMoveFallbackEvidence& evidence)
{
    return evidence.sourceMountPoint == RemoteMountPointState::NotMountPoint &&
           evidence.sourceContainerFileSystem.has_value() &&
           evidence.destinationContainerFileSystem.has_value() &&
           evidence.sourceContainerFileSystem != evidence.destinationContainerFileSystem;
}

} // namespace rfm::core
