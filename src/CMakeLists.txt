project(simple_ipc CXX)
     
set(CMAKE_INCLUDE_CURRENT_DIR ON)

add_library(simple_ipc INTERFACE)
target_include_directories(simple_ipc INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})             

