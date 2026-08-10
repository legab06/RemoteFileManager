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

}  // namespace rfm::ssh
