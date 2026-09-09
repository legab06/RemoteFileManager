cmake_minimum_required(VERSION 3.25)

project(
    RemoteFileManager
    VERSION 0.8.6
    DESCRIPTION "Native remote file manager over SSH and SFTP"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(RFM_BUILD_TESTS "Build RemoteFileManager tests" ON)
option(RFM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets)
find_package(LibSSH 0.10 REQUIRED)

qt_standard_project_setup()

include(CompilerWarnings)
include(GNUInstallDirs)

add_subdirectory(src)

if(UNIX AND NOT APPLE)
    install(
        FILES
            "${PROJECT_SOURCE_DIR}/data/io.github.legab06.RemoteFileManager.desktop"
        DESTINATION
            "${CMAKE_INSTALL_DATADIR}/applications"
    )

    install(
        FILES
            "${PROJECT_SOURCE_DIR}/data/io.github.legab06.RemoteFileManager.metainfo.xml"
        DESTINATION
            "${CMAKE_INSTALL_DATADIR}/metainfo"
    )

    install(
        FILES
            "${PROJECT_SOURCE_DIR}/assets/branding/png/logo-256.png"
        DESTINATION
            "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps"
        RENAME
            "io.github.legab06.RemoteFileManager.png"
    )
endif()

include(CTest)
if(BUILD_TESTING AND RFM_BUILD_TESTS)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
