#pragma once

struct ssh_session_struct;
struct sftp_session_struct;

namespace rfm::ssh
{

[[nodiscard]] bool transportAlive(ssh_session_struct* session, sftp_session_struct* sftp);

} // namespace rfm::ssh
