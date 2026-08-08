#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"
#include "remotefilemanager/core/RemoteFileOperations.hpp"
#include "remotefilemanager/core/TransferTypes.hpp"

#include <QObject>
#include <memory>

namespace rfm::ssh {

class SshSession final : public QObject {
    Q_OBJECT

public:
    explicit SshSession(QObject* parent = nullptr);
    ~SshSession() override;

public slots:
    void connectToHost(rfm::core::ConnectionProfile profile, QString password);
    void confirmUnknownHost(bool accepted);
    void listDirectory(QString path);
    void createDirectory(quint64 id, QString parent, QString name);
    void renameEntry(quint64 id, QString source, QString newName);
    void moveEntries(quint64 id,
                     QList<rfm::core::RemoteSelection> sources,
                     QString destinationDirectory);
    void copyEntries(quint64 id,
                     QList<rfm::core::RemoteSelection> sources,
                     QString destinationDirectory);
    void removeEntries(quint64 id, QList<rfm::core::RemoteSelection> sources, bool recursive);
    void enqueueTransfer(rfm::core::TransferRequest request);
    void pauseTransfer(quint64 id);
    void resumeTransfer(quint64 id);
    void cancelTransfer(quint64 id);
    void shutdownTransfers();
    void disconnectFromHost();

signals:
    void hostKeyConfirmationRequired(QString host, QString fingerprint);
    void connected(QString initialPath, QList<rfm::core::RemoteEntry> entries);
    void directoryListed(QString path, QList<rfm::core::RemoteEntry> entries);
    void operationFinished(rfm::core::RemoteOperationResult result);
    void transferUpdated(rfm::core::TransferProgress progress);
    void transferRejected(quint64 id, QString error);
    void transfersShutdown();
    void failed(QString message);
    void disconnected();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    void authenticateAndOpen();
    void processTransferStep();
    void scheduleTransferStep();
    void fail(const QString& message);
};

}  // namespace rfm::ssh
