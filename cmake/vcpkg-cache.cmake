# Initialize before project() loads the vcpkg toolchain. Explicit -D/cache values win.
if(DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED APIGATE_DEPENDENCY_PRESET)
    return()
endif()

if(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(_apigate_cache_root "$ENV{XDG_CACHE_HOME}")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(_apigate_cache_root "$ENV{HOME}/.cache")
else()
    message(FATAL_ERROR "Set XDG_CACHE_HOME, HOME, or VCPKG_INSTALLED_DIR for dependency storage.")
endif()

set(VCPKG_INSTALLED_DIR
    "${_apigate_cache_root}/apigate/vcpkg/installed/${APIGATE_DEPENDENCY_PRESET}"
    CACHE PATH "Installed dependencies for the selected ApiGate preset")
unset(_apigate_cache_root)
