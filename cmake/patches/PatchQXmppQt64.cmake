set(_source "${QXMPP_SOURCE_DIR}/src/omemo/OmemoCryptoProvider.cpp")
if(NOT EXISTS "${_source}")
  message(FATAL_ERROR "QXmpp OMEMO source not found: ${_source}")
endif()

file(READ "${_source}" _contents)
set(_old "mac->addData(QByteArrayView(reinterpret_cast<const char *>(data), data_len));")
set(_new "mac->addData(reinterpret_cast<const char *>(data), qsizetype(data_len));")

string(FIND "${_contents}" "${_old}" _old_position)
string(FIND "${_contents}" "${_new}" _new_position)

if(NOT _old_position EQUAL -1)
  string(REPLACE "${_old}" "${_new}" _contents "${_contents}")
  file(WRITE "${_source}" "${_contents}")
  message(STATUS "Patched QXmpp 1.15.1 for QMessageAuthenticationCode on Qt 6.4")
elseif(_new_position EQUAL -1)
  message(FATAL_ERROR "Unsupported QXmpp OmemoCryptoProvider.cpp; Qt 6.4 compatibility patch could not be applied")
else()
  message(STATUS "QXmpp Qt 6.4 compatibility patch is already applied")
endif()

# QXmpp 1.15 discovers libomemo-c exclusively through pkg-config, which is commonly unavailable in native MSVC
# environments. Allow the enclosing bundled build to provide the already staged static libraries directly.
set(_root_cmake "${QXMPP_SOURCE_DIR}/CMakeLists.txt")
file(READ "${_root_cmake}" _root_contents)

set(_openssl_old "find_package(OpenSSL 3.0 QUIET)")
set(_openssl_new
    [=[find_package(OpenSSL QUIET CONFIG)
if(NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL 3.0 QUIET)
endif()]=])
if(_root_contents MATCHES "find_package\\(OpenSSL 3\\.0 QUIET\\)")
  string(REPLACE "${_openssl_old}" "${_openssl_new}" _root_contents "${_root_contents}")
endif()

set(_pkgconfig_end
    [=[endif()

include(GNUInstallDirs)]=])
set(_omemo_fallback
    [=[endif()

include(GNUInstallDirs)

if(NOT OmemoC_FOUND AND ANYKEEP_OMEMO_C_ROOT)
    add_library(PkgConfig::OmemoC INTERFACE IMPORTED)
    target_include_directories(PkgConfig::OmemoC INTERFACE
        "${ANYKEEP_OMEMO_C_ROOT}/${CMAKE_INSTALL_INCLUDEDIR}/omemo"
        "${ANYKEEP_OMEMO_C_ROOT}/${CMAKE_INSTALL_INCLUDEDIR}")
    target_link_libraries(PkgConfig::OmemoC INTERFACE
        "${ANYKEEP_OMEMO_C_ROOT}/${CMAKE_INSTALL_LIBDIR}/${CMAKE_STATIC_LIBRARY_PREFIX}omemo-c${CMAKE_STATIC_LIBRARY_SUFFIX}"
        "${ANYKEEP_OMEMO_C_ROOT}/${CMAKE_INSTALL_LIBDIR}/${CMAKE_STATIC_LIBRARY_PREFIX}protobuf-c${CMAKE_STATIC_LIBRARY_SUFFIX}")
    set(OmemoC_FOUND TRUE)
endif()]=])
if(NOT _root_contents MATCHES "ANYKEEP_OMEMO_C_ROOT")
  string(REPLACE "${_pkgconfig_end}" "${_omemo_fallback}" _root_contents "${_root_contents}")
endif()

if(NOT _root_contents MATCHES "ANYKEEP_OMEMO_C_ROOT")
  message(FATAL_ERROR "Unsupported QXmpp CMakeLists.txt; bundled libomemo-c fallback could not be applied")
endif()

file(WRITE "${_root_cmake}" "${_root_contents}")
