#include "SshTransportHealth.hpp"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

namespace rfm::ssh
{

bool transportAlive(ssh_session session, sftp_session sftp)
{
    if (session == nullptr || sftp == nullptr || ssh_is_connected(session) == 0) {
        return false;
    }
    const int sftpError = sftp_get_error(sftp);
    if (sftpError == SSH_FX_NO_CONNECTION || sftpError == SSH_FX_CONNECTION_LOST) {
        return false;
    }
    const int status = ssh_get_status(session);
    if ((status & (SSH_CLOSED | SSH_CLOSED_ERROR)) != 0) {
        return false;
    }
    const int error = ssh_get_error_code(session);
    return error == SSH_NO_ERROR || error == SSH_REQUEST_DENIED || error == SSH_EINTR;
}

} // namespace rfm::ssh
