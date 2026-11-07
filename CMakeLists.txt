cmake_minimum_required(VERSION 3.20)
project(3drudder-hid-transmitter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT so the exe runs without the VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9b
  GIT_SHALLOW TRUE
)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui json)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

# Everything except the UI, so the test tool can link it too.
add_library(rht_core STATIC
  src/gamepad.cpp
  src/mapping.cpp
  src/rudder.cpp
  src/sender.cpp
  src/config.cpp
)
target_compile_definitions(rht_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(rht_core PUBLIC /W3 /utf-8)
target_link_libraries(rht_core PUBLIC
  nlohmann_json::nlohmann_json
  ws2_32 ole32 hid setupapi
)

add_executable(3drudder-hid-transmitter WIN32 src/main.cpp)
target_link_libraries(3drudder-hid-transmitter PRIVATE rht_core imgui_lib d3d11 dxgi d3dcompiler shell32)

# Command line tool for the protocol decoding and the mapping.
add_executable(rht-test src/test_main.cpp)
target_link_libraries(rht-test PRIVATE rht_core shell32)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT 3drudder-hid-transmitter)
