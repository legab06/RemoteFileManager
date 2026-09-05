#pragma once

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

namespace rfm::core {

struct RemoteEntry {
    QString name;
    quint64 size{0};
    QDateTime modifiedAt;
    bool directory{false};
    bool symbolicLink{false};
    bool hidden{false};
};

}  // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::RemoteEntry)
Q_DECLARE_METATYPE(QList<rfm::core::RemoteEntry>)
