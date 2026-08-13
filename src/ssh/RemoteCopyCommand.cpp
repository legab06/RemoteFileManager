#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"

namespace rfm::ssh {

QString RemoteCopyCommand::quoteArgument(const QString& argument)
{
    if (argument.contains(QChar{'\0'})) {
        return {};
    }
    QString escaped = argument;
    escaped.replace(QChar{'\''}, QStringLiteral("'\\''"));
    return QChar{'\''} + escaped + QChar{'\''};
}

QString RemoteCopyCommand::build(
    const QString& source, const QString& destination, bool recursive)
{
    const QString quotedSource = quoteArgument(source);
    const QString quotedDestination = quoteArgument(destination);
    if (source.isEmpty() || destination.isEmpty() || quotedSource.isEmpty()
        || quotedDestination.isEmpty()) {
        return {};
    }
    return QStringLiteral("cp -P %1-n -- %2 %3")
        .arg(recursive ? QStringLiteral("-R ") : QString{}, quotedSource, quotedDestination);
}

QString RemoteCopyCommand::buildRemove(const QString& path, bool recursive)
{
    const QString quotedPath = quoteArgument(path);
    if (path.isEmpty() || quotedPath.isEmpty()) {
        return {};
    }
    return QStringLiteral("rm %1-f -- %2")
        .arg(recursive ? QStringLiteral("-R ") : QString{}, quotedPath);
}

}  // namespace rfm::ssh
