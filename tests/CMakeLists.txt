find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Test REQUIRED)

add_executable(noteblockmodel_test noteblockmodel_test.cpp)
target_link_libraries(noteblockmodel_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME noteblockmodel_test COMMAND noteblockmodel_test)
set_tests_properties(noteblockmodel_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(dialogservice_test dialogservice_test.cpp)
target_link_libraries(dialogservice_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME dialogservice_test COMMAND dialogservice_test)

add_executable(noteeditor_test noteeditor_test.cpp)
target_link_libraries(noteeditor_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME noteeditor_test COMMAND noteeditor_test)

add_executable(notefragment_test notefragment_test.cpp)
target_link_libraries(notefragment_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME notefragment_test COMMAND notefragment_test)

add_executable(notetransfercontroller_test notetransfercontroller_test.cpp)
target_link_libraries(notetransfercontroller_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME notetransfercontroller_test COMMAND notetransfercontroller_test)
set_tests_properties(notetransfercontroller_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(notefragmentmediatransfer_test notefragmentmediatransfer_test.cpp)
target_link_libraries(notefragmentmediatransfer_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME notefragmentmediatransfer_test COMMAND notefragmentmediatransfer_test)

add_executable(desktopnoteeditorhost_test desktopnoteeditorhost_test.cpp)
target_link_libraries(desktopnoteeditorhost_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::QuickWidgets)
add_test(NAME desktopnoteeditorhost_test COMMAND desktopnoteeditorhost_test)
set_tests_properties(desktopnoteeditorhost_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software")

add_executable(qmlnoteeditor_test
    qmlnoteeditor_test.cpp
    qmlnoteeditor.cpp
    qmlnoteeditor.h
)
target_link_libraries(qmlnoteeditor_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::QuickWidgets)
add_test(NAME qmlnoteeditor_test COMMAND qmlnoteeditor_test)
set_tests_properties(qmlnoteeditor_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software")

add_executable(filedraftstore_test filedraftstore_test.cpp)
target_link_libraries(filedraftstore_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME filedraftstore_test COMMAND filedraftstore_test)

add_executable(fileremotecachestore_test fileremotecachestore_test.cpp)
target_link_libraries(fileremotecachestore_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME fileremotecachestore_test COMMAND fileremotecachestore_test)

add_executable(secureenvelope_test secureenvelope_test.cpp)
target_link_libraries(secureenvelope_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME secureenvelope_test COMMAND secureenvelope_test)

add_executable(localmediastore_test localmediastore_test.cpp)
target_link_libraries(localmediastore_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME localmediastore_test COMMAND localmediastore_test)

add_executable(xmppnotecodec_test
    xmppnotecodec_test.cpp
    ../plugins/xmpppubsub/xmppnotecodec.cpp
)
target_include_directories(xmppnotecodec_test PRIVATE ../plugins/xmpppubsub)
target_link_libraries(xmppnotecodec_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME xmppnotecodec_test COMMAND xmppnotecodec_test)

if(TARGET QXmpp::QXmpp)
    add_executable(xmppkeysyncextension_test
        xmppkeysyncextension_test.cpp
        ../plugins/xmpppubsub/xmppkeysyncextension.cpp
    )
    target_include_directories(xmppkeysyncextension_test PRIVATE ../plugins/xmpppubsub)
    target_link_libraries(xmppkeysyncextension_test PRIVATE Qt${QT_VERSION_MAJOR}::Test QXmpp::QXmpp)
    qtnote_configure_qxmpp_target(xmppkeysyncextension_test)
    add_test(NAME xmppkeysyncextension_test COMMAND xmppkeysyncextension_test)

    add_executable(xmppomemopubsubitems_test
        xmppomemopubsubitems_test.cpp
        ../plugins/xmpppubsub/xmppomemopubsubitems.cpp
    )
    target_include_directories(xmppomemopubsubitems_test PRIVATE ../plugins/xmpppubsub)
    target_link_libraries(xmppomemopubsubitems_test PRIVATE Qt${QT_VERSION_MAJOR}::Test QXmpp::QXmpp)
    qtnote_configure_qxmpp_target(xmppomemopubsubitems_test)
    add_test(NAME xmppomemopubsubitems_test COMMAND xmppomemopubsubitems_test)

    add_executable(xmpperror_test
        xmpperror_test.cpp
        ../plugins/xmpppubsub/xmpperror.cpp
    )
    target_include_directories(xmpperror_test PRIVATE ../plugins/xmpppubsub)
    target_link_libraries(xmpperror_test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Network QXmpp::QXmpp)
    qtnote_configure_qxmpp_target(xmpperror_test)
    add_test(NAME xmpperror_test COMMAND xmpperror_test)

    add_executable(xmpppersistenttruststorage_test
        xmpppersistenttruststorage_test.cpp
        ../plugins/xmpppubsub/xmpppersistenttruststorage.cpp
    )
    target_include_directories(xmpppersistenttruststorage_test PRIVATE ../plugins/xmpppubsub)
    target_link_libraries(xmpppersistenttruststorage_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test QXmpp::QXmpp)
    qtnote_configure_qxmpp_target(xmpppersistenttruststorage_test)
    add_test(NAME xmpppersistenttruststorage_test COMMAND xmpppersistenttruststorage_test)

    if(TARGET QXmpp::Omemo)
        add_executable(xmppomemostorage_test
            xmppomemostorage_test.cpp
            ../plugins/xmpppubsub/xmppomemostorage.cpp
        )
        target_include_directories(xmppomemostorage_test PRIVATE ../plugins/xmpppubsub)
        target_link_libraries(xmppomemostorage_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test QXmpp::Omemo)
        qtnote_configure_qxmpp_target(xmppomemostorage_test)
        add_test(NAME xmppomemostorage_test COMMAND xmppomemostorage_test)
    endif()
endif()
