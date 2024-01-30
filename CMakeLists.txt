cmake_minimum_required(VERSION 3.25)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

project(simple_ipc CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake)

add_subdirectory(src)
add_subdirectory(examples)