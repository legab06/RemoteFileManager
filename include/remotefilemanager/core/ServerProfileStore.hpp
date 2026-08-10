#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"

#include <QList>
#include <QString>

namespace rfm::core
{

class ServerProfileStore final
{
  public:
    static constexpr int formatVersion = 1;

    explicit ServerProfileStore(QString storageDirectory = {});

    [[nodiscard]] QString filePath() const;
    [[nodiscard]] QList<ConnectionProfile> load(QString* error = nullptr) const;
    [[nodiscard]] bool save(const QList<ConnectionProfile>& profiles,
                            QString* error = nullptr) const;
    [[nodiscard]] bool upsert(const ConnectionProfile& profile,
                              QString* error = nullptr) const;
    [[nodiscard]] bool remove(const QString& id, QString* error = nullptr) const;

  private:
    QString m_storageDirectory;
};

} // namespace rfm::core
