# The Windows install identity is not the numeric application version.
# A rolling nightly must use the *same* buildId in the download manifest,
# the on-disk versions/<id> directory, and the initial launcher's fallback.
# The numeric VERSIONINFO / PROJECT_VERSION remains the latest stable number.
function(matterleast_resolve_windows_install_identity version default_channel)
    set(MATTERLEAST_INSTALL_ID "" CACHE STRING
        "Windows install identity (empty: v<version>; nightly: nightly-YYYYMMDD-HHMMSS-sha)")
    if(MATTERLEAST_INSTALL_ID STREQUAL "")
        set(_identity "v${version}")
    else()
        set(_identity "${MATTERLEAST_INSTALL_ID}")
    endif()

    if(_identity STREQUAL "v${version}")
        if(NOT default_channel STREQUAL "stable")
            message(FATAL_ERROR
                "Nightly channel requires a unique MATTERLEAST_INSTALL_ID (nightly buildId)")
        endif()
    elseif(_identity MATCHES "^nightly-([0-9]+)-([0-9]+)-([a-f0-9]+)$")
        set(_date "${CMAKE_MATCH_1}")
        set(_time "${CMAKE_MATCH_2}")
        set(_sha "${CMAKE_MATCH_3}")
        string(LENGTH "${_date}" _date_length)
        string(LENGTH "${_time}" _time_length)
        string(LENGTH "${_sha}" _sha_length)
        if(NOT _date_length EQUAL 8 OR NOT _time_length EQUAL 6
            OR _sha_length LESS 7 OR _sha_length GREATER 40)
            message(FATAL_ERROR "Invalid nightly MATTERLEAST_INSTALL_ID: ${_identity}")
        endif()
        if(NOT default_channel STREQUAL "nightly")
            message(FATAL_ERROR
                "A nightly MATTERLEAST_INSTALL_ID requires the nightly update channel")
        endif()
    else()
        message(FATAL_ERROR "Invalid MATTERLEAST_INSTALL_ID: ${_identity}")
    endif()

    set(MATTERLEAST_INSTALL_ID "${_identity}" PARENT_SCOPE)
endfunction()
