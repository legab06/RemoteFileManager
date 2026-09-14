#include "remotefilemanager/ssh/RemoteDirectoryCountJob.hpp"

#include <utility>

namespace rfm::ssh
{

RemoteDirectoryCountJob::RemoteDirectoryCountJob(quint64 id, QString path, ReadNext readNext,
                                                 Close close, EntryHidden entryHidden)
    : m_id(id), m_path(std::move(path)), m_readNext(std::move(readNext)), m_close(std::move(close)),
      m_entryHidden(std::move(entryHidden))
{}

RemoteDirectoryCountJob::~RemoteDirectoryCountJob() { cancel(); }

RemoteDirectoryCountState RemoteDirectoryCountJob::step()
{
    if (m_state != RemoteDirectoryCountState::Pending) {
        return m_state;
    }
    for (qsizetype index = 0; index < entriesPerStep; ++index) {
        const RemoteDirectoryCountRead read = m_readNext();
        if (read.state == RemoteDirectoryCountReadState::Entry) {
            if (read.name != QStringLiteral(".") && read.name != QStringLiteral("..") &&
                (!m_entryHidden || !m_entryHidden(read.name))) {
                ++m_count;
            }
            continue;
        }
        m_state = read.state == RemoteDirectoryCountReadState::End
                      ? RemoteDirectoryCountState::Completed
                      : RemoteDirectoryCountState::Failed;
        m_error = read.error;
        close();
        return m_state;
    }
    return m_state;
}

void RemoteDirectoryCountJob::cancel()
{
    if (m_state == RemoteDirectoryCountState::Pending) {
        m_state = RemoteDirectoryCountState::Cancelled;
    }
    close();
}

quint64 RemoteDirectoryCountJob::id() const { return m_id; }

const QString& RemoteDirectoryCountJob::path() const { return m_path; }

quint64 RemoteDirectoryCountJob::count() const { return m_count; }

int RemoteDirectoryCountJob::error() const { return m_error; }

void RemoteDirectoryCountJob::close()
{
    if (!m_closed) {
        m_closed = true;
        if (m_close) {
            m_close();
        }
    }
}

} // namespace rfm::ssh
