#include "remotefilemanager/core/LocalDownloadPath.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace rfm::core
{
namespace
{

LocalPathFlavor effectiveFlavor(LocalPathFlavor flavor)
{
    if (flavor != LocalPathFlavor::Native) {
        return flavor;
    }
#ifdef Q_OS_WIN
    return LocalPathFlavor::Windows;
#else
    return LocalPathFlavor::Posix;
#endif
}

bool invalidWindowsName(const QString& name)
{
    if (name.endsWith(QChar{' '}) || name.endsWith(QChar{'.'})) {
        return true;
    }
    for (const QChar character : name) {
        if (character.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(character)) {
            return true;
        }
    }
    const QString baseName = name.section(QChar{'.'}, 0, 0).toUpper();
    static const QRegularExpression reserved(
        QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$"));
    return reserved.match(baseName).hasMatch();
}

QString invalidNameMessage(const QString& name)
{
    return QStringLiteral("The remote name cannot be represented safely on this computer: %1")
        .arg(name);
}

} // namespace

LocalDownloadPathResult LocalDownloadPath::child(const QString& selectedRoot,
                                                 const QString& parent,
                                                 const QString& remoteName,
                                                 LocalPathFlavor flavor)
{
    if (selectedRoot.isEmpty() || parent.isEmpty() || remoteName.isEmpty() ||
        remoteName == QStringLiteral(".") || remoteName == QStringLiteral("..") ||
        remoteName.contains(QChar{'\0'}) || remoteName.contains(QChar{'/'}) ||
        QDir::isAbsolutePath(remoteName)) {
        return {{}, invalidNameMessage(remoteName)};
    }
    const LocalPathFlavor platform = effectiveFlavor(flavor);
    if (platform == LocalPathFlavor::Windows && invalidWindowsName(remoteName)) {
        return {{}, invalidNameMessage(remoteName)};
    }

    const QString root = QDir::fromNativeSeparators(
        QDir::cleanPath(QFileInfo(selectedRoot).absoluteFilePath()));
    const QString normalizedParent =
        QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(parent).absoluteFilePath()));
    const QString candidate = QDir::fromNativeSeparators(
        QDir::cleanPath(QDir(normalizedParent).filePath(remoteName)));
    const Qt::CaseSensitivity sensitivity =
        platform == LocalPathFlavor::Windows ? Qt::CaseInsensitive : Qt::CaseSensitive;
    const QString rootPrefix = root.endsWith(QChar{'/'}) ? root : root + QChar{'/'};
    if (normalizedParent.compare(root, sensitivity) != 0 &&
        !normalizedParent.startsWith(rootPrefix, sensitivity)) {
        return {{}, QStringLiteral("The local download destination escapes the selected folder.")};
    }
    if (candidate.compare(root, sensitivity) == 0 ||
        !candidate.startsWith(rootPrefix, sensitivity)) {
        return {{}, QStringLiteral("The local download destination escapes the selected folder.")};
    }
    return {candidate, {}};
}

} // namespace rfm::core
