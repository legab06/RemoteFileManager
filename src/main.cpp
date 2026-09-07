#include "remotefilemanager/app/MainWindow.hpp"

#include <QApplication>
#include <QCoreApplication>

#include <QIcon>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setWindowIcon(QIcon(QStringLiteral(":/icons/logo-256.png")));
    QCoreApplication::setOrganizationName(QStringLiteral("RemoteFileManager"));
    QCoreApplication::setApplicationName(QStringLiteral("RemoteFileManager"));
    // Note: Application version is automatically set from CMake PROJECT_VERSION via qt_standard_project_setup()

    rfm::app::MainWindow window;
    window.show();

    return application.exec();
}
