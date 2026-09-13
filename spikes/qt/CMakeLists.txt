cmake_minimum_required(VERSION 3.22)
project(fovea_qt_spikes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.9 REQUIRED COMPONENTS Core Network Sql HttpServer Widgets Test)
qt_standard_project_setup()

if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_fovea_gl WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    list(FILTER _fovea_gl EXCLUDE REGEX "AGL")
    set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_fovea_gl}")
endif()

qt_add_executable(shm_ring shm_ring.cpp)
target_link_libraries(shm_ring PRIVATE Qt6::Core)

qt_add_executable(http_api http_api.cpp)
target_link_libraries(http_api PRIVATE Qt6::Core Qt6::Network Qt6::HttpServer)

qt_add_executable(sql_check sql_check.cpp)
target_link_libraries(sql_check PRIVATE Qt6::Core Qt6::Sql)

qt_add_executable(proc_lock proc_lock.cpp)
target_link_libraries(proc_lock PRIVATE Qt6::Core)

qt_add_executable(tst_smoke tst_smoke.cpp)
target_link_libraries(tst_smoke PRIVATE Qt6::Core Qt6::Widgets Qt6::Test)
