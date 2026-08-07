#pragma once

#include "remotefilemanager/core/ConnectionProfile.hpp"
#include "remotefilemanager/core/RemoteEntry.hpp"

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
    void disconnectFromHost();

signals:
    void hostKeyConfirmationRequired(QString host, QString fingerprint);
    void connected(QString initialPath, QList<rfm::core::RemoteEntry> entries);
    void directoryListed(QString path, QList<rfm::core::RemoteEntry> entries);
    void failed(QString message);
    void disconnected();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    void authenticateAndOpen();
    void fail(const QString& message);
};

}  // namespace rfm::ssh
