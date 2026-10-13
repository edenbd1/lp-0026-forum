# The image pipeline's own tests (src/image_pipeline.cpp), which need Qt Gui
# and its JPEG, PNG and (if installed) WebP plugins.
#
#   cmake -S tests/media -B build-media -DCMAKE_PREFIX_PATH=<Qt 6 prefix> && cmake --build build-media -j4
#   build-media/media_tests [out-dir]   # out-dir: keep the before/after files for exiftool
cmake_minimum_required(VERSION 3.20)
project(forum_media_tests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui)
add_executable(media_tests media_tests.cpp ../../src/image_pipeline.cpp)
target_include_directories(media_tests PRIVATE ../../src ../../core/include)
target_link_libraries(media_tests PRIVATE Qt6::Core Qt6::Gui)
