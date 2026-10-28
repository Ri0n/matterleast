# Select the Qt-compatible system QCA before considering the vendored fallback.
# QCA 2.3.4 includes the AES-256-GCM Cipher/AuthTag API required by MatterLeast.
set(MATTERLEAST_QCA2_MIN_VERSION "2.3.4")
set(MATTERLEAST_QCA_TARGET "")
set(MATTERLEAST_QCA_PROVIDER "")
option(MATTERLEAST_BUNDLED_QCA
  "Build QCA from the exact dependencies.lock.json commit, even if system QCA exists"
  ${WIN32})

if(NOT MATTERLEAST_BUNDLED_QCA)
  find_package(Qca3-qt${QT_VERSION_MAJOR}
    ${MATTERLEAST_QCA_LOCK_MIN_VERSION} CONFIG QUIET)
  if(TARGET Qca3::Qca)
    set(MATTERLEAST_QCA_TARGET Qca3::Qca)
    set(MATTERLEAST_QCA_PROVIDER "system-qca3")
  else()
    find_package(Qca-qt${QT_VERSION_MAJOR}
      ${MATTERLEAST_QCA2_MIN_VERSION} CONFIG QUIET)
    if(TARGET qca-qt${QT_VERSION_MAJOR})
      set(MATTERLEAST_QCA_TARGET qca-qt${QT_VERSION_MAJOR})
      set(MATTERLEAST_QCA_PROVIDER "system-qca2")
    endif()
  endif()
endif()
