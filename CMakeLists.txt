cmake_minimum_required(VERSION 3.16)
project(gopro-cli VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(GOPRO_BUILD_GUI "Build the graphical application (gopro-gui)" ON)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.9 REQUIRED)
find_package(Threads REQUIRED)

# --- libgopro: all camera logic, shared by the CLI and the GUI ---------------
add_library(gopro STATIC src/http.cpp src/camera.cpp)
target_include_directories(gopro PUBLIC src)
target_link_libraries(gopro PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
target_compile_options(gopro PRIVATE -Wall -Wextra -Wpedantic)

# --- gopro: command-line tool -------------------------------------------------
add_executable(gopro-cli src/main.cpp)
target_link_libraries(gopro-cli PRIVATE gopro)
target_compile_options(gopro-cli PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(gopro-cli PROPERTIES OUTPUT_NAME gopro)
install(TARGETS gopro-cli RUNTIME DESTINATION bin)

# --- gopro-gui: window with live preview, controls, settings, media ----------
if(GOPRO_BUILD_GUI)
  find_package(SDL2 REQUIRED)
  find_package(OpenGL REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)

  # Dear ImGui is vendored (MIT licence) so the build needs no internet.
  set(IMGUI_DIR third_party/imgui)
  add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${SDL2_INCLUDE_DIRS})
  target_link_libraries(imgui PUBLIC ${SDL2_LIBRARIES} OpenGL::GL ${CMAKE_DL_LIBS})

  add_executable(gopro-gui
    src/gui/main_gui.cpp src/gui/worker.cpp src/gui/video_stream.cpp)
  target_link_libraries(gopro-gui PRIVATE gopro imgui PkgConfig::FFMPEG Threads::Threads)
  target_compile_options(gopro-gui PRIVATE -Wall -Wextra -Wpedantic)
  install(TARGETS gopro-gui RUNTIME DESTINATION bin)
endif()
