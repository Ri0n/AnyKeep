find_package(
  Qt6
  COMPONENTS Test Xml
  REQUIRED)

add_library(anykeep_headless_test_init OBJECT headlesstestinit.cpp)
target_link_libraries(anykeep_headless_test_init PRIVATE Qt6::Core)
set_target_properties(anykeep_headless_test_init PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

add_library(anykeep_qca_test_init OBJECT qcatestinit.cpp)
target_link_libraries(anykeep_qca_test_init PRIVATE anykeep)
set_target_properties(anykeep_qca_test_init PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

set(anykeep_test_results_dir "${CMAKE_BINARY_DIR}/test-results")
file(MAKE_DIRECTORY "${anykeep_test_results_dir}")

function(register_anykeep_test target)
  if(WIN32)
    # On Windows QtTest's console logger bypasses the stdout pipe captured by CTest. The wrapper reads the incremental
    # text report back into CTest's output and also retains JUnit XML for CI artifacts.
    add_test(
      NAME ${target}
      COMMAND
        ${CMAKE_COMMAND} "-DANYKEEP_TEST_EXECUTABLE=$<TARGET_FILE:${target}>"
        "-DANYKEEP_TEST_TEXT_REPORT=${anykeep_test_results_dir}/${target}.txt"
        "-DANYKEEP_TEST_JUNIT_REPORT=${anykeep_test_results_dir}/${target}.xml" -P
        "${CMAKE_CURRENT_LIST_DIR}/runqttest.cmake")
  else()
    add_test(NAME ${target} COMMAND ${target})
  endif()
endfunction()

function(add_anykeep_test target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE anykeep Qt6::Test)
  register_anykeep_test(${target})
endfunction()

add_anykeep_test(
  noteblockmodel_test
  noteblockmodel_test.cpp
  noteblockmodel_test.h
  noteblockmodel_markdown_test.cpp
  noteblockmodel_operations_test.cpp
  noteblockmodel_lists_tables_test.cpp
  noteblockmodel_structure_test.cpp
  noteblockmodel_search_code_test.cpp)

add_anykeep_test(dialogservice_test dialogservice_test.cpp)

add_anykeep_test(noteeditor_test noteeditor_test.cpp)

add_anykeep_test(notefragment_test notefragment_test.cpp)

add_anykeep_test(notetransfercontroller_test notetransfercontroller_test.cpp)

add_anykeep_test(notefragmentmediatransfer_test notefragmentmediatransfer_test.cpp)

add_anykeep_test(desktopnoteeditorhost_test desktopnoteeditorhost_test.cpp)
target_link_libraries(desktopnoteeditorhost_test PRIVATE Qt6::QuickWidgets)

add_anykeep_test(editorqml_test editorqml_test.cpp)
target_link_libraries(editorqml_test PRIVATE Qt6::QuickWidgets)

add_anykeep_test(
  notesmanagerqml_test
  notesmanagerqml_test.cpp
  notesmanagerqml_test.h
  notesmanagerqml_shell_test.cpp
  notesmanagerqml_drag_test.cpp
  managerinteractionfixture.cpp
  managerinteractionfixture.h
  notecollectionqml_test.cpp
  foldersqml_test.cpp
  desktopqmltestsupport.h
  notesmanagerqml_test.qrc)
target_link_libraries(notesmanagerqml_test PRIVATE Qt6::QuickWidgets)
set_target_properties(notesmanagerqml_test PROPERTIES AUTORCC ON)

add_anykeep_test(settingsqml_test settingsqml_test.cpp desktopqmltestsupport.h)
target_link_libraries(settingsqml_test PRIVATE Qt6::QuickWidgets)

add_custom_target(desktop_qml_tests DEPENDS desktopnoteeditorhost_test editorqml_test notesmanagerqml_test
                                            settingsqml_test)

# One stable entry point for structured-editor work.  The target builds the model/controller/QML layers together, while
# the matching CTest label runs exactly the same focused set without maintaining a regular expression in contributor
# instructions or automation.
add_custom_target(editor_tests DEPENDS noteblockmodel_test noteeditor_test desktopnoteeditorhost_test editorqml_test)
set_tests_properties(noteblockmodel_test noteeditor_test PROPERTIES LABELS editor)
set_tests_properties(desktopnoteeditorhost_test editorqml_test PROPERTIES LABELS "editor;qml")
set_tests_properties(notesmanagerqml_test PROPERTIES LABELS "notesmanager;qml")
set_tests_properties(settingsqml_test PROPERTIES LABELS "settings;qml")

add_anykeep_test(ptfstorage_test ptfstorage_test.cpp)

add_anykeep_test(notesindex_test notesindex_test.cpp)

add_anykeep_test(filedraftstore_test filedraftstore_test.cpp)

add_anykeep_test(draftmanagertransfer_test draftmanagertransfer_test.cpp)

add_anykeep_test(foldercatalog_test foldercatalog_test.cpp)

add_anykeep_test(foldercatalogmanager_test foldercatalogmanager_test.cpp)

add_anykeep_test(noterule_test noterule_test.cpp)

add_anykeep_test(filenoterulestore_test filenoterulestore_test.cpp)

add_anykeep_test(noterulemanager_test noterulemanager_test.cpp)

add_anykeep_test(noteruleapplicationcontroller_test noteruleapplicationcontroller_test.cpp)

add_anykeep_test(rulescontroller_test rulescontroller_test.cpp)

add_anykeep_test(rulespage_test rulespage_test.cpp)
target_link_libraries(rulespage_test PRIVATE Qt6::QuickWidgets)

add_anykeep_test(foldernotesmodel_test foldernotesmodel_test.cpp)

add_anykeep_test(folderoperationscontroller_test folderoperationscontroller_test.cpp)

add_anykeep_test(tomboyfolderoverlay_test tomboyfolderoverlay_test.cpp ../plugins/tomboy/tomboynoteformat.cpp
                 ../plugins/tomboy/tomboystorage.cpp)
target_include_directories(tomboyfolderoverlay_test PRIVATE ../plugins/tomboy)
target_link_libraries(tomboyfolderoverlay_test PRIVATE Qt6::Xml)

add_anykeep_test(notesworkspacefolders_test notesworkspacefolders_test.cpp)

add_anykeep_test(fileremotecachestore_test fileremotecachestore_test.cpp)

add_anykeep_test(nextcloudcategory_test nextcloudcategory_test.cpp ../plugins/nextcloud/nextcloudcategory.cpp)
target_include_directories(nextcloudcategory_test PRIVATE ../plugins/nextcloud)

add_anykeep_test(nextcloudworker_test nextcloudworker_test.cpp ../plugins/nextcloud/nextcloudworker.cpp)
target_include_directories(nextcloudworker_test PRIVATE ../plugins/nextcloud)
target_link_libraries(nextcloudworker_test PRIVATE Qt6::Network)

add_anykeep_test(nextcloudstorage_test nextcloudstorage_test.cpp ../plugins/nextcloud/nextcloudcategory.cpp
                 ../plugins/nextcloud/nextcloudstorage.cpp ../plugins/nextcloud/nextcloudworker.cpp)
target_include_directories(nextcloudstorage_test PRIVATE ../plugins/nextcloud)
target_link_libraries(nextcloudstorage_test PRIVATE Qt6::Network)

add_anykeep_test(secureenvelope_test secureenvelope_test.cpp)

add_anykeep_test(localmediastore_test localmediastore_test.cpp)

add_anykeep_test(xmppnotecodec_test xmppnotecodec_test.cpp ../plugins/xmpppubsub/xmppnotecodec.cpp)
target_include_directories(xmppnotecodec_test PRIVATE ../plugins/xmpppubsub)
target_link_libraries(xmppnotecodec_test PRIVATE Qt6::Xml)

add_anykeep_test(xmpppayloadxml_test xmpppayloadxml_test.cpp ../plugins/xmpppubsub/xmpppayloadxml.cpp)
target_include_directories(xmpppayloadxml_test PRIVATE ../plugins/xmpppubsub)
target_link_libraries(xmpppayloadxml_test PRIVATE Qt6::Xml)

add_executable(xmppkeyresolutioncontroller_test xmppkeyresolutioncontroller_test.cpp
                                                ../plugins/xmpppubsub/xmppkeyresolutioncontroller.cpp)
target_include_directories(xmppkeyresolutioncontroller_test PRIVATE ../plugins/xmpppubsub)
target_link_libraries(xmppkeyresolutioncontroller_test PRIVATE Qt6::Test)
register_anykeep_test(xmppkeyresolutioncontroller_test)

if(TARGET QXmpp::QXmpp)
  add_executable(
    privatenotespubsubitem_test
    privatenotespubsubitem_test.cpp
    ../plugins/xmpppubsub/privatenotespubsubitem.cpp
    ../plugins/xmpppubsub/xmpppayloadxml.cpp
    ../plugins/xmpppubsub/xmpppepextension.cpp)
  target_include_directories(privatenotespubsubitem_test PRIVATE ../plugins/xmpppubsub)
  target_link_libraries(privatenotespubsubitem_test PRIVATE anykeep Qt6::Test Qt6::Xml QXmpp::QXmpp)
  anykeep_configure_qxmpp_target(privatenotespubsubitem_test)
  register_anykeep_test(privatenotespubsubitem_test)

  add_executable(xmppkeysyncextension_test xmppkeysyncextension_test.cpp ../plugins/xmpppubsub/xmppkeysyncextension.cpp)
  target_include_directories(xmppkeysyncextension_test PRIVATE ../plugins/xmpppubsub)
  target_link_libraries(xmppkeysyncextension_test PRIVATE Qt6::Test QXmpp::QXmpp)
  anykeep_configure_qxmpp_target(xmppkeysyncextension_test)
  register_anykeep_test(xmppkeysyncextension_test)

  add_executable(xmppomemopubsubitems_test xmppomemopubsubitems_test.cpp ../plugins/xmpppubsub/xmppomemopubsubitems.cpp)
  target_include_directories(xmppomemopubsubitems_test PRIVATE ../plugins/xmpppubsub)
  target_link_libraries(xmppomemopubsubitems_test PRIVATE Qt6::Test QXmpp::QXmpp)
  anykeep_configure_qxmpp_target(xmppomemopubsubitems_test)
  register_anykeep_test(xmppomemopubsubitems_test)

  add_executable(xmpperror_test xmpperror_test.cpp ../plugins/xmpppubsub/xmpperror.cpp)
  target_include_directories(xmpperror_test PRIVATE ../plugins/xmpppubsub)
  target_link_libraries(xmpperror_test PRIVATE Qt6::Test Qt6::Network QXmpp::QXmpp)
  anykeep_configure_qxmpp_target(xmpperror_test)
  register_anykeep_test(xmpperror_test)

  add_executable(xmpppersistenttruststorage_test xmpppersistenttruststorage_test.cpp
                                                 ../plugins/xmpppubsub/xmpppersistenttruststorage.cpp)
  target_include_directories(xmpppersistenttruststorage_test PRIVATE ../plugins/xmpppubsub)
  target_link_libraries(xmpppersistenttruststorage_test PRIVATE anykeep Qt6::Test QXmpp::QXmpp)
  anykeep_configure_qxmpp_target(xmpppersistenttruststorage_test)
  register_anykeep_test(xmpppersistenttruststorage_test)

  if(TARGET QXmpp::Omemo)
    add_executable(xmppomemostorage_test xmppomemostorage_test.cpp ../plugins/xmpppubsub/xmppomemostorage.cpp)
    target_include_directories(xmppomemostorage_test PRIVATE ../plugins/xmpppubsub)
    target_link_libraries(xmppomemostorage_test PRIVATE anykeep Qt6::Test QXmpp::Omemo)
    anykeep_configure_qxmpp_target(xmppomemostorage_test)
    register_anykeep_test(xmppomemostorage_test)
  endif()
endif()

add_anykeep_test(pluginmetadata_test pluginmetadata_test.cpp)

if(TARGET Qt6::Multimedia)
  add_executable(yandexspeechutils_test yandexspeechutils_test.cpp ../plugins/yandex/yandexspeechutils.cpp)
  target_include_directories(yandexspeechutils_test PRIVATE ../plugins/yandex)
  target_link_libraries(yandexspeechutils_test PRIVATE Qt6::Test Qt6::Multimedia)
  register_anykeep_test(yandexspeechutils_test)
endif()

set_tests_properties(notesmanagerqml_test PROPERTIES LABELS "folders;notesmanager;qml")
set_tests_properties(filedraftstore_test draftmanagertransfer_test PROPERTIES LABELS lifecycle)
set_tests_properties(
  foldercatalog_test
  foldercatalogmanager_test
  noterule_test
  filenoterulestore_test
  noterulemanager_test
  noteruleapplicationcontroller_test
  rulescontroller_test
  rulespage_test
  foldernotesmodel_test
  folderoperationscontroller_test
  tomboyfolderoverlay_test
  notesworkspacefolders_test
  PROPERTIES LABELS folders)
set_tests_properties(ptfstorage_test notesindex_test fileremotecachestore_test secureenvelope_test localmediastore_test
                     PROPERTIES LABELS storage)
set_tests_properties(nextcloudcategory_test nextcloudworker_test nextcloudstorage_test PROPERTIES LABELS
                                                                                                  "nextcloud;storage")
set_tests_properties(xmppnotecodec_test xmpppayloadxml_test PROPERTIES LABELS "storage;xmpp")
foreach(
  xmpp_test_target
  privatenotespubsubitem_test
  xmppkeyresolutioncontroller_test
  xmppkeysyncextension_test
  xmppomemopubsubitems_test
  xmpperror_test
  xmpppersistenttruststorage_test
  xmppomemostorage_test)
  if(TARGET ${xmpp_test_target})
    set_tests_properties(${xmpp_test_target} PROPERTIES LABELS "storage;xmpp")
  endif()
endforeach()

# Qt Creator commonly launches the selected test executable directly and does not apply CTest's ENVIRONMENT property.
# Link an early initializer into every test executable so QTEST_MAIN chooses the offscreen platform before creating
# QApplication. Explicit environment values still override these defaults.
get_property(
  anykeep_test_targets
  DIRECTORY
  PROPERTY BUILDSYSTEM_TARGETS)
set(anykeep_test_executable_targets)
foreach(anykeep_test_target IN LISTS anykeep_test_targets)
  get_target_property(anykeep_test_target_type ${anykeep_test_target} TYPE)
  if(anykeep_test_target_type STREQUAL "EXECUTABLE")
    target_sources(${anykeep_test_target} PRIVATE $<TARGET_OBJECTS:anykeep_headless_test_init>)
    get_target_property(anykeep_test_link_libraries ${anykeep_test_target} LINK_LIBRARIES)
    if(anykeep IN_LIST anykeep_test_link_libraries)
      target_sources(${anykeep_test_target} PRIVATE $<TARGET_OBJECTS:anykeep_qca_test_init>)
    endif()
    list(APPEND anykeep_test_executable_targets ${anykeep_test_target})
  endif()
endforeach()

# Windows resolves DLL dependencies from the executable directory before PATH. Keep test executables beside anykeep.dll
# so CTest, Qt Creator, and direct runs all use the same runtime tree.
if(WIN32)
  set_target_properties(${anykeep_test_executable_targets} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                                      "${EXECUTABLE_OUTPUT_PATH}")
endif()

add_custom_target(anykeep_all_tests DEPENDS ${anykeep_test_executable_targets})

# Keep every test independent of the developer's active display server.  This also covers new QTEST_MAIN-based tests
# automatically instead of relying on each target to remember its own headless environment.
get_property(
  anykeep_tests
  DIRECTORY
  PROPERTY TESTS)
set_tests_properties(
  ${anykeep_tests}
  PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software;QT_LOGGING_RULES=anykeep.*.info=false")
