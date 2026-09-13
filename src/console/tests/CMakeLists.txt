# Console test fixtures. console-stub-core is a fake service for UI
# verification only; the console never launches it.
qt_add_executable(console-stub-core stub_core.cpp)
target_link_libraries(console-stub-core PRIVATE fovea-common Qt6::Gui Qt6::Network Qt6::HttpServer)
set_target_properties(console-stub-core PROPERTIES MACOSX_BUNDLE OFF)
fovea_target_warnings(console-stub-core)

qt_add_executable(test_ffmpeg_command test_ffmpeg_command.cpp ../src/dialogs/FfmpegCommand.cpp)
target_include_directories(test_ffmpeg_command PRIVATE ../src)
target_link_libraries(test_ffmpeg_command PRIVATE Qt6::Core Qt6::Test)
set_target_properties(test_ffmpeg_command PROPERTIES MACOSX_BUNDLE OFF)
fovea_target_warnings(test_ffmpeg_command)
add_test(NAME test_ffmpeg_command COMMAND test_ffmpeg_command)

qt_add_executable(console_smoke console_smoke.cpp)
target_link_libraries(console_smoke PRIVATE Qt6::Gui Qt6::Test)
target_compile_definitions(console_smoke PRIVATE
  FOVEA_CONSOLE_BIN="$<TARGET_FILE:fovea>"
  FOVEA_STUB_CORE_BIN="$<TARGET_FILE:console-stub-core>")
add_dependencies(console_smoke fovea console-stub-core)
set_target_properties(console_smoke PROPERTIES MACOSX_BUNDLE OFF)
fovea_target_warnings(console_smoke)
add_test(NAME console_smoke COMMAND console_smoke)
set_tests_properties(console_smoke PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP   :" TIMEOUT 180)
