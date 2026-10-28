# CPack's builtin NSIS.template.in replaces the default directory with
# $DOCUMENTS for standard (JustMe) installs, even if INSTALL_ROOT is set.
# Documents may be redirected to OneDrive. Patch the version-matched CMake
# template at configuration time instead of vendoring 1000 lines of NSIS.
function(matterleast_prepare_per_user_nsis_template output_directory)
    set(_source "${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in")
    if(NOT EXISTS "${_source}")
        message(FATAL_ERROR "Cannot find the CPack NSIS template: ${_source}")
    endif()
    file(READ "${_source}" _template)

    # Refuse unknown future templates rather than silently installing into
    # Documents or requesting elevation.
    set(_admin [=[  RequestExecutionLevel admin]=])
    set(_user [=[  RequestExecutionLevel user]=])
    string(FIND "${_template}" "${_admin}" _admin_pos)
    if(_admin_pos EQUAL -1)
        message(FATAL_ERROR "Unknown CPack NSIS elevation template")
    endif()
    string(REPLACE "${_admin}" "${_user}" _template "${_template}")

    set(_init_begin [=[  StrCpy $SV_ALLUSERS "JustMe"]=])
    set(_init_end [=[  StrCmp "@CPACK_NSIS_MODIFY_PATH@" "ON" 0 noOptionsPage]=])
    string(FIND "${_template}" "${_init_begin}" _begin)
    string(FIND "${_template}" "${_init_end}" _end)
    if(_begin EQUAL -1 OR _end EQUAL -1 OR _end LESS _begin)
        message(FATAL_ERROR "Unknown CPack NSIS .onInit directory selection")
    endif()
    string(SUBSTRING "${_template}" 0 ${_begin} _before)
    string(SUBSTRING "${_template}" ${_end} -1 _after)
    # The original InstallDir handles the default and explicit /D=path.
    set(_template "${_before}  StrCpy $SV_ALLUSERS \"JustMe\"\n  SetShellVarContext current\n\n${_after}")

    set(_uninstall_begin [=[Function un.onInit]=])
    set(_uninstall_end [=[FunctionEnd]=])
    string(FIND "${_template}" "${_uninstall_begin}" _begin)
    if(_begin EQUAL -1)
        message(FATAL_ERROR "Missing NSIS uninstaller initialization")
    endif()
    string(SUBSTRING "${_template}" ${_begin} -1 _tail)
    string(FIND "${_tail}" "${_uninstall_end}" _relative_end)
    if(_relative_end EQUAL -1)
        message(FATAL_ERROR "Missing NSIS uninstaller initialization end")
    endif()
    math(EXPR _end "${_begin} + ${_relative_end}")
    string(SUBSTRING "${_template}" 0 ${_begin} _before)
    string(SUBSTRING "${_template}" ${_end} -1 _after)
    set(_template "${_before}Function un.onInit\n  SetShellVarContext current\n${_after}")

    # Component selection state and previous-install checks in CPack still
    # explicitly use HKLM. Move only the MatterLeast uninstall subtree to
    # HKCU; leave Windows system-information lookups unchanged.
    set(_machine_key [=[HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@]=])
    set(_user_key [=[HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@]=])
    string(FIND "${_template}" "${_machine_key}" _machine_pos)
    if(_machine_pos EQUAL -1)
        message(FATAL_ERROR "Unknown CPack NSIS registry template")
    endif()
    string(REPLACE "${_machine_key}" "${_user_key}" _template "${_template}")

    # Fail closed if a future CPack template reintroduces unsafe defaults.
    if(_template MATCHES "StrCpy \\$INSTDIR \"\\$DOCUMENTS"
        OR _template MATCHES "SetShellVarContext all"
        OR _template MATCHES "RequestExecutionLevel admin")
        message(FATAL_ERROR "NSIS per-user template still contains an unsafe install policy")
    endif()

    file(MAKE_DIRECTORY "${output_directory}")
    file(WRITE "${output_directory}/NSIS.template.in" "${_template}")
endfunction()
