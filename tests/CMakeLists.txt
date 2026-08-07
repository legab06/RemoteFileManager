qt_add_executable(rfm_core_tests test_connection_profile.cpp)
target_link_libraries(rfm_core_tests PRIVATE Qt6::Test RemoteFileManager::Core)
rfm_set_project_warnings(rfm_core_tests)
add_test(NAME unit.connection_profile COMMAND rfm_core_tests)

qt_add_executable(rfm_ui_tests test_main_window.cpp)
target_link_libraries(rfm_ui_tests PRIVATE Qt6::Test RemoteFileManager::Ui)
rfm_set_project_warnings(rfm_ui_tests)
add_test(NAME ui.main_window COMMAND rfm_ui_tests)
set_tests_properties(ui.main_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

