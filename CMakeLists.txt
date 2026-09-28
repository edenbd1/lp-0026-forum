cmake_minimum_required(VERSION 3.20)
project(LogosForumPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)
string(JSON MODULE_VERSION GET ${METADATA_JSON} version)

# The forum core: every rule the forum follows, independent of Basecamp and
# tested on its own (core/tests). Built position-independent to link into the
# plugin.
set(FORUM_CORE_TESTS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(core)

logos_module(
    NAME ${MODULE_NAME}
    REP_FILE src/logos_forum.rep
    SOURCES
        src/logos_forum_backend.h
        src/logos_forum_backend.cpp
        src/delivery_transport.h
        src/delivery_transport.cpp
    INCLUDE_DIRS
        src
    LINK_LIBRARIES
        forum_core
)

target_compile_definitions(${MODULE_NAME}_module_plugin PRIVATE
    LOGOS_FORUM_VERSION="${MODULE_VERSION}")
