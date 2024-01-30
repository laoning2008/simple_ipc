project(simple_ipc CXX)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

add_library(simple_ipc INTERFACE)

file(GLOB_RECURSE SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR} *.hpp)
target_sources(simple_ipc PRIVATE ${SOURCE_FILES})
target_include_directories(simple_ipc INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

foreach(source IN LISTS SOURCE_FILES)
    get_filename_component(source_path "${source}" PATH)
    string(REPLACE ${CMAKE_CURRENT_SOURCE_DIR} "" relatie_source_path ${source_path})
    source_group("${relatie_source_path}" FILES "${source}")
endforeach()


