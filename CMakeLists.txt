cmake_minimum_required(VERSION 3.16)
project(gopro-cli VERSION 0.6.0 LANGUAGES CXX
        DESCRIPTION "Control a GoPro HERO7 Black over Wi-Fi: CLI and GUI")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(GOPRO_BUILD_GUI "Build the graphical application (gopro-gui)" ON)
option(GOPRO_BUILD_TESTS "Register the smoke test with CTest" ON)

include(GNUInstallDirs)

# Warning flags and source encoding for our own targets (not for third-party code).
function(gopro_target_defaults target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.9 REQUIRED)
find_package(Threads REQUIRED)

# --- libgopro: all camera logic, shared by the CLI and the GUI ---------------
add_library(gopro STATIC src/http.cpp src/camera.cpp src/platform.cpp)
target_include_directories(gopro PUBLIC src)
target_link_libraries(gopro PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
if(WIN32)
  target_link_libraries(gopro PUBLIC ws2_32)
endif()
gopro_target_defaults(gopro)

# --- gopro: command-line tool -------------------------------------------------
add_executable(gopro-cli src/main.cpp)
target_link_libraries(gopro-cli PRIVATE gopro Threads::Threads)
target_compile_definitions(gopro-cli PRIVATE GOPRO_VERSION="${PROJECT_VERSION}")
set_target_properties(gopro-cli PROPERTIES OUTPUT_NAME gopro)
gopro_target_defaults(gopro-cli)
install(TARGETS gopro-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- gopro-gui: window with live preview, controls, settings, media ----------
if(GOPRO_BUILD_GUI)
  find_package(SDL2 REQUIRED)
  find_package(OpenGL REQUIRED)

  # FFmpeg: pkg-config on Linux and macOS (Homebrew), vcpkg's module on Windows.
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG_PC IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
  endif()
  if(NOT FFMPEG_PC_FOUND)
    find_package(FFMPEG REQUIRED)
  endif()

  # Dear ImGui is vendored (MIT licence) so the build needs no internet.
  set(IMGUI_DIR third_party/imgui)
  add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
  if(TARGET SDL2::SDL2)
    target_link_libraries(imgui PUBLIC SDL2::SDL2)
  else()  # older SDL2 packages only export variables
    target_include_directories(imgui PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(imgui PUBLIC ${SDL2_LIBRARIES})
  endif()
  target_link_libraries(imgui PUBLIC OpenGL::GL ${CMAKE_DL_LIBS})
  if(APPLE)
    # Apple marks all of OpenGL as deprecated; it still works and we know.
    target_compile_definitions(imgui PUBLIC GL_SILENCE_DEPRECATION)
  endif()

  add_executable(gopro-gui
    src/gui/main_gui.cpp src/gui/worker.cpp src/gui/video_stream.cpp)
  target_link_libraries(gopro-gui PRIVATE gopro imgui Threads::Threads)
  if(TARGET SDL2::SDL2main)
    target_link_libraries(gopro-gui PRIVATE SDL2::SDL2main)  # WinMain shim on Windows
  endif()
  if(FFMPEG_PC_FOUND)
    target_link_libraries(gopro-gui PRIVATE PkgConfig::FFMPEG_PC)
  else()
    target_include_directories(gopro-gui PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(gopro-gui PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(gopro-gui PRIVATE ${FFMPEG_LIBRARIES})
  endif()
  gopro_target_defaults(gopro-gui)
  install(TARGETS gopro-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# --- tests: the CLI against the camera emulator ------------------------------
if(GOPRO_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    enable_testing()
    add_test(NAME smoke
             COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/smoke_test.py
                     $<TARGET_FILE:gopro-cli>)
  endif()
endif()
