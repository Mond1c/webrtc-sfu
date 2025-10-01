cmake_minimum_required(VERSION 3.20)
project(sfu_min LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(LibDataChannel CONFIG REQUIRED)

add_executable(sfu src/main.cpp)
target_link_libraries(sfu PRIVATE LibDataChannel::LibDataChannel)
