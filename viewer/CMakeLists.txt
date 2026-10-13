cmake_minimum_required(VERSION 3.16)
project(pixview LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(ZLIB REQUIRED)

set(PIX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# PIX codec is C++14: in C++17 its 'byte' typedef clashes with std::byte ('using namespace std')
add_library(pixcodec STATIC ${PIX_SRC}/pix_codec.cpp ${PIX_SRC}/pix_error.cpp ${PIX_SRC}/pix_api.cpp
            ${PIX_SRC}/pix_report.cpp ${PIX_SRC}/pix2_codec.cpp)
set_target_properties(pixcodec PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(pixcodec PRIVATE -Wall -Wextra)
target_link_libraries(pixcodec PRIVATE ZLIB::ZLIB)

# viewer is C++17 (required by Qt 6), it only includes pix_api.h
set(CMAKE_AUTOMOC ON)
add_executable(pixview main.cpp main_window.cpp main_window.h image_view.cpp image_view.h)
set_target_properties(pixview PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(pixview PRIVATE -Wall -Wextra)
target_include_directories(pixview PRIVATE ${PIX_SRC})
target_link_libraries(pixview PRIVATE Qt6::Widgets pixcodec)
