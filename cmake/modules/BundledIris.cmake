include(FetchContent)

set(ANYKEEP_BUNDLED_IRIS_GIT_REPOSITORY
    "https://github.com/psi-im/iris.git"
    CACHE STRING "Iris git repository")
# Keep the repository's default Iris revision source-controlled. A cache entry for the default pin silently survives
# source updates and can make a clean checkout build an older Iris revision.
set(_anykeep_iris_git_tag "870c5cc31a2ab84427b2a66f80c941d6413b85e3")
set(ANYKEEP_IRIS_GIT_TAG
    ""
    CACHE STRING "Override the Iris git tag, branch, or commit")
if(ANYKEEP_IRIS_GIT_TAG)
  set(_iris_git_tag "${ANYKEEP_IRIS_GIT_TAG}")
else()
  set(_iris_git_tag "${_anykeep_iris_git_tag}")
endif()
set(ANYKEEP_IRIS_SOURCE_DIR
    ""
    CACHE PATH "Local Iris source directory (avoids cloning the repository)")

# QCA is owned by AnyKeep's top-level dependency selection. Iris must never build or discover another copy when embedded
# here; it consumes the exact Qca3::Qca / Qca::Qca target already selected for libanykeep.
if(NOT ANYKEEP_QCA_TARGET)
  message(FATAL_ERROR "Bundled Iris requires the QCA target selected by AnyKeep")
endif()
if(NOT TARGET ${ANYKEEP_QCA_TARGET})
  message(FATAL_ERROR "Bundled Iris cannot see AnyKeep QCA target: ${ANYKEEP_QCA_TARGET}")
endif()
if(NOT "${ANYKEEP_QCA_TARGET}" STREQUAL "Qca3::Qca" AND NOT "${ANYKEEP_QCA_TARGET}" STREQUAL "Qca::Qca")
  message(FATAL_ERROR "Unexpected AnyKeep QCA target: ${ANYKEEP_QCA_TARGET}")
endif()

# Configure Iris as an in-tree dependency. This intentionally uses Iris's own Iris::Iris target instead of
# reconstructing its filename, include paths, compile definitions and transitive link interface here.
set(USE_QT6
    ON
    CACHE BOOL "Build Iris with Qt 6" FORCE)
set(IRIS_ENABLE_INSTALL
    OFF
    CACHE BOOL "Do not install bundled Iris SDK" FORCE)
set(IRIS_ENABLE_OMEMO
    ON
    CACHE BOOL "Enable Iris OMEMO support" FORCE)
set(IRIS_BUNDLED_OMEMO_C
    ON
    CACHE BOOL "Build Iris OMEMO dependencies" FORCE)
set(IRIS_BUNDLED_QCA
    OFF
    CACHE BOOL "Use AnyKeep's QCA instance" FORCE)
# SCTP data channels are required by AnyKeep's Iris backend for media/file transfer. Prefer a system UsrSCTP on Unix
# desktops, while platforms that do not normally provide a target UsrSCTP package build Iris's bundled copy.
set(IRIS_ENABLE_JINGLE_SCTP
    ON
    CACHE BOOL "Enable SCTP data-channel transport" FORCE)
if(ANDROID
   OR WIN32
   OR APPLE)
  set(IRIS_BUNDLED_USRSCTP
      ON
      CACHE BOOL "Build bundled UsrSCTP for Iris" FORCE)
else()
  set(IRIS_BUNDLED_USRSCTP
      OFF
      CACHE BOOL "Use system UsrSCTP for Iris" FORCE)
endif()
set(IRIS_BUILD_TOOLS
    OFF
    CACHE BOOL "Do not build Iris tools" FORCE)
# Iris is embedded as a static FetchContent target but is linked into the shared xmpppubsub plugin, so all Iris/internal
# static objects must be PIC. Keep this Iris-specific instead of changing CMAKE_POSITION_INDEPENDENT_CODE for the rest
# of AnyKeep.
set(IRIS_POSITION_INDEPENDENT_CODE
    ON
    CACHE BOOL "Build bundled Iris as PIC" FORCE)

set(IRIS_SYSTEM_QCA
    "${ANYKEEP_QCA_MAJOR}"
    CACHE STRING "QCA generation selected by AnyKeep" FORCE)

if(ANYKEEP_IRIS_SOURCE_DIR)
  if(NOT EXISTS "${ANYKEEP_IRIS_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "ANYKEEP_IRIS_SOURCE_DIR does not contain an Iris source tree: ${ANYKEEP_IRIS_SOURCE_DIR}")
  endif()
  FetchContent_Declare(anykeep_iris SOURCE_DIR "${ANYKEEP_IRIS_SOURCE_DIR}")
else()
  FetchContent_Declare(
    anykeep_iris
    GIT_REPOSITORY "${ANYKEEP_BUNDLED_IRIS_GIT_REPOSITORY}"
    GIT_TAG "${_iris_git_tag}"
    GIT_SHALLOW FALSE
    GIT_PROGRESS TRUE
    UPDATE_DISCONNECTED TRUE)
endif()

FetchContent_MakeAvailable(anykeep_iris)

if(NOT TARGET Iris::Iris)
  message(FATAL_ERROR "Bundled Iris did not provide the Iris::Iris CMake target")
endif()
