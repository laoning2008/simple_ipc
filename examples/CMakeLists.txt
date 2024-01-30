cmake_minimum_required(VERSION 3.25)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

project(examples CXX)
     
add_executable(client ${CMAKE_CURRENT_SOURCE_DIR}/client.cpp)
target_link_libraries(client PRIVATE simple_ipc)
set_target_properties(client PROPERTIES FOLDER "examples")

add_executable(server ${CMAKE_CURRENT_SOURCE_DIR}/server.cpp)
target_link_libraries(server PRIVATE simple_ipc)
set_target_properties(server PROPERTIES FOLDER "examples")

