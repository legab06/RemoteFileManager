#pragma once

#include <QMetaType>
#include <QString>

namespace rfm::core
{

enum class FileSource { None, Local, Ssh };

struct BrowserLocation {
    FileSource source{FileSource::None};
    QString machineId;
    QString path;

    [[nodiscard]] bool isValid() const
    {
        return source != FileSource::None && !machineId.isEmpty() && !path.isEmpty();
    }

    friend bool operator==(const BrowserLocation&, const BrowserLocation&) = default;
};

inline constexpr auto LocalMachineId = "local";

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::FileSource)
Q_DECLARE_METATYPE(rfm::core::BrowserLocation)
