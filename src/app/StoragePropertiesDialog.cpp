#include "remotefilemanager/app/StoragePropertiesDialog.hpp"

#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QVBoxLayout>

#include <limits>
#include <optional>

namespace rfm::app
{
namespace
{

QString availableMetadata(const QString& value)
{
    QString safe = value.trimmed();
    for (qsizetype index = 0; index < safe.size(); ++index) {
        const QChar::Category category = safe.at(index).category();
        if (category == QChar::Other_Control || category == QChar::Separator_Line ||
            category == QChar::Separator_Paragraph) {
            safe[index] = QChar{' '};
        }
    }
    safe = safe.simplified();
    return safe.compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) == 0 ? QString{} : safe;
}

std::optional<QString> formattedSize(quint64 bytes)
{
    if (bytes > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        return std::nullopt;
    }
    return QLocale{}.formattedDataSize(static_cast<qint64>(bytes), 1,
                                       QLocale::DataSizeTraditionalFormat);
}

QLabel* addValue(QFormLayout* form, const QString& title, const QString& objectName,
                 const QString& value)
{
    const QString available = availableMetadata(value);
    if (available.isEmpty()) {
        return nullptr;
    }

    auto* const label = new QLabel(available, form->parentWidget());
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setWordWrap(true);
    label->setMaximumWidth(520);
    form->addRow(title, label);
    return label;
}

QLabel* addSectionTitle(QVBoxLayout* layout, const QString& text, const QString& objectName)
{
    auto* const label = new QLabel(text, layout->parentWidget());
    label->setObjectName(objectName);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    layout->addWidget(label);
    return label;
}

QString volumeTitle(const rfm::core::StorageVolume& volume)
{
    const QStringList candidates{volume.displayName, volume.fileSystemLabel,
                                 volume.mounted ? volume.rootPath : QString{}, volume.device};
    for (const QString& candidate : candidates) {
        const QString available = availableMetadata(candidate);
        if (!available.isEmpty()) {
            return available;
        }
    }
    return StoragePropertiesDialog::tr("Storage volume");
}

} // namespace

StoragePropertiesDialog::StoragePropertiesDialog(const rfm::core::StorageVolume& volume,
                                                 QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("storagePropertiesDialog"));
    setWindowTitle(tr("Properties — %1").arg(volumeTitle(volume)));
    setModal(true);
    setMinimumWidth(420);

    auto* const content = new QVBoxLayout(this);
    content->setSizeConstraint(QLayout::SetFixedSize);
    addSectionTitle(content, tr("Storage"), QStringLiteral("storageSectionTitle"));

    auto* const metadata = new QFormLayout;
    metadata->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    addValue(metadata, tr("Name:"), QStringLiteral("storageNameValue"), volume.displayName);
    addValue(metadata, tr("Label:"), QStringLiteral("storageLabelValue"), volume.fileSystemLabel);
    addValue(metadata, tr("Device:"), QStringLiteral("storageDeviceValue"), volume.device);
    addValue(metadata, tr("Model:"), QStringLiteral("storageModelValue"), volume.deviceModel);
    addValue(metadata, tr("Filesystem:"), QStringLiteral("storageFilesystemValue"),
             QString::fromLatin1(volume.fileSystemType));
    if (volume.mounted) {
        addValue(metadata, tr("Mount point:"), QStringLiteral("storageMountPointValue"),
                 volume.rootPath);
    }
    addValue(metadata, tr("Status:"), QStringLiteral("storageStatusValue"),
             volume.mounted ? tr("Mounted") : tr("Not mounted"));
    addValue(metadata, tr("Access:"), QStringLiteral("storageAccessValue"),
             volume.readOnly ? tr("Read-only") : tr("Writable"));
    content->addLayout(metadata);

    const std::optional<QString> capacity =
        volume.bytesTotal > 0 ? formattedSize(volume.bytesTotal) : std::nullopt;
    const std::optional<rfm::core::StorageUsage> usage = rfm::core::storageUsage(volume);
    if (capacity.has_value() || usage.has_value()) {
        addSectionTitle(content, tr("Space"), QStringLiteral("storageSpaceSectionTitle"));
        auto* const space = new QFormLayout;
        space->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        if (capacity.has_value()) {
            addValue(space, tr("Capacity:"), QStringLiteral("storageCapacityValue"), *capacity);
        }
        if (usage.has_value()) {
            if (const auto used = formattedSize(usage->used); used.has_value()) {
                addValue(space, tr("Used:"), QStringLiteral("storageUsedValue"), *used);
            }
            if (const auto available = formattedSize(usage->available); available.has_value()) {
                addValue(space, tr("Available:"), QStringLiteral("storageAvailableValue"),
                         *available);
            }
        }
        content->addLayout(space);

        if (usage.has_value()) {
            auto* const usageBar = new QProgressBar(this);
            usageBar->setObjectName(QStringLiteral("storageUsageBar"));
            usageBar->setRange(0, 100);
            usageBar->setValue(usage->percentage);
            usageBar->setTextVisible(false);
            usageBar->setAccessibleName(tr("Storage usage"));
            usageBar->setAccessibleDescription(tr("%1% used").arg(usage->percentage));
            content->addWidget(usageBar);

            auto* const usageLabel = new QLabel(tr("%1% used").arg(usage->percentage), this);
            usageLabel->setObjectName(QStringLiteral("storageUsageLabel"));
            usageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                Qt::TextSelectableByKeyboard);
            content->addWidget(usageLabel);
        }
    }

    auto* const buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->setObjectName(QStringLiteral("storagePropertiesButtons"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    content->addWidget(buttons);
}

} // namespace rfm::app
