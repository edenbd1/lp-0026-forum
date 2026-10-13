cmake_minimum_required(VERSION 3.20)
project(forum_core CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
find_package(nlohmann_json REQUIRED)

add_library(forum_core STATIC src/post.cpp src/identity.cpp src/store.cpp src/engine.cpp src/dns.cpp src/rln.cpp
    src/media.cpp src/reaction.cpp src/search.cpp)
target_include_directories(forum_core PUBLIC include)
target_link_libraries(forum_core PUBLIC PkgConfig::SODIUM PkgConfig::SQLITE nlohmann_json::nlohmann_json)
# inet_pton lives in Winsock on Windows.
if(WIN32)
    target_link_libraries(forum_core PUBLIC ws2_32)
endif()

option(FORUM_CORE_TESTS "Build the core's tests" ON)
if(FORUM_CORE_TESTS)
    enable_testing()
    # The core as released in 0.3.6 (tests/compat/v036: the release's files,
    # unedited), under its own namespace, so the tests can show what an
    # installed 0.3.x peer does with what this version sends.
    add_library(forum_core_v036 STATIC
        tests/compat_v036.cpp
        tests/compat/v036/src/post.cpp tests/compat/v036/src/identity.cpp
        tests/compat/v036/src/store.cpp tests/compat/v036/src/engine.cpp)
    target_include_directories(forum_core_v036 PRIVATE tests/compat/v036/include)
    target_compile_definitions(forum_core_v036 PRIVATE forum=forum_v036)
    target_link_libraries(forum_core_v036 PRIVATE PkgConfig::SODIUM PkgConfig::SQLITE nlohmann_json::nlohmann_json)
    if(WIN32)
        target_link_libraries(forum_core_v036 PRIVATE ws2_32)
    endif()
    add_executable(forum_core_tests tests/tests.cpp)
    target_link_libraries(forum_core_tests PRIVATE forum_core forum_core_v036)
    add_test(NAME forum_core_tests COMMAND forum_core_tests)
endif()
