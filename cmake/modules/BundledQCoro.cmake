include(FetchContent)

if(NOT QT_VERSION_MAJOR EQUAL 6)
    message(FATAL_ERROR "Bundled QCoro is supported only with Qt 6")
endif()

if(Qt6Core_VERSION VERSION_LESS 6.8)
    message(FATAL_ERROR "Bundled QCoro 0.13 requires Qt 6.8 or newer")
endif()

set(QTNOTE_BUNDLED_QCORO_VERSION "0.13.0" CACHE STRING "Bundled QCoro version")
set(QTNOTE_QCORO_SOURCE_DIR "" CACHE PATH "Local QCoro source directory (avoids cloning the release tag)")

function(qtnote_add_bundled_qcoro)
    # QtNote only uses QCoro::Core. Keep the bundled dependency small and let
    # the Android toolchain selected by the parent project build it for the
    # same ABI as qtnote_mobile.
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(USE_QT_VERSION 6)
    set(QCORO_BUILD_EXAMPLES OFF)
    set(QCORO_BUILD_TESTING OFF)
    set(QCORO_ENABLE_ASAN OFF)
    set(QCORO_WITH_QTDBUS OFF)
    set(QCORO_WITH_QTNETWORK OFF)
    set(QCORO_WITH_QTWEBSOCKETS OFF)
    set(QCORO_WITH_QTQUICK OFF)
    set(QCORO_WITH_QML OFF)
    set(QCORO_WITH_QTTEST OFF)

    if(QTNOTE_QCORO_SOURCE_DIR)
        if(NOT EXISTS "${QTNOTE_QCORO_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "QTNOTE_QCORO_SOURCE_DIR does not contain a QCoro source tree: "
                "${QTNOTE_QCORO_SOURCE_DIR}")
        endif()
        FetchContent_Declare(qtnote_bundled_qcoro
            SOURCE_DIR "${QTNOTE_QCORO_SOURCE_DIR}"
        )
    else()
        FetchContent_Declare(qtnote_bundled_qcoro
            GIT_REPOSITORY https://github.com/qcoro/qcoro.git
            GIT_TAG "v${QTNOTE_BUNDLED_QCORO_VERSION}"
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
        )
    endif()

    FetchContent_MakeAvailable(qtnote_bundled_qcoro)
endfunction()

qtnote_add_bundled_qcoro()

# QCoro 0.13 exports QCoro::Core when used through FetchContent. Keep a
# defensive alias for source snapshots that expose only the concrete target.
if(NOT TARGET QCoro::Core AND TARGET QCoro6Core)
    add_library(QCoro::Core ALIAS QCoro6Core)
endif()

if(NOT TARGET QCoro::Core)
    message(FATAL_ERROR "Bundled QCoro did not provide the QCoro::Core target")
endif()
