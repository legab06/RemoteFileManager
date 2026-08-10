#include "remotefilemanager/app/MainWindow.hpp"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("RemoteFileManager"));
    QCoreApplication::setApplicationName(QStringLiteral("RemoteFileManager"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.6.0"));

    rfm::app::MainWindow window;
    window.show();

    return application.exec();
}
