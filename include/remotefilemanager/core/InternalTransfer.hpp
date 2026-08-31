#pragma once

#include "remotefilemanager/core/BrowserLocation.hpp"
#include "remotefilemanager/core/RemoteFileOperations.hpp"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace rfm::core
{

inline constexpr auto InternalTransferMimeType =
    "application/x-remotefilemanager-internal-transfer-v2";

enum class InternalTransferAction { Copy, Move };

struct RemoteConnectionIdentity {
    QString host;
    quint16 port{0};
    quint64 generation{0};

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool operator==(const RemoteConnectionIdentity& other) const = default;
};

struct InternalTransferPayload {
    FileSource source{FileSource::None};
    QString sourceMachineId;
    QString applicationInstanceId;
    RemoteConnectionIdentity connection;
    quint64 sourcePaneId{0};
    QList<RemoteSelection> sources;

    [[nodiscard]] bool isValid() const;
};

struct ClipboardEntry {
    InternalTransferAction action{InternalTransferAction::Copy};
    InternalTransferPayload payload;
};

class InternalClipboard final
{
  public:
    void set(InternalTransferAction action, InternalTransferPayload payload);
    void clear();

    [[nodiscard]] bool hasContent() const;
    [[nodiscard]] bool isCut() const;
    [[nodiscard]] bool matchesCutGeneration(quint64 generation) const;
    [[nodiscard]] quint64 generation() const;
    [[nodiscard]] const std::optional<ClipboardEntry>& content() const;

  private:
    std::optional<ClipboardEntry> m_content;
    quint64 m_generation{0};
};

enum class InternalTransferValidationError {
    None,
    InvalidPayload,
    InvalidSource,
    ForeignApplication,
    IncompatibleSource,
    IncompatibleConnection,
    InvalidDestination,
    IncompatiblePathConvention,
    IdenticalSourceAndDestination,
    DestinationInsideSource,
};

struct InternalTransferValidation {
    InternalTransferValidationError error{InternalTransferValidationError::None};

    [[nodiscard]] bool accepted() const { return error == InternalTransferValidationError::None; }
};

[[nodiscard]] QByteArray encodeInternalTransfer(const InternalTransferPayload& payload);
[[nodiscard]] std::optional<InternalTransferPayload> decodeInternalTransfer(const QByteArray& data);
[[nodiscard]] InternalTransferValidation
validateInternalTransfer(const InternalTransferPayload& payload,
                         const QString& applicationInstanceId, const BrowserLocation& destination,
                         const RemoteConnectionIdentity& destinationConnection = {});

} // namespace rfm::core

Q_DECLARE_METATYPE(rfm::core::InternalTransferPayload)
