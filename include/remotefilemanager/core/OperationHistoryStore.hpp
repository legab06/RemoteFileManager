#pragma once

#include "remotefilemanager/core/OperationProgress.hpp"

#include <QList>
#include <QString>

namespace rfm::core
{

class OperationHistoryStore final
{
  public:
    static constexpr int formatVersion = 1;
    static constexpr qsizetype maximumEntries = 200;

    explicit OperationHistoryStore(QString storageDirectory = {});

    [[nodiscard]] QString filePath() const;
    [[nodiscard]] QList<OperationProgress> load() const;
    [[nodiscard]] bool save(const QList<OperationProgress>& operations,
                            QString* error = nullptr) const;
    [[nodiscard]] static QList<OperationProgress>
    retainedTerminalOperations(const QList<OperationProgress>& operations);

  private:
    QString m_storageDirectory;
};

} // namespace rfm::core
