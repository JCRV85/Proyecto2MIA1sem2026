cmake_minimum_required(VERSION 3.20)
project(mia_api LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(mia-api
  src/main.cpp
  src/engine_state.cpp
  src/engine_disk.cpp
  src/engine_storage.cpp
  src/engine_auth.cpp
  src/engine_fs.cpp
  src/engine_reports.cpp
  src/engine_utils.cpp
)

target_include_directories(mia-api PRIVATE
  include
  vendor
)

if (WIN32)
  target_link_libraries(mia-api PRIVATE ws2_32)
endif()
