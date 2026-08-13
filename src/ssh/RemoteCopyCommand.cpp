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
                                       const QString& mountPointIdentity,
                                       const QString& mountInfoPath)
{
    const QString quotedPath = quoteArgument(path);
    if (path.isEmpty() || quotedPath.isEmpty()) {
        return {};
    }
    QString guard;
    if (recursive && protectMountPoint) {
        const QString quotedMountInfoPath = quoteArgument(mountInfoPath);
        if (mountPointIdentity.isEmpty() || mountInfoPath.isEmpty() ||
            quotedMountInfoPath.isEmpty()) {
            return {};
        }
        QString encodedMountPoint = mountPointIdentity;
        encodedMountPoint.replace(QChar{'\\'}, QStringLiteral("\\134"));
        encodedMountPoint.replace(QChar{' '}, QStringLiteral("\\040"));
        encodedMountPoint.replace(QChar{'\t'}, QStringLiteral("\\011"));
        encodedMountPoint.replace(QChar{'\n'}, QStringLiteral("\\012"));
        const QString quotedMountPoint = quoteArgument(encodedMountPoint);
        guard = QStringLiteral("[ -r %2 ] || { "
                               "printf 'Unable to verify the remote removal tree.\\n' >&2; "
                               "exit 74; }; "
                               "set -f; "
                               "rfm_invalid_mountinfo() { "
                               "printf 'Invalid remote mount information.\\n' >&2; exit 74; }; "
                               "rfm_valid_mount_path() { "
                               "case \"$1\" in /*) ;; *) return 1;; esac; "
                               "rfm_path_remainder=$1; "
                               "while case \"$rfm_path_remainder\" in *\\\\*) true;; *) false;; "
                               "esac; do "
                               "rfm_after_escape=${rfm_path_remainder#*\\\\}; "
                               "case \"$rfm_after_escape\" in 040*|011*|012*|134*) ;; *) "
                               "return 1;; esac; "
                               "rfm_path_remainder=${rfm_after_escape#???}; "
                               "done; return 0; }; "
                               "rfm_mountinfo_seen=; "
                               "rfm_mount_in_tree=; "
                               "while IFS= read -r rfm_mount_record || "
                               "[ -n \"$rfm_mount_record\" ]; do "
                               "[ -n \"$rfm_mount_record\" ] || rfm_invalid_mountinfo; "
                               "set -- $rfm_mount_record; "
                               "[ \"$#\" -ge 10 ] || rfm_invalid_mountinfo; "
                               "rfm_mount_id=$1; rfm_parent_id=$2; rfm_device=$3; "
                               "rfm_root=$4; rfm_mount_point=$5; rfm_mount_options=$6; "
                               "case \"$rfm_mount_id\" in ''|*[!0-9]*) "
                               "rfm_invalid_mountinfo;; esac; "
                               "case \"$rfm_parent_id\" in ''|*[!0-9]*) "
                               "rfm_invalid_mountinfo;; esac; "
                               "case \"$rfm_device\" in *:*) "
                               "rfm_major=${rfm_device%%:*}; rfm_minor=${rfm_device#*:};; *) "
                               "rfm_invalid_mountinfo;; esac; "
                               "case \"$rfm_major\" in ''|*[!0-9]*) "
                               "rfm_invalid_mountinfo;; esac; "
                               "case \"$rfm_minor\" in ''|*[!0-9]*) "
                               "rfm_invalid_mountinfo;; esac; "
                               "rfm_valid_mount_path \"$rfm_root\" || rfm_invalid_mountinfo; "
                               "rfm_valid_mount_path \"$rfm_mount_point\" || "
                               "rfm_invalid_mountinfo; "
                               "[ -n \"$rfm_mount_options\" ] || rfm_invalid_mountinfo; "
                               "shift 6; rfm_separator_seen=; "
                               "while [ \"$#\" -gt 0 ]; do "
                               "if [ \"$1\" = - ]; then rfm_separator_seen=1; shift; break; fi; "
                               "shift; done; "
                               "[ \"$rfm_separator_seen\" = 1 ] || rfm_invalid_mountinfo; "
                               "[ \"$#\" -eq 3 ] || rfm_invalid_mountinfo; "
                               "[ -n \"$1\" ] && [ -n \"$2\" ] && [ -n \"$3\" ] || "
                               "rfm_invalid_mountinfo; "
                               "rfm_mountinfo_seen=1; "
                               "case \"$rfm_mount_point\" in %1|%1/*) "
                               "rfm_mount_in_tree=$rfm_mount_point;; esac; "
                               "done < %2; "
                               "[ \"$rfm_mountinfo_seen\" = 1 ] || { "
                               "printf 'Unable to verify the remote removal tree.\\n' >&2; "
                               "exit 74; }; "
                               "[ -z \"$rfm_mount_in_tree\" ] || { "
                               "printf 'Refusing recursive removal: a mount point exists in the "
                               "removal tree (%s).\\n' \"$rfm_mount_in_tree\" >&2; exit 75; }; ")
                    .arg(quotedMountPoint, quotedMountInfoPath);
    }
    return guard + QStringLiteral("rm %1%2-f -- %3")
                       .arg(recursive ? QStringLiteral("-R ") : QString{},
                            recursive && protectMountPoint ? QStringLiteral("--one-file-system ")
                                                           : QString{},
                            quotedPath);
}

} // namespace rfm::ssh
