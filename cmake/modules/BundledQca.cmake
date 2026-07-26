include(ExternalProject)
include(GNUInstallDirs)
include(ProcessorCount)

if(NOT QT_VERSION_MAJOR EQUAL 6)
    message(FATAL_ERROR "Bundled QCA is supported only with Qt 6")
endif()

set(QTNOTE_BUNDLED_QCA_GIT_REPOSITORY "https://github.com/psi-im/qca.git" CACHE STRING "Bundled QCA git repository")
set(QTNOTE_BUNDLED_QCA_GIT_TAG "master" CACHE STRING "Bundled QCA git tag or branch")
set(QTNOTE_QCA_SOURCE_DIR "" CACHE PATH "Local QCA source directory (avoids cloning the repository)")

ProcessorCount(_qca_detected_jobs)
if(NOT _qca_detected_jobs)
    set(_qca_detected_jobs 2)
endif()
set(QTNOTE_BUNDLED_QCA_JOBS "${_qca_detected_jobs}" CACHE STRING "Parallel jobs used to build bundled QCA")

if(Qt6Core_DIR AND NOT Qt6Test_DIR)
    get_filename_component(_qtnote_qt6_cmake_root "${Qt6Core_DIR}/.." ABSOLUTE)
    if(EXISTS "${_qtnote_qt6_cmake_root}/Qt6Test/Qt6TestConfig.cmake")
        set(Qt6Test_DIR "${_qtnote_qt6_cmake_root}/Qt6Test" CACHE PATH "Qt6Test package directory" FORCE)
    endif()
endif()

if(ANDROID)
    include(AndroidOpenSSL)
else()
    find_package(OpenSSL REQUIRED)
endif()

set(_qca_prefix "${CMAKE_BINARY_DIR}/_deps/qca")
set(_qca_install_dir "${_qca_prefix}/install")
set(_qca_include_dir "${_qca_install_dir}/${CMAKE_INSTALL_INCLUDEDIR}/Qca-qt6/QtCrypto")
set(_qca_library_dir "${_qca_install_dir}/${CMAKE_INSTALL_LIBDIR}")
set(_qca_library "${_qca_library_dir}/${CMAKE_STATIC_LIBRARY_PREFIX}qca-qt6${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(_qca_ossl_plugin "${_qca_library_dir}/qca-qt6/crypto/${CMAKE_STATIC_LIBRARY_PREFIX}qca-ossl${CMAKE_STATIC_LIBRARY_SUFFIX}")

if(QTNOTE_QCA_SOURCE_DIR)
    if(NOT EXISTS "${QTNOTE_QCA_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "QTNOTE_QCA_SOURCE_DIR does not contain a QCA source tree: ${QTNOTE_QCA_SOURCE_DIR}")
    endif()
    set(_qca_source_args
        SOURCE_DIR "${QTNOTE_QCA_SOURCE_DIR}"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
    )
else()
    set(_qca_source_args
        GIT_REPOSITORY "${QTNOTE_BUNDLED_QCA_GIT_REPOSITORY}"
        GIT_TAG "${QTNOTE_BUNDLED_QCA_GIT_TAG}"
        UPDATE_COMMAND ""
    )
endif()

set(_qca_cmake_args
    "-DBUILD_SHARED_LIBS=OFF"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    "-DBUILD_PLUGINS=ossl"
    "-DLOAD_SHARED_PLUGINS=OFF"
    "-DBUILD_TESTS=OFF"
    "-DBUILD_TOOLS=OFF"
    "-DBUILD_WITH_QT6=ON"
    "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
    "-DCMAKE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
    "-DCMAKE_INSTALL_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
    "-DQt6_DIR=${Qt6_DIR}"
    "-DQt6Core_DIR=${Qt6Core_DIR}"
    "-DQt6Test_DIR=${Qt6Test_DIR}"
    "-DQT_HOST_PATH=${QT_HOST_PATH}"
    "-DQT_HOST_PATH_CMAKE_DIR=${QT_HOST_PATH_CMAKE_DIR}"
    "-DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}"
    "-DOPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}"
    "-DOPENSSL_SSL_LIBRARY=${OPENSSL_SSL_LIBRARY}"
    "-DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_CRYPTO_LIBRARY}"
    "-DOPENSSL_USE_STATIC_LIBS=${OPENSSL_USE_STATIC_LIBS}"
    "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DANDROID_ABI=${ANDROID_ABI}"
    "-DCMAKE_ANDROID_ARCH_ABI=${ANDROID_ABI}"
    "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
    "-DANDROID_NDK=${ANDROID_NDK}"
    "-DOSX_FRAMEWORK=OFF"
)

ExternalProject_Add(qtnote_bundled_qca
    ${_qca_source_args}
    PREFIX "${_qca_prefix}"
    INSTALL_DIR "${_qca_install_dir}"
    CMAKE_ARGS ${_qca_cmake_args}
    BUILD_COMMAND
        "${CMAKE_COMMAND}" --build <BINARY_DIR>
        --parallel "${QTNOTE_BUNDLED_QCA_JOBS}"
    BUILD_BYPRODUCTS
        "${_qca_library}"
        "${_qca_ossl_plugin}"
)

file(MAKE_DIRECTORY "${_qca_include_dir}")

add_library(qca-qt6 STATIC IMPORTED GLOBAL)
set_target_properties(qca-qt6 PROPERTIES
    IMPORTED_LOCATION "${_qca_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_qca_include_dir}"
    INTERFACE_LINK_LIBRARIES "${_qca_ossl_plugin};OpenSSL::SSL;OpenSSL::Crypto;Qt6::Core"
    INTERFACE_COMPILE_DEFINITIONS QCA_STATIC
)
add_dependencies(qca-qt6 qtnote_bundled_qca)
