cmake_minimum_required(VERSION 3.20)

project(game_rooms_cpp_sdk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(game_rooms
  src/sdk.cpp
)

target_include_directories(game_rooms
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

enable_testing()

add_executable(game_rooms_tests
  tests/sdk_test.cpp
)

target_link_libraries(game_rooms_tests PRIVATE game_rooms)

add_test(NAME game_rooms_tests COMMAND game_rooms_tests)
