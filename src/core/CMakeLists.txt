set(FOVEA_CORE_LIB_SOURCES
  include/fovea/core/Config.h
  include/fovea/core/Store.h
  include/fovea/core/SecretStore.h
  include/fovea/core/MediaProbe.h
  include/fovea/core/CameraManager.h
  include/fovea/core/PlaybackManager.h
  include/fovea/core/ApiServer.h
  include/fovea/core/CameraPipeline.h
  include/fovea/core/SessionClock.h
  src/Store.cpp
  src/SecretStore.cpp
  src/ApiServer.cpp
  src/MediaProbe.cpp
  src/SessionClock.cpp
  src/CameraPipeline.cpp
  src/CameraManager.cpp
  src/PlaybackManager.cpp
)

qt_add_library(fovea-core-lib STATIC ${FOVEA_CORE_LIB_SOURCES})
target_include_directories(fovea-core-lib PUBLIC include)
target_link_libraries(fovea-core-lib PUBLIC fovea-common Qt6::Core Qt6::Sql Qt6::Network Qt6::HttpServer PkgConfig::GST)
fovea_target_warnings(fovea-core-lib)

qt_add_executable(fovea-core src/main.cpp)
target_link_libraries(fovea-core PRIVATE fovea-core-lib)
fovea_target_warnings(fovea-core)
