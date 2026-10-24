#pragma once

#include "remotefilemanager/core/Storage.hpp"

#include <QDialog>

namespace rfm::app
{

class StoragePropertiesDialog final : public QDialog
{
    Q_OBJECT

  public:
    explicit StoragePropertiesDialog(const rfm::core::StorageVolume& volume,
                                     QWidget* parent = nullptr);
};

} // namespace rfm::app
