# Compile-time update source and trust policy.  Neither the update URL nor
# the verification key may be changed by untrusted server responses.
#
# The default GitHub update pointer is public/unsigned.  Corporate builds
# must explicitly choose an HTTPS URL and authenticate its raw manifest bytes.
option(MATTERLEAST_ENABLE_UPDATE_CHECKS
    "Compile support for checking for MatterLeast updates" ON)

# Nightly installers should start on the nightly feed. An explicit user
# preference in QSettings takes precedence and survives future upgrades.
set(MATTERLEAST_UPDATE_DEFAULT_CHANNEL "stable" CACHE STRING
    "Default Windows update channel for new installations (stable or nightly)")
set_property(CACHE MATTERLEAST_UPDATE_DEFAULT_CHANNEL PROPERTY STRINGS stable nightly)
if(NOT MATTERLEAST_UPDATE_DEFAULT_CHANNEL MATCHES "^(stable|nightly)$")
    message(FATAL_ERROR
        "MATTERLEAST_UPDATE_DEFAULT_CHANNEL must be stable or nightly")
endif()

set(_matterleast_default_update_url
    "https://github.com/Ri0n/matterleast/releases/download/updates-{channel}/windows-x64.json")
set(MATTERLEAST_UPDATE_MANIFEST_URL "${_matterleast_default_update_url}" CACHE STRING
    "HTTPS URL for the update JSON manifest; {channel} becomes stable or nightly")
set(MATTERLEAST_UPDATE_VERIFY_KEY_FILE "" CACHE FILEPATH
    "RSA/SHA-256 PEM public key for authenticating detached .sig update manifests")

# A plugin verification key already defines a corporate trust boundary.  Reuse
# that key for updates by default; an explicitly supplied update key overrides.
set(_matterleast_update_key_file "${MATTERLEAST_UPDATE_VERIFY_KEY_FILE}")
if(NOT _matterleast_update_key_file AND MATTERLEAST_PLUGIN_VERIFY_KEY_FILE)
    set(_matterleast_update_key_file "${MATTERLEAST_PLUGIN_VERIFY_KEY_FILE}")
endif()

if(MATTERLEAST_PLUGIN_VERIFY_KEY_FILE
    OR MATTERLEAST_PLUGIN_SIGNING_KEY_FILE
    OR MATTERLEAST_UPDATE_VERIFY_KEY_FILE)
    if(MATTERLEAST_UPDATE_MANIFEST_URL STREQUAL _matterleast_default_update_url)
        message(FATAL_ERROR
            "A signed corporate build must explicitly set MATTERLEAST_UPDATE_MANIFEST_URL; "
            "the default public GitHub update endpoint cannot be trusted with a corporate key")
    endif()
endif()

if(MATTERLEAST_ENABLE_UPDATE_CHECKS)
    if(NOT MATTERLEAST_UPDATE_MANIFEST_URL MATCHES "^https://[^/ \t\r\n?#]+/[^ \t\r\n?#]*$")
        message(FATAL_ERROR "MATTERLEAST_UPDATE_MANIFEST_URL must be an absolute HTTPS URL")
    endif()
    if(MATTERLEAST_UPDATE_MANIFEST_URL MATCHES "@|\\\\|[;]")
        message(FATAL_ERROR "Update URL may not contain userinfo, backslashes, or CMake list separators")
    endif()
    string(REPLACE "{channel}" "" _matterleast_url_without_channel
        "${MATTERLEAST_UPDATE_MANIFEST_URL}")
    if(_matterleast_url_without_channel MATCHES "[{}]")
        message(FATAL_ERROR "Only {channel} is supported in the update URL")
    endif()
    if(MATTERLEAST_UPDATE_MANIFEST_URL MATCHES "\\{channel\\}")
        string(REPLACE "{channel}" "stable" _matterleast_stable_url
            "${MATTERLEAST_UPDATE_MANIFEST_URL}")
        string(REPLACE "{channel}" "nightly" _matterleast_nightly_url
            "${MATTERLEAST_UPDATE_MANIFEST_URL}")
    else()
        set(_matterleast_stable_url "${MATTERLEAST_UPDATE_MANIFEST_URL}")
        set(_matterleast_nightly_url "${MATTERLEAST_UPDATE_MANIFEST_URL}")
    endif()
endif()

set(MATTERLEAST_UPDATE_SIGNATURE_REQUIRED 0)
if(_matterleast_update_key_file)
    if(NOT EXISTS "${_matterleast_update_key_file}")
        message(FATAL_ERROR "Update verification public key does not exist: ${_matterleast_update_key_file}")
    endif()
    # Do not embed update key material when update checking is compiled out.
    if(MATTERLEAST_ENABLE_UPDATE_CHECKS)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_matterleast_update_key_file}")
        file(READ "${_matterleast_update_key_file}" MATTERLEAST_UPDATE_PUBLIC_KEY_PEM)
        if(NOT MATTERLEAST_UPDATE_PUBLIC_KEY_PEM MATCHES "-----BEGIN PUBLIC KEY-----")
            message(FATAL_ERROR "Update verification key must contain a PEM PUBLIC KEY")
        endif()
        set(MATTERLEAST_UPDATE_SIGNATURE_REQUIRED 1)
    endif()
endif()

if(MATTERLEAST_ENABLE_UPDATE_CHECKS)
    set(MATTERLEAST_UPDATE_CHECKS_COMPILED 1)
else()
    set(MATTERLEAST_UPDATE_CHECKS_COMPILED 0)
    set(_matterleast_stable_url "")
    set(_matterleast_nightly_url "")
endif()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_SOURCE_DIR}/sources/update/UpdateTrustConfig.h.in"
    "${CMAKE_BINARY_DIR}/generated/UpdateTrustConfig.h" @ONLY)

message(STATUS "MatterLeast update checks: ${MATTERLEAST_ENABLE_UPDATE_CHECKS}")
message(STATUS "MatterLeast default update channel: ${MATTERLEAST_UPDATE_DEFAULT_CHANNEL}")
if(MATTERLEAST_ENABLE_UPDATE_CHECKS)
    message(STATUS "MatterLeast update source: ${MATTERLEAST_UPDATE_MANIFEST_URL}")
endif()
if(MATTERLEAST_UPDATE_SIGNATURE_REQUIRED)
    message(STATUS "MatterLeast update signatures: required (RSA/SHA-256)")
endif()
