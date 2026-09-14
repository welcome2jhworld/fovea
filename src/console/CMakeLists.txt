# Qt 6.9.0's FindWrapOpenGL links AGL.framework, which the macOS 26 SDK no
# longer ships; drop it from the imported target before any Gui link.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
  get_target_property(_fovea_gl WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
  if(_fovea_gl)
    list(FILTER _fovea_gl EXCLUDE REGEX "AGL")
    set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_fovea_gl}")
  endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Multimedia)

# Everything but main.cpp, so tests can drive the console's widgets and service client in process.
qt_add_library(fovea-console STATIC
  src/theme/Tokens.h
  src/theme/Theme.h
  src/theme/Theme.cpp
  src/theme/Icons.h
  src/theme/Icons.cpp
  src/theme/Tone.h
  src/core/CoreClient.h
  src/core/CoreClient.cpp
  src/core/ProcessProbe.h
  src/core/ProcessProbe.cpp
  src/core/CoreLauncher.h
  src/core/CoreLauncher.cpp
  src/core/StatusPoller.h
  src/core/StatusPoller.cpp
  src/core/AlertTypes.h
  src/core/AlertTypes.cpp
  src/core/EventStore.h
  src/core/EventStore.cpp
  src/core/AlertDispatcher.h
  src/core/AlertDispatcher.cpp
  src/core/ThumbnailCache.h
  src/core/ThumbnailCache.cpp
  src/core/SearchTypes.h
  src/core/SearchTypes.cpp
  src/search/SearchLogic.h
  src/search/SearchLogic.cpp
  src/alerts/AlertLogic.h
  src/alerts/AlertLogic.cpp
  src/video/WallTicker.h
  src/video/WallTicker.cpp
  src/video/FrameSurface.h
  src/video/FrameSurface.cpp
  src/video/PlaybackControls.h
  src/video/PlaybackControls.cpp
  src/screens/MainWindow.h
  src/screens/MainWindow.cpp
  src/screens/TitleBar.h
  src/screens/TitleBar.cpp
  src/screens/MainTabBar.h
  src/screens/MainTabBar.cpp
  src/screens/NotImplementedState.h
  src/screens/NotImplementedState.cpp
  src/screens/monitor/MonitorScreen.h
  src/screens/monitor/MonitorScreen.cpp
  src/screens/monitor/ServiceStatusLine.h
  src/screens/monitor/ServiceStatusLine.cpp
  src/screens/monitor/CameraTreeModel.h
  src/screens/monitor/CameraTreeModel.cpp
  src/screens/monitor/CameraTreeDelegate.h
  src/screens/monitor/CameraTreeDelegate.cpp
  src/screens/monitor/CameraRail.h
  src/screens/monitor/CameraRail.cpp
  src/screens/monitor/WallLayout.h
  src/screens/monitor/LayoutPopup.h
  src/screens/monitor/LayoutPopup.cpp
  src/screens/monitor/WallToolbar.h
  src/screens/monitor/WallToolbar.cpp
  src/screens/monitor/VideoTile.h
  src/screens/monitor/VideoTile.cpp
  src/screens/monitor/VideoWall.h
  src/screens/monitor/VideoWall.cpp
  src/screens/monitor/RecordingsModel.h
  src/screens/monitor/RecordingsModel.cpp
  src/screens/monitor/RecordingsPanel.h
  src/screens/monitor/RecordingsPanel.cpp
  src/screens/monitor/DetectionPoller.h
  src/screens/monitor/DetectionPoller.cpp
  src/screens/monitor/LiveEventFeed.h
  src/screens/monitor/LiveEventFeed.cpp
  src/screens/alerts/AlertsScreen.h
  src/screens/alerts/AlertsScreen.cpp
  src/screens/alerts/RulesRail.h
  src/screens/alerts/RulesRail.cpp
  src/screens/alerts/RuleEditor.h
  src/screens/alerts/RuleEditor.cpp
  src/screens/alerts/ZoneCanvas.h
  src/screens/alerts/ZoneCanvas.cpp
  src/screens/alerts/AlertTable.h
  src/screens/alerts/AlertTable.cpp
  src/screens/alerts/AlertDetailPanel.h
  src/screens/alerts/AlertDetailPanel.cpp
  src/screens/alerts/EvidencePlayer.h
  src/screens/alerts/EvidencePlayer.cpp
  src/screens/search/SearchScreen.h
  src/screens/search/SearchScreen.cpp
  src/screens/search/ResultGrid.h
  src/screens/search/ResultGrid.cpp
  src/screens/search/SearchInspector.h
  src/screens/search/SearchInspector.cpp
  src/screens/search/CustomRangePopup.h
  src/screens/search/CustomRangePopup.cpp
  src/util/Format.h
  src/util/Format.cpp
  src/widgets/ToggleSwitch.h
  src/widgets/ToggleSwitch.cpp
  src/widgets/StatusChip.h
  src/widgets/StatusChip.cpp
  src/widgets/Painting.h
  src/widgets/Painting.cpp
  src/widgets/ToneChip.h
  src/widgets/ToneChip.cpp
  src/widgets/FormField.h
  src/widgets/FormField.cpp
  src/widgets/SelectButton.h
  src/widgets/SelectButton.cpp
  src/dialogs/DialogFrame.h
  src/dialogs/DialogFrame.cpp
  src/dialogs/DialogHost.h
  src/dialogs/DialogHost.cpp
  src/dialogs/FfmpegCommand.h
  src/dialogs/FfmpegCommand.cpp
  src/dialogs/ConnectionPage.h
  src/dialogs/ConnectionPage.cpp
  src/dialogs/RecordingPage.h
  src/dialogs/RecordingPage.cpp
  src/dialogs/CameraSettingsDialog.h
  src/dialogs/CameraSettingsDialog.cpp
  src/dialogs/PlaybackDialog.h
  src/dialogs/PlaybackDialog.cpp
)

qt_add_resources(fovea-console "fovea_fonts"
  PREFIX "/fonts"
  BASE resources/fonts
  FILES
    resources/fonts/IBMPlexSans-Regular.ttf
    resources/fonts/IBMPlexSans-Medium.ttf
    resources/fonts/IBMPlexSans-SemiBold.ttf
    resources/fonts/IBMPlexMono-Regular.ttf
    resources/fonts/LICENSE.txt
)

qt_add_resources(fovea-console "fovea_sounds"
  PREFIX "/sounds"
  BASE resources/sounds
  FILES
    resources/sounds/alert.wav
)

qt_add_resources(fovea-console "fovea_icons"
  PREFIX "/icons"
  BASE resources/icons
  FILES
    resources/icons/caret-down.svg
    resources/icons/caret-right.svg
    resources/icons/close.svg
    resources/icons/combo-caret.svg
    resources/icons/gear.svg
    resources/icons/maximize.svg
    resources/icons/square.svg
)

target_include_directories(fovea-console PUBLIC src)
target_link_libraries(fovea-console PUBLIC fovea-common Qt6::Widgets Qt6::Network Qt6::Svg Qt6::Multimedia)
target_compile_definitions(fovea-console PRIVATE
  FOVEA_DEV_CORE_BIN="${CMAKE_BINARY_DIR}/src/core/fovea-core${CMAKE_EXECUTABLE_SUFFIX}")
fovea_target_warnings(fovea-console)

qt_add_executable(fovea src/main.cpp)
target_link_libraries(fovea PRIVATE fovea-console)
target_compile_definitions(fovea PRIVATE FOVEA_VERSION="${PROJECT_VERSION}")
set_target_properties(fovea PROPERTIES MACOSX_BUNDLE OFF WIN32_EXECUTABLE ON)
fovea_target_warnings(fovea)

if(FOVEA_BUILD_TESTS)
  add_subdirectory(tests)
endif()
