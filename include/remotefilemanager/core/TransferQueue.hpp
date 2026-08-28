#pragma once

#include "remotefilemanager/core/TransferTypes.hpp"

#include <QQueue>

#include <optional>

namespace rfm::core
{

class TransferQueue final
{
  public:
    [[nodiscard]] bool enqueue(const TransferRequest& request);
    [[nodiscard]] bool contains(quint64 id) const;
    [[nodiscard]] bool cancel(quint64 id, TransferRequest& cancelled);
    [[nodiscard]] std::optional<TransferRequest> takeNext();
    [[nodiscard]] QList<TransferRequest> takeAll();
    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] qsizetype size() const;

  private:
    QQueue<TransferRequest> m_requests;
};

} // namespace rfm::core
