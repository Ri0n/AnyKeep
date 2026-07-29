find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Test Xml REQUIRED)

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

add_executable(ptfstorage_test ptfstorage_test.cpp)
target_link_libraries(ptfstorage_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME ptfstorage_test COMMAND ptfstorage_test)

add_executable(notesindex_test notesindex_test.cpp)
target_link_libraries(notesindex_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME notesindex_test COMMAND notesindex_test)

add_executable(filedraftstore_test filedraftstore_test.cpp)
target_link_libraries(filedraftstore_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME filedraftstore_test COMMAND filedraftstore_test)

add_executable(foldercatalog_test foldercatalog_test.cpp)
target_link_libraries(foldercatalog_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME foldercatalog_test COMMAND foldercatalog_test)

add_executable(foldercatalogmanager_test foldercatalogmanager_test.cpp)
target_link_libraries(foldercatalogmanager_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME foldercatalogmanager_test COMMAND foldercatalogmanager_test)

add_executable(foldernotesmodel_test foldernotesmodel_test.cpp)
target_link_libraries(foldernotesmodel_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME foldernotesmodel_test COMMAND foldernotesmodel_test)

add_executable(folderoperationscontroller_test folderoperationscontroller_test.cpp)
target_link_libraries(folderoperationscontroller_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME folderoperationscontroller_test COMMAND folderoperationscontroller_test)

add_executable(notesworkspacefolders_test notesworkspacefolders_test.cpp)
target_link_libraries(notesworkspacefolders_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME notesworkspacefolders_test COMMAND notesworkspacefolders_test)
set_tests_properties(notesworkspacefolders_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software")

add_executable(fileremotecachestore_test fileremotecachestore_test.cpp)
target_link_libraries(fileremotecachestore_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME fileremotecachestore_test COMMAND fileremotecachestore_test)

add_executable(nextcloudcategory_test
    nextcloudcategory_test.cpp
    ../plugins/nextcloud/nextcloudcategory.cpp
)
target_include_directories(nextcloudcategory_test PRIVATE ../plugins/nextcloud)
target_link_libraries(nextcloudcategory_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME nextcloudcategory_test COMMAND nextcloudcategory_test)

add_executable(nextcloudworker_test
    nextcloudworker_test.cpp
    ../plugins/nextcloud/nextcloudworker.cpp
)
target_include_directories(nextcloudworker_test PRIVATE ../plugins/nextcloud)
target_link_libraries(nextcloudworker_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Network)
add_test(NAME nextcloudworker_test COMMAND nextcloudworker_test)

add_executable(nextcloudstorage_test
    nextcloudstorage_test.cpp
    ../plugins/nextcloud/nextcloudcategory.cpp
    ../plugins/nextcloud/nextcloudstorage.cpp
    ../plugins/nextcloud/nextcloudworker.cpp
)
target_include_directories(nextcloudstorage_test PRIVATE ../plugins/nextcloud)
target_link_libraries(nextcloudstorage_test PRIVATE qtnote Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Network)
add_test(NAME nextcloudstorage_test COMMAND nextcloudstorage_test)

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
target_link_libraries(xmppnotecodec_test PRIVATE
    qtnote
    Qt${QT_VERSION_MAJOR}::Test
    Qt${QT_VERSION_MAJOR}::Xml
)
add_test(NAME xmppnotecodec_test COMMAND xmppnotecodec_test)

if(TARGET QXmpp::QXmpp)
    add_executable(qtnotepubsubitem_test
        qtnotepubsubitem_test.cpp
        ../plugins/xmpppubsub/qtnotepubsubitem.cpp
        ../plugins/xmpppubsub/xmpppepextension.cpp
    )
    target_include_directories(qtnotepubsubitem_test PRIVATE ../plugins/xmpppubsub)
    target_link_libraries(qtnotepubsubitem_test PRIVATE
        qtnote
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Xml
        QXmpp::QXmpp
    )
    qtnote_configure_qxmpp_target(qtnotepubsubitem_test)
    add_test(NAME qtnotepubsubitem_test COMMAND qtnotepubsubitem_test)

    add_executable(xmppkeyresolutioncontroller_test
        xmppkeyresolutioncontroller_test.cpp
        ../plugins/xmpppubsub/xmppkeyresolutioncontroller.cpp
    )
    target_include_directories(xmppkeyresolutioncontroller_test PRIVATE ../plugins/xmpppubsub)
    target_link_libraries(xmppkeyresolutioncontroller_test PRIVATE Qt${QT_VERSION_MAJOR}::Test QXmpp::QXmpp)
    qtnote_configure_qxmpp_target(xmppkeyresolutioncontroller_test)
    add_test(NAME xmppkeyresolutioncontroller_test COMMAND xmppkeyresolutioncontroller_test)

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
