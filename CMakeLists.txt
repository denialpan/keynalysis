cmake_minimum_required(VERSION 3.20)

project(keynalysis LANGUAGES CXX)

include(FetchContent)

FetchContent_Declare(
    imgui_docking
    URL https://github.com/ocornut/imgui/archive/refs/heads/docking.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui_docking)

set(IMGUI_SOURCES
    ${imgui_docking_SOURCE_DIR}/imgui.cpp
    ${imgui_docking_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_docking_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_docking_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_docking_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_docking_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

add_executable(keynalysis WIN32
    src/main.cpp
    src/app.rc
    ${IMGUI_SOURCES}
)

target_compile_features(keynalysis PRIVATE cxx_std_17)
target_compile_definitions(keynalysis PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_include_directories(keynalysis PRIVATE
    ${imgui_docking_SOURCE_DIR}
    ${imgui_docking_SOURCE_DIR}/backends
)
target_link_libraries(keynalysis PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dwmapi
    shell32
    comdlg32
)

if(MINGW)
    target_link_options(keynalysis PRIVATE -municode)
endif()
