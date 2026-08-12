#include "remotefilemanager/core/RemotePath.hpp"

#include <QStringList>

namespace rfm::core
{

bool RemotePath::isValidName(const QString& name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") &&
           !name.contains(QChar{'/'}) && !name.contains(QChar{'\0'});
}

QString RemotePath::normalize(const QString& path)
{
    if (path.isEmpty() || path.contains(QChar{'\0'})) {
        return {};
    }
    const bool absolute = path.startsWith(QChar{'/'});
    QStringList components;
    for (const QString& component : path.split(QChar{'/'}, Qt::SkipEmptyParts)) {
        if (component == QStringLiteral(".")) {
            continue;
        }
        if (component == QStringLiteral("..")) {
            if (!components.isEmpty() && components.constLast() != QStringLiteral("..")) {
                components.removeLast();
            } else if (!absolute) {
                components.push_back(component);
            }
            continue;
        }
        components.push_back(component);
    }
    if (absolute) {
        return components.isEmpty() ? QStringLiteral("/")
                                    : QStringLiteral("/") + components.join(QChar{'/'});
    }
    return components.isEmpty() ? QStringLiteral(".") : components.join(QChar{'/'});
}

QString RemotePath::join(const QString& directory, const QString& name)
{
    if (!isValidName(name)) {
        return {};
    }
    const QString normalizedDirectory = normalize(directory);
    if (normalizedDirectory.isEmpty()) {
        return {};
    }
    if (normalizedDirectory == QStringLiteral("/")) {
        return normalizedDirectory + name;
    }
    if (normalizedDirectory == QStringLiteral(".")) {
        return QStringLiteral("./") + name;
    }
    return normalizedDirectory + QChar{'/'} + name;
}

QString RemotePath::parent(const QString& path)
{
    const QString normalized = normalize(path);
    if (normalized.isEmpty() || normalized == QStringLiteral("/") ||
        normalized == QStringLiteral(".")) {
        return normalized;
    }
    const qsizetype separator = normalized.lastIndexOf(QChar{'/'});
    if (separator < 0) {
        return QStringLiteral(".");
    }
    if (separator == 0) {
        return QStringLiteral("/");
    }
    return normalized.left(separator);
}

QString RemotePath::fileName(const QString& path)
{
    const QString normalized = normalize(path);
    if (normalized.isEmpty() || normalized == QStringLiteral("/") ||
        normalized == QStringLiteral(".")) {
        return {};
    }
    return normalized.mid(normalized.lastIndexOf(QChar{'/'}) + 1);
}

bool RemotePath::isProtected(const QString& path)
{
    const QString normalized = normalize(path);
    return normalized.isEmpty() || normalized == QStringLiteral("/") ||
           normalized == QStringLiteral(".") || normalized == QStringLiteral("..") ||
           normalized.startsWith(QStringLiteral("../"));
}

bool RemotePath::isAtOrBelow(const QString& path, const QString& rootPath)
{
    const QString normalizedPath = normalize(path);
    const QString normalizedRoot = normalize(rootPath);
    if (!normalizedPath.startsWith(QChar{'/'}) || !normalizedRoot.startsWith(QChar{'/'})) {
        return false;
    }
    return normalizedPath == normalizedRoot || normalizedRoot == QStringLiteral("/") ||
           normalizedPath.startsWith(normalizedRoot + QChar{'/'});
}

} // namespace rfm::core
