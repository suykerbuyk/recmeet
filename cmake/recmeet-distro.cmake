# cmake/recmeet-distro.cmake — Linux distribution detection
#
# Parses /etc/os-release and exports RECMEET_DISTRO (lowercased) as one of:
#   arch, debian, fedora, nixos, alpine, gentoo, opensuse, unknown
#
# Honors both ID and ID_LIKE so derivatives (Ubuntu/Mint/Pop → debian,
# Rocky/Alma → fedora, etc.) map onto their parent family. openSUSE Leap
# and openSUSE Tumbleweed both collapse to `opensuse`.
#
# Included by cmake/prerequisites.cmake and cmake/recmeet-vulkan.cmake.
# Idempotent: include_guard() prevents double-parsing.

include_guard(GLOBAL)

set(RECMEET_DISTRO "unknown")
set(_distro_id "")
set(_distro_like "")

if(EXISTS "/etc/os-release")
    file(STRINGS "/etc/os-release" _os_release)
    foreach(_line IN LISTS _os_release)
        if(_line MATCHES "^ID=(.+)")
            string(STRIP "${CMAKE_MATCH_1}" _distro_id)
            string(REPLACE "\"" "" _distro_id "${_distro_id}")
            string(TOLOWER "${_distro_id}" _distro_id)
            break()
        endif()
    endforeach()
    foreach(_line IN LISTS _os_release)
        if(_line MATCHES "^ID_LIKE=(.+)")
            string(STRIP "${CMAKE_MATCH_1}" _distro_like)
            string(REPLACE "\"" "" _distro_like "${_distro_like}")
            string(TOLOWER "${_distro_like}" _distro_like)
            break()
        endif()
    endforeach()

    if(_distro_id STREQUAL "arch" OR _distro_like MATCHES "arch")
        set(RECMEET_DISTRO "arch")
    elseif(_distro_id MATCHES "^(debian|ubuntu|linuxmint|pop)$"
           OR _distro_like MATCHES "debian|ubuntu")
        set(RECMEET_DISTRO "debian")
    elseif(_distro_id MATCHES "^(fedora|rhel|centos|rocky|alma)$"
           OR _distro_like MATCHES "fedora|rhel")
        set(RECMEET_DISTRO "fedora")
    elseif(_distro_id STREQUAL "nixos")
        set(RECMEET_DISTRO "nixos")
    elseif(_distro_id STREQUAL "alpine")
        set(RECMEET_DISTRO "alpine")
    elseif(_distro_id STREQUAL "gentoo")
        set(RECMEET_DISTRO "gentoo")
    elseif(_distro_id MATCHES "^opensuse"
           OR _distro_like MATCHES "opensuse|suse")
        set(RECMEET_DISTRO "opensuse")
    endif()
endif()

message(STATUS "Detected distro family: ${RECMEET_DISTRO}")
