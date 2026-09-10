#pragma once

#include "remotefilemanager/core/ServerCapabilities.hpp"

#include <QList>
#include <QString>
#include <QtGlobal>

namespace rfm::core
{

struct PersistedServerCapabilities {
    QString profileId;
    QString host;
    QString username;
    quint16 port{0};
    ServerCapabilities capabilities;

    [[nodiscard]] bool isValid() const;
};

class ServerCapabilitiesStore final
{
  public:
    static constexpr int formatVersion = 1;

    explicit ServerCapabilitiesStore(QString storageDirectory = {});

    [[nodiscard]] QString filePath() const;
    [[nodiscard]] QList<PersistedServerCapabilities> load(QString* error = nullptr) const;
    [[nodiscard]] bool save(const QList<PersistedServerCapabilities>& snapshots,
                            QString* error = nullptr) const;
    [[nodiscard]] bool upsert(const PersistedServerCapabilities& snapshot,
                              QString* error = nullptr) const;
    [[nodiscard]] bool remove(const QString& profileId, QString* error = nullptr) const;

  private:
    QString m_storageDirectory;
};

} // namespace rfm::core
