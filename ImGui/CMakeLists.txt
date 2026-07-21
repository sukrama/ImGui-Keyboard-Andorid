cmake_minimum_required(VERSION 3.22.1)

project("imgui")

add_library(imgui STATIC
        imgui.cpp
        imgui_draw.cpp
        imgui_tables.cpp
        imgui_widgets.cpp
        backends/imgui_impl_android.cpp
        backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/backends
        ${CMAKE_CURRENT_SOURCE_DIR}/fonts
)

target_compile_options(imgui PRIVATE
        -Os
        -ffunction-sections
        -fdata-sections
        -fvisibility=hidden
        -fno-rtti
        -flto
        -w
        -Wno-everything
        -DNDEBUG
        -DIMGUI_DISABLE_DEMO_WINDOWS
        -DIMGUI_DISABLE_DEBUG_TOOLS
)
