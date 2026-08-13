#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"

namespace rfm::ssh
{
namespace
{

constexpr auto moveStagingStatusPrefix = "RFM_MOVE_COPY_STATUS:";

} // namespace

QString RemoteCopyCommand::quoteArgument(const QString& argument)
{
    if (argument.contains(QChar{'\0'})) {
        return {};
    }
    QString escaped = argument;
    escaped.replace(QChar{'\''}, QStringLiteral("'\\''"));
    return QChar{'\''} + escaped + QChar{'\''};
}

QString RemoteCopyCommand::build(const QString& source, const QString& destination, bool recursive)
{
    const QString quotedSource = quoteArgument(source);
    const QString quotedDestination = quoteArgument(destination);
    if (source.isEmpty() || destination.isEmpty() || quotedSource.isEmpty() ||
        quotedDestination.isEmpty()) {
        return {};
    }
    return QStringLiteral("cp -P %1-n -- %2 %3")
        .arg(recursive ? QStringLiteral("-R ") : QString{}, quotedSource, quotedDestination);
}

QString RemoteCopyCommand::buildMoveStaging(const QString& source, const QString& destination)
{
    const QString quotedSource = quoteArgument(source);
    const QString quotedDestination = quoteArgument(destination);
    if (source.isEmpty() || destination.isEmpty() || quotedSource.isEmpty() ||
        quotedDestination.isEmpty()) {
        return {};
    }
    // GNU cp -a preserves links, modes, ownership where permitted, timestamps,
    // ACLs, xattrs and hard-link relationships while recursively copying trees.
    // Report cp's status in-band as well: an SSH exit-status request may arrive
    // after EOF and is not guaranteed to be observed by a non-blocking poller.
    return QStringLiteral("cp -a -- %1 %2; rfm_copy_status=$?; "
                          "printf '\\nRFM_MOVE_COPY_STATUS:%s\\n' \"$rfm_copy_status\"; "
                          "exit \"$rfm_copy_status\"")
        .arg(quotedSource, quotedDestination);
}

std::optional<quint32> RemoteCopyCommand::parseMoveStagingStatus(const QByteArray& standardOutput)
{
    const QByteArray prefix{moveStagingStatusPrefix};
    const qsizetype prefixPosition = standardOutput.lastIndexOf(prefix);
    if (prefixPosition < 0 ||
        (prefixPosition > 0 && standardOutput.at(prefixPosition - 1) != '\n')) {
        return std::nullopt;
    }
    const qsizetype valueStart = prefixPosition + prefix.size();
    const qsizetype valueEnd = standardOutput.indexOf('\n', valueStart);
    if (valueEnd < 0) {
        return std::nullopt;
    }
    const QByteArray value = standardOutput.sliced(valueStart, valueEnd - valueStart);
    bool valid = false;
    const uint status = value.toUInt(&valid);
    return valid && status <= 255 ? std::optional<quint32>{status} : std::nullopt;
}

QString RemoteCopyCommand::buildRemove(const QString& path, bool recursive, bool protectMountPoint,
                                       const QString& mountPointIdentity)
{
    const QString quotedPath = quoteArgument(path);
    if (path.isEmpty() || quotedPath.isEmpty()) {
        return {};
    }
    QString guard;
    if (recursive && protectMountPoint) {
        if (mountPointIdentity.isEmpty()) {
            return {};
        }
        QString encodedMountPoint = mountPointIdentity;
        encodedMountPoint.replace(QChar{'\\'}, QStringLiteral("\\134"));
        encodedMountPoint.replace(QChar{' '}, QStringLiteral("\\040"));
        encodedMountPoint.replace(QChar{'\t'}, QStringLiteral("\\011"));
        encodedMountPoint.replace(QChar{'\n'}, QStringLiteral("\\012"));
        const QString quotedMountPoint = quoteArgument(encodedMountPoint);
        guard = QStringLiteral("[ -r /proc/self/mountinfo ] || exit 74; "
                               "rfm_mountinfo_seen=; "
                               "while IFS=' ' read -r rfm_mount_id rfm_parent_id _ _ "
                               "rfm_mount_point _; do "
                               "case \"$rfm_mount_id\" in ''|*[!0-9]*) continue;; esac; "
                               "case \"$rfm_parent_id\" in ''|*[!0-9]*) continue;; esac; "
                               "case \"$rfm_mount_point\" in /*) ;; *) continue;; esac; "
                               "rfm_mountinfo_seen=1; "
                               "[ \"$rfm_mount_point\" = %1 ] && { "
                               "printf 'Refusing to remove a mount point.\\n' >&2; exit 75; }; "
                               "done < /proc/self/mountinfo; "
                               "[ \"$rfm_mountinfo_seen\" = 1 ] || exit 74; ")
                    .arg(quotedMountPoint);
    }
    return guard + QStringLiteral("rm %1-f -- %2")
                       .arg(recursive ? QStringLiteral("-R ") : QString{}, quotedPath);
}

} // namespace rfm::ssh
