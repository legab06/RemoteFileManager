#include "remotefilemanager/core/TransferQueue.hpp"

namespace rfm::core
{

bool TransferQueue::enqueue(const TransferRequest& request)
{
    if (request.id == 0 || contains(request.id)) {
        return false;
    }
    m_requests.enqueue(request);
    return true;
}

bool TransferQueue::contains(quint64 id) const
{
    for (const TransferRequest& request : m_requests) {
        if (request.id == id) {
            return true;
        }
    }
    return false;
}

bool TransferQueue::cancel(quint64 id, TransferRequest& cancelled)
{
    for (auto iterator = m_requests.begin(); iterator != m_requests.end(); ++iterator) {
        if (iterator->id == id) {
            cancelled = *iterator;
            m_requests.erase(iterator);
            return true;
        }
    }
    return false;
}

std::optional<TransferRequest> TransferQueue::takeNext()
{
    if (m_requests.isEmpty()) {
        return std::nullopt;
    }
    return m_requests.dequeue();
}

bool TransferQueue::isEmpty() const { return m_requests.isEmpty(); }

qsizetype TransferQueue::size() const { return m_requests.size(); }

void TransferQueue::clear() { m_requests.clear(); }

} // namespace rfm::core
