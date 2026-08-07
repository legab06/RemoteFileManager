# Find libssh and expose the cross-platform target LibSSH::LibSSH.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LIBSSH QUIET libssh)
endif()

find_path(
    LibSSH_INCLUDE_DIR
    NAMES libssh/libssh.h
    HINTS ${PC_LIBSSH_INCLUDE_DIRS}
)

find_library(
    LibSSH_LIBRARY
    NAMES ssh libssh
    HINTS ${PC_LIBSSH_LIBRARY_DIRS}
)

set(LibSSH_VERSION "${PC_LIBSSH_VERSION}")

if(NOT LibSSH_VERSION AND LibSSH_INCLUDE_DIR)
    set(_libssh_version_header "${LibSSH_INCLUDE_DIR}/libssh/libssh_version.h")
    if(EXISTS "${_libssh_version_header}")
        file(STRINGS "${_libssh_version_header}" _libssh_version_lines
            REGEX "^#define LIBSSH_VERSION_(MAJOR|MINOR|MICRO)[ \t]+[0-9]+")
        foreach(_component MAJOR MINOR MICRO)
            set(_libssh_version_${_component} "")
            foreach(_line IN LISTS _libssh_version_lines)
                if(_line MATCHES "LIBSSH_VERSION_${_component}[ \t]+([0-9]+)")
                    set(_libssh_version_${_component} "${CMAKE_MATCH_1}")
                endif()
            endforeach()
        endforeach()
        if(NOT _libssh_version_MAJOR STREQUAL ""
           AND NOT _libssh_version_MINOR STREQUAL ""
           AND NOT _libssh_version_MICRO STREQUAL "")
            set(LibSSH_VERSION
                "${_libssh_version_MAJOR}.${_libssh_version_MINOR}.${_libssh_version_MICRO}")
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    LibSSH
    REQUIRED_VARS LibSSH_LIBRARY LibSSH_INCLUDE_DIR
    VERSION_VAR LibSSH_VERSION
)

if(LibSSH_FOUND AND NOT TARGET LibSSH::LibSSH)
    add_library(LibSSH::LibSSH UNKNOWN IMPORTED)
    set_target_properties(
        LibSSH::LibSSH
        PROPERTIES
            IMPORTED_LOCATION "${LibSSH_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LibSSH_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(LibSSH_INCLUDE_DIR LibSSH_LIBRARY)
