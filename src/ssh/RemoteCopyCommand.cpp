#include "remotefilemanager/ssh/RemoteCopyCommand.hpp"

namespace rfm::ssh
{
namespace
{

constexpr auto copyStatusPrefix = "RFM_COPY_STATUS:";

QString wrapCopyCommand(const QString& command)
{
    return QStringLiteral("rfm_copy_pid=; "
                          "rfm_forward_term() { "
                          "[ -z \"$rfm_copy_pid\" ] || kill -TERM \"$rfm_copy_pid\" 2>/dev/null; "
                          "[ -z \"$rfm_copy_pid\" ] || wait \"$rfm_copy_pid\" 2>/dev/null; "
                          "exit 143; }; "
                          "trap 'rfm_forward_term' TERM HUP INT; "
                          "%1 & rfm_copy_pid=$!; "
                          "wait \"$rfm_copy_pid\"; rfm_copy_status=$?; "
                          "trap - TERM HUP INT; "
                          "printf '\\nRFM_COPY_STATUS:%s\\n' \"$rfm_copy_status\"; "
                          "exit \"$rfm_copy_status\"")
        .arg(command);
}

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
    return wrapCopyCommand(
        QStringLiteral("cp -P %1-n -- %2 %3")
            .arg(recursive ? QStringLiteral("-R ") : QString{}, quotedSource, quotedDestination));
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
    return wrapCopyCommand(QStringLiteral("cp -a -- %1 %2").arg(quotedSource, quotedDestination));
}

std::optional<quint32> RemoteCopyCommand::parseCopyStatus(const QByteArray& standardOutput)
{
    const QByteArray prefix{copyStatusPrefix};
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
                               "rfm_valid_mount_escapes() { "
                               "rfm_path_remainder=$1; "
                               "while case \"$rfm_path_remainder\" in *\\\\*) true;; *) false;; "
                               "esac; do "
                               "rfm_after_escape=${rfm_path_remainder#*\\\\}; "
                               "case \"$rfm_after_escape\" in 040*|011*|012*|134*) ;; *) "
                               "return 1;; esac; "
                               "rfm_path_remainder=${rfm_after_escape#???}; "
                               "done; return 0; }; "
                               "rfm_valid_mount_path() { "
                               "case \"$1\" in /*) ;; *) return 1;; esac; "
                               "rfm_valid_mount_escapes \"$1\"; }; "
                               "rfm_take_mount_field() { "
                               "case \"$rfm_mount_fields\" in *' '*) "
                               "rfm_mount_field=${rfm_mount_fields%% *}; "
                               "rfm_mount_fields=${rfm_mount_fields#* };; *) return 1;; esac; "
                               "[ -n \"$rfm_mount_field\" ]; }; "
                               "rfm_valid_mount_options() { "
                               "case \"$1\" in ro|rw|ro,*|rw,*) ;; *) return 1;; esac; "
                               "case \"$1\" in ''|,*|*,|*,,*|*[!A-Za-z0-9_,.=:+-]*) "
                               "return 1;; esac; return 0; }; "
                               "rfm_valid_optional_field() { "
                               "case \"$1\" in unbindable) return 0;; "
                               "shared:*|master:*|propagate_from:*) "
                               "rfm_optional_value=${1#*:}; "
                               "case \"$rfm_optional_value\" in ''|*[!0-9]*) return 1;; esac; "
                               "return 0;; *) return 1;; esac; }; "
                               "rfm_tab=$(printf '\\t'); "
                               "rfm_mountinfo_seen=; "
                               "rfm_mount_in_tree=; "
                               "while :; do "
                               "rfm_mount_record=; "
                               "if IFS= read -r rfm_mount_record; then :; else "
                               "[ -z \"$rfm_mount_record\" ] || rfm_invalid_mountinfo; "
                               "break; fi; "
                               "case \"$rfm_mount_record\" in ''|' '*|*' '|*'  '*|*\"$rfm_tab\"*) "
                               "rfm_invalid_mountinfo;; esac; "
                               "rfm_mount_fields=$rfm_mount_record; "
                               "rfm_take_mount_field || rfm_invalid_mountinfo; "
                               "rfm_mount_id=$rfm_mount_field; "
                               "rfm_take_mount_field || rfm_invalid_mountinfo; "
                               "rfm_parent_id=$rfm_mount_field; "
                               "rfm_take_mount_field || rfm_invalid_mountinfo; "
                               "rfm_device=$rfm_mount_field; "
                               "rfm_take_mount_field || rfm_invalid_mountinfo; "
                               "rfm_root=$rfm_mount_field; "
                               "rfm_take_mount_field || rfm_invalid_mountinfo; "
                               "rfm_mount_point=$rfm_mount_field; "
                               "rfm_take_mount_field || rfm_invalid_mountinfo; "
                               "rfm_mount_options=$rfm_mount_field; "
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
                               "rfm_valid_mount_options \"$rfm_mount_options\" || "
                               "rfm_invalid_mountinfo; "
                               "while :; do "
                               "rfm_take_mount_field || rfm_invalid_mountinfo; "
                               "[ \"$rfm_mount_field\" = - ] && break; "
                               "rfm_valid_optional_field \"$rfm_mount_field\" || "
                               "rfm_invalid_mountinfo; done; "
                               "rfm_take_mount_field || rfm_invalid_mountinfo; "
                               "rfm_filesystem_type=$rfm_mount_field; "
                               "rfm_take_mount_field || rfm_invalid_mountinfo; "
                               "rfm_mount_source=$rfm_mount_field; "
                               "case \"$rfm_mount_fields\" in ''|*' '*) "
                               "rfm_invalid_mountinfo;; esac; "
                               "rfm_super_options=$rfm_mount_fields; "
                               "case \"$rfm_filesystem_type\" in *[!A-Za-z0-9._+-]*) "
                               "rfm_invalid_mountinfo;; esac; "
                               "rfm_valid_mount_escapes \"$rfm_mount_source\" || "
                               "rfm_invalid_mountinfo; "
                               "rfm_valid_mount_escapes \"$rfm_super_options\" || "
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
