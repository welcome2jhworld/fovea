qt_add_library(fovea-rules STATIC
  include/fovea/rules/Types.h
  include/fovea/rules/RuleEvaluator.h
  src/Types.cpp
  src/RuleEvaluator.cpp
)
target_include_directories(fovea-rules PUBLIC include)
target_link_libraries(fovea-rules PUBLIC fovea-common Qt6::Core)
fovea_target_warnings(fovea-rules)

if(FOVEA_BUILD_TESTS)
  add_subdirectory(tests)
endif()
