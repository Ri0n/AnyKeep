include_guard(GLOBAL)

if(NOT ANDROID)
    return()
endif()

set(ANYKEEP_ANDROID_OPENSSL_ROOT "" CACHE PATH
    "Android OpenSSL bundle root, for example <Android SDK>/android_openssl/ssl_3")

if(NOT ANYKEEP_ANDROID_OPENSSL_ROOT)
    set(_anykeep_android_openssl_candidates
        "${ANDROID_SDK_ROOT}/android_openssl/ssl_3"
        "$ENV{ANDROID_SDK_ROOT}/android_openssl/ssl_3"
        "$ENV{ANDROID_HOME}/android_openssl/ssl_3"
        "$ENV{HOME}/Android/Sdk/android_openssl/ssl_3"
    )

    foreach(_anykeep_android_openssl_candidate IN LISTS _anykeep_android_openssl_candidates)
        if(EXISTS "${_anykeep_android_openssl_candidate}/include/openssl/opensslv.h")
            set(ANYKEEP_ANDROID_OPENSSL_ROOT
                "${_anykeep_android_openssl_candidate}"
                CACHE PATH
                "Android OpenSSL bundle root, for example <Android SDK>/android_openssl/ssl_3"
                FORCE)
            break()
        endif()
    endforeach()
endif()

if(NOT ANYKEEP_ANDROID_OPENSSL_ROOT)
    message(FATAL_ERROR
        "Android OpenSSL was not found. Install the Qt/KDAB android_openssl "
        "bundle under <Android SDK>/android_openssl/ssl_3 or set "
        "ANYKEEP_ANDROID_OPENSSL_ROOT explicitly.")
endif()

set(_anykeep_android_openssl_abi "${CMAKE_ANDROID_ARCH_ABI}")
if(NOT _anykeep_android_openssl_abi)
    set(_anykeep_android_openssl_abi "${ANDROID_ABI}")
endif()
if(NOT _anykeep_android_openssl_abi)
    message(FATAL_ERROR
        "Neither CMAKE_ANDROID_ARCH_ABI nor ANDROID_ABI is set; cannot select "
        "Android OpenSSL libraries from ${ANYKEEP_ANDROID_OPENSSL_ROOT}")
endif()

set(_anykeep_android_openssl_include
    "${ANYKEEP_ANDROID_OPENSSL_ROOT}/include")
set(_anykeep_android_openssl_lib_dir
    "${ANYKEEP_ANDROID_OPENSSL_ROOT}/${_anykeep_android_openssl_abi}")
set(_anykeep_android_openssl_ssl
    "${_anykeep_android_openssl_lib_dir}/libssl.a")
set(_anykeep_android_openssl_crypto
    "${_anykeep_android_openssl_lib_dir}/libcrypto.a")
set(_anykeep_android_openssl_ssl_runtime
    "${_anykeep_android_openssl_lib_dir}/libssl_3.so")
set(_anykeep_android_openssl_crypto_runtime
    "${_anykeep_android_openssl_lib_dir}/libcrypto_3.so")

if(NOT EXISTS "${_anykeep_android_openssl_include}/openssl/opensslv.h")
    message(FATAL_ERROR
        "Android OpenSSL headers were not found under "
        "${_anykeep_android_openssl_include}")
endif()
if(NOT EXISTS "${_anykeep_android_openssl_ssl}" OR
   NOT EXISTS "${_anykeep_android_openssl_crypto}")
    message(FATAL_ERROR
        "Android OpenSSL bundle does not contain static libraries for "
        "${_anykeep_android_openssl_abi}: ${ANYKEEP_ANDROID_OPENSSL_ROOT}")
endif()
if(NOT EXISTS "${_anykeep_android_openssl_ssl_runtime}" OR
   NOT EXISTS "${_anykeep_android_openssl_crypto_runtime}")
    message(FATAL_ERROR
        "Android OpenSSL bundle does not contain the runtime libraries "
        "libssl_3.so and libcrypto_3.so for ${_anykeep_android_openssl_abi}: "
        "${_anykeep_android_openssl_lib_dir}")
endif()

# Qt Network's OpenSSL TLS backend resolves OpenSSL dynamically at runtime.
# The static archives above are used while building bundled QCA and QXmpp, but
# they do not make TLS available to QSslSocket inside the APK. Keep the matching
# shared libraries as an explicit, cache-visible list so any Android executable
# can deploy the same OpenSSL instance without repeating path logic.
set(ANYKEEP_ANDROID_OPENSSL_RUNTIME_LIBRARIES
    "${_anykeep_android_openssl_crypto_runtime};${_anykeep_android_openssl_ssl_runtime}"
    CACHE INTERNAL "Android OpenSSL runtime libraries packaged with AnyKeep")

# Do not call FindOpenSSL while cross-compiling. On Unix it consults the host
# pkg-config database even when exact Android archive paths are supplied. With
# a modern host OpenSSL that can leak host-only private dependencies such as
# -lzstd or -l:libjitterentropy.a into the Android link command.
#
# Create the standard imported targets directly from the already selected
# per-ABI files. QCA and QXmpp then share exactly the same Android OpenSSL
# instance without inheriting anything from the build host.
set(OPENSSL_ROOT_DIR
    "${ANYKEEP_ANDROID_OPENSSL_ROOT}"
    CACHE PATH "OpenSSL root directory" FORCE)
set(OPENSSL_INCLUDE_DIR
    "${_anykeep_android_openssl_include}"
    CACHE PATH "OpenSSL include directory" FORCE)
set(OPENSSL_SSL_LIBRARY
    "${_anykeep_android_openssl_ssl}"
    CACHE FILEPATH "OpenSSL SSL library" FORCE)
set(OPENSSL_CRYPTO_LIBRARY
    "${_anykeep_android_openssl_crypto}"
    CACHE FILEPATH "OpenSSL Crypto library" FORCE)
set(OPENSSL_USE_STATIC_LIBS ON CACHE BOOL
    "Prefer static OpenSSL libraries" FORCE)
set(OPENSSL_CRYPTO_LIBRARIES "${OPENSSL_CRYPTO_LIBRARY}")
set(OPENSSL_SSL_LIBRARIES "${OPENSSL_SSL_LIBRARY};${OPENSSL_CRYPTO_LIBRARY}")
set(OPENSSL_LIBRARIES "${OPENSSL_SSL_LIBRARIES}")
set(OpenSSL_FOUND TRUE)
set(OPENSSL_FOUND TRUE)

if(NOT TARGET OpenSSL::Crypto)
    add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    )
endif()

if(NOT TARGET OpenSSL::SSL)
    add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES OpenSSL::Crypto
    )
endif()

function(anykeep_deploy_android_openssl)
    foreach(_anykeep_android_openssl_target IN LISTS ARGN)
        if(NOT TARGET "${_anykeep_android_openssl_target}")
            message(FATAL_ERROR
                "anykeep_deploy_android_openssl(): target "
                "'${_anykeep_android_openssl_target}' does not exist")
        endif()

        get_target_property(_anykeep_android_extra_libs
            "${_anykeep_android_openssl_target}" QT_ANDROID_EXTRA_LIBS)
        if(NOT _anykeep_android_extra_libs OR
           _anykeep_android_extra_libs MATCHES "-NOTFOUND$")
            set(_anykeep_android_extra_libs)
        endif()
        list(APPEND _anykeep_android_extra_libs
            ${ANYKEEP_ANDROID_OPENSSL_RUNTIME_LIBRARIES})
        list(REMOVE_DUPLICATES _anykeep_android_extra_libs)
        set_property(TARGET "${_anykeep_android_openssl_target}" PROPERTY
            QT_ANDROID_EXTRA_LIBS "${_anykeep_android_extra_libs}")

        message(STATUS
            "AnyKeep: packaging Android OpenSSL runtime libraries for "
            "${_anykeep_android_openssl_target}: "
            "${ANYKEEP_ANDROID_OPENSSL_RUNTIME_LIBRARIES}")
    endforeach()
endfunction()

message(STATUS
    "AnyKeep: using Android OpenSSL from ${ANYKEEP_ANDROID_OPENSSL_ROOT} "
    "for ${_anykeep_android_openssl_abi} (host pkg-config disabled)")
