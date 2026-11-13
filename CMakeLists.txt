cmake_minimum_required(VERSION 3.16)
project(gopro-cli VERSION 0.4.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.9 REQUIRED)

# The camera logic lives in a library so a GUI can link against it later.
add_library(gopro STATIC src/http.cpp src/camera.cpp)
target_include_directories(gopro PUBLIC src)
target_link_libraries(gopro PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
target_compile_options(gopro PRIVATE -Wall -Wextra -Wpedantic)

add_executable(gopro-cli src/main.cpp)
target_link_libraries(gopro-cli PRIVATE gopro)
target_compile_options(gopro-cli PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(gopro-cli PROPERTIES OUTPUT_NAME gopro)

install(TARGETS gopro-cli RUNTIME DESTINATION bin)
