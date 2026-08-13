#include "remotefilemanager/core/RemoteMoveSafety.hpp"

#include "remotefilemanager/core/RemotePath.hpp"

#include <QHash>

namespace rfm::core
{
namespace
{

QString decodeMountInfoField(const QByteArray& field)
{
    QByteArray decoded;
    decoded.reserve(field.size());
    static const QHash<QByteArray, char> standardEscapes{{QByteArrayLiteral("\\040"), ' '},
                                                         {QByteArrayLiteral("\\011"), '\t'},
                                                         {QByteArrayLiteral("\\012"), '\n'},
                                                         {QByteArrayLiteral("\\134"), '\\'}};
    for (qsizetype index = 0; index < field.size(); ++index) {
        if (field.at(index) == '\\' && index + 3 < field.size()) {
            const QByteArray escape = field.sliced(index, 4);
            const auto decodedEscape = standardEscapes.constFind(escape);
            if (decodedEscape != standardEscapes.cend()) {
                decoded.push_back(decodedEscape.value());
                index += 3;
                continue;
            }
        }
        decoded.push_back(field.at(index));
    }
    return RemotePath::normalize(QString::fromUtf8(decoded));
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
    if (!normalized.startsWith(QChar{'/'}) || mountInfo.isEmpty()) {
        return RemoteMountPointState::Unknown;
    }

    bool parsedRecord = false;
    for (const QByteArray& line : mountInfo.split('\n')) {
        const qsizetype separator = line.indexOf(" - ");
        if (separator < 0) {
            continue;
        }
        const QList<QByteArray> fields = line.first(separator).split(' ');
        if (fields.size() < 6) {
            continue;
        }
        bool mountIdValid = false;
        bool parentIdValid = false;
        static_cast<void>(fields.at(0).toULongLong(&mountIdValid));
        static_cast<void>(fields.at(1).toULongLong(&parentIdValid));
        const QString mountPoint = decodeMountInfoField(fields.at(4));
        if (!mountIdValid || !parentIdValid || !mountPoint.startsWith(QChar{'/'})) {
            continue;
        }
        parsedRecord = true;
        if (mountPoint == normalized) {
            return RemoteMountPointState::MountPoint;
        }
    }
    return parsedRecord ? RemoteMountPointState::NotMountPoint : RemoteMountPointState::Unknown;
}

bool allowsRemoteMoveFallback(const RemoteMoveFallbackEvidence& evidence)
{
    return evidence.sourceMountPoint == RemoteMountPointState::NotMountPoint &&
           evidence.sourceContainerFileSystem.has_value() &&
           evidence.destinationContainerFileSystem.has_value() &&
           evidence.sourceContainerFileSystem != evidence.destinationContainerFileSystem;
}

} // namespace rfm::core
