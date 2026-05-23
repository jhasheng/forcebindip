file(READ "${CMAKE_CURRENT_LIST_DIR}/../VERSION" FORCEBINDIP_VERSION)
string(STRIP "${FORCEBINDIP_VERSION}" FORCEBINDIP_VERSION)
if(NOT FORCEBINDIP_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
    message(FATAL_ERROR "VERSION must be numeric SemVer-like text, for example 0.1.0 or 0.1.0.0")
endif()

function(forcebindip_version_tweak out_var)
    if("${PROJECT_VERSION_TWEAK}" STREQUAL "")
        set("${out_var}" 0 PARENT_SCOPE)
    else()
        set("${out_var}" "${PROJECT_VERSION_TWEAK}" PARENT_SCOPE)
    endif()
endfunction()

function(forcebindip_configure_version_header)
    forcebindip_version_tweak(FORCEBINDIP_VERSION_TWEAK)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/forcebindip_version.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/forcebindip_version.h"
        @ONLY
    )
endfunction()

function(forcebindip_add_windows_version_resource target description original_filename file_type)
    forcebindip_version_tweak(FORCEBINDIP_VERSION_TWEAK)
    set(FORCEBINDIP_FILE_DESCRIPTION "${description}")
    set(FORCEBINDIP_ORIGINAL_FILENAME "${original_filename}")
    set(FORCEBINDIP_INTERNAL_NAME "${target}")
    set(FORCEBINDIP_FILE_TYPE "${file_type}")
    set(version_rc "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}_version.rc")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
        "${version_rc}"
        @ONLY
    )
    target_sources("${target}" PRIVATE "${version_rc}")
endfunction()
