cmake_minimum_required(VERSION 3.20)
project(sfu LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

find_package(LibDataChannel CONFIG REQUIRED)

set(SOURCE_FILES
    src/main.cpp
    src/utils.cpp
    src/track_broadcaster.cpp
    src/publisher.cpp
    src/subscriber.cpp
    src/sfu.cpp
)

add_executable(sfu ${SOURCE_FILES})
target_link_libraries(sfu PRIVATE LibDataChannel::LibDataChannel)

target_include_directories(
    sfu
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
