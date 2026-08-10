add_library(
    rfm_core STATIC
    core/ConnectionProfile.cpp
    core/OperationHistoryStore.cpp
    core/OperationProgress.cpp
    core/RemoteFileOperations.cpp
    core/RemotePath.cpp
    core/TransferDirectoryJob.cpp
    core/TransferFileJob.cpp
    core/TransferQueue.cpp
    ssh/LibsshRuntime.cpp
    ssh/RemoteCopyCommand.cpp
    ssh/SshSession.cpp
    ssh/SftpTransferBackend.cpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/ssh/SshSession.hpp
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
    app/ConnectionDialog.cpp
    app/FileBrowserPane.cpp
    app/PaneWorkspace.cpp
    app/OperationPanel.cpp
    app/TransferRequestFactory.cpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/MainWindow.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/ConnectionDialog.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/FileBrowserPane.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/PaneWorkspace.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/OperationPanel.hpp
    ${PROJECT_SOURCE_DIR}/include/remotefilemanager/app/TransferRequestFactory.hpp
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
target_link_libraries(RemoteFileManager PRIVATE RemoteFileManager::Ui)
rfm_set_project_warnings(RemoteFileManager)

set_target_properties(
    RemoteFileManager
    PROPERTIES
        OUTPUT_NAME "remote-file-manager"
        WIN32_EXECUTABLE TRUE
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "RemoteFileManager"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.remotefilemanager"
)

install(
    TARGETS RemoteFileManager
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
