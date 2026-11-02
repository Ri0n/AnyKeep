file(MAKE_DIRECTORY "${INSTALL_DIR}/include/sasl" "${INSTALL_DIR}/lib")
file(GLOB _cyrus_headers "${SOURCE_DIR}/include/*.h")
file(COPY ${_cyrus_headers} DESTINATION "${INSTALL_DIR}/include/sasl")
file(COPY "${SOURCE_DIR}/lib/libsasl2-static.lib" DESTINATION "${INSTALL_DIR}/lib")
