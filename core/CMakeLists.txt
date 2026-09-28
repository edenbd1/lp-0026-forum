cmake_minimum_required(VERSION 3.20)
project(forum_core CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
find_package(nlohmann_json REQUIRED)

add_library(forum_core STATIC src/post.cpp src/identity.cpp)
target_include_directories(forum_core PUBLIC include)
target_link_libraries(forum_core PUBLIC PkgConfig::SODIUM PkgConfig::SQLITE nlohmann_json::nlohmann_json)

enable_testing()
add_executable(forum_core_tests tests/tests.cpp)
target_link_libraries(forum_core_tests PRIVATE forum_core)
add_test(NAME forum_core_tests COMMAND forum_core_tests)
