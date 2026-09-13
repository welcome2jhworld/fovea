qt_add_library(fovea-common STATIC
  include/fovea/Ids.h
  include/fovea/Clock.h
  include/fovea/Paths.h
  include/fovea/Redact.h
  include/fovea/Backoff.h
  include/fovea/Token.h
  include/fovea/FrameRing.h
  include/fovea/Api.h
  src/Ids.cpp
  src/Clock.cpp
  src/Paths.cpp
  src/Redact.cpp
  src/Backoff.cpp
  src/Token.cpp
  src/FrameRing.cpp
  src/Api.cpp
)
target_include_directories(fovea-common PUBLIC include)
target_link_libraries(fovea-common PUBLIC Qt6::Core)
fovea_target_warnings(fovea-common)
