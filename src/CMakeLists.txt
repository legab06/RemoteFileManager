add_library(
    rfm_core STATIC
    core/ConnectionProfile.cpp
    core/InternalTransfer.cpp
    core/LocalCopyMove.cpp
    core/LocalCopyMove.hpp
    core/LocalFileSystem.cpp
    core/LinuxMountTable.hpp
    core/LocalRemove.hpp
    core/LocalStorageTopology.hpp
    core/LocalDownloadPath.cpp
    core/OperationHistoryStore.cpp
    core/OperationProgress.cpp
    core/RemoteFilesystem.cpp
    core/RemoteFileOperations.cpp
    core/RemoteMoveSafety.cpp
    core/RemotePath.cpp
    core/RemoteCopyStrategy.cpp
    core/SecurePassword.cpp
    core/ServerCapabilities.cpp
    core/ServerCapabilitiesStore.cpp
    core/ServerProfileStore.cpp
    core/Storage.cpp
    core/ServerSideCopyJob.cpp
    core/TransferDirectoryJob.cpp
    core/TransferFileJob.cpp
    core/TransferCoordinator.cpp
    core/TransferQueue.cpp
    core/VolumeService.cpp
    ssh/LibsshRuntime.cpp
    ssh/RemoteDelete.hpp
    ssh/RemoteCopyCommand.cpp
    ssh/RemoteStorageScanner.cpp
    ssh/RemoteVolumeService.cpp
    ssh/SshSession.cpp
    ssh/SshAuthenticationPolicy.cpp
    ssh/SftpTransferBackend.cpp
    ssh/SftpWriteLoop.hpp
    ssh/SshTransportHealth.cpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/core/LocalFileSystem.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/core/RemoteFilesystem.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/core/RemoteMoveSafety.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/core/RemoteCopyStrategy.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/core/Storage.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/core/SecurePassword.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/core/ServerCapabilities.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/core/ServerCapabilitiesStore.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/core/TransferCoordinator.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/core/VolumeService.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/ssh/SshSession.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/ssh/SshAuthenticationPolicy.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/ssh/RemoteStorageScanner.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/ssh/RemoteVolumeService.hpp
)
add_library(RemoteFileManager::Core ALIAS rfm_core)

target_include_directories(
    rfm_core
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(rfm_core PUBLIC Qt6::Core PRIVATE LibSSH::LibSSH)
rfm_set_project_warnings(rfm_core)

add_library(
    rfm_ui STATIC
    app/MainWindow.cpp
    app/NavigationTree.cpp
    app/ConnectionDialog.cpp
    app/FileBrowserPane.cpp
    app/HomePage.cpp
    app/PaneWorkspace.cpp
    app/WorkspaceTabs.cpp
    app/PasswordAuthenticationDialog.cpp
    app/ServerProfileForm.cpp
    app/ServerProfileDialog.cpp
    app/OperationPanel.cpp
    app/TransferRequestFactory.cpp
    app/VolumeAuthenticationDialog.cpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/MainWindow.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/NavigationTree.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/ConnectionDialog.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/FileBrowserPane.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/HomePage.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/PaneWorkspace.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/WorkspaceTabs.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/PasswordAuthenticationDialog.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/ServerProfileForm.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/ServerProfileDialog.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/OperationPanel.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/TransferRequestFactory.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/VolumeAuthenticationDialog.hpp
)
add_library(RemoteFileManager::Ui ALIAS rfm_ui)

target_include_directories(
    rfm_ui
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(rfm_ui PUBLIC Qt6::Widgets RemoteFileManager::Core)
rfm_set_project_warnings(rfm_ui)

qt_add_executable(RemoteFileManager main.cpp)

qt_add_resources(
    RemoteFileManager
    "app_icons"
    PREFIX "/icons"
    BASE "${PROJECT_SOURCE_DIR}/assets/branding/png"
    FILES
        "${PROJECT_SOURCE_DIR}/assets/branding/png/logo-256.png"
)

target_link_libraries(RemoteFileManager PRIVATE RemoteFileManager::Ui)
rfm_set_project_warnings(RemoteFileManager)

set_target_properties(
    RemoteFileManager
    PROPERTIES
        OUTPUT_NAME "remote-file-manager"
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "RemoteFileManager"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.legab06.RemoteFileManager"
)

install(
    TARGETS RemoteFileManager
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
