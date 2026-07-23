# AsmJit static library for ImGui-Keyboard-Android
# Include path: parent of this file (Includes/) so that #include <asmjit/core.h> works

cmake_minimum_required(VERSION 3.22.1)

# Collect all .cpp source files under asmjit/
file(GLOB_RECURSE ASMJIT_SRC "asmjit/*.cpp")

add_library(asmjit STATIC ${ASMJIT_SRC})

target_include_directories(asmjit PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_definitions(asmjit PUBLIC
    ASMJIT_STATIC
    ASMJIT_NO_FOREIGN=1
)

target_compile_options(asmjit PRIVATE
    -O3 -fvisibility=hidden -ffunction-sections -fdata-sections
)

if(ANDROID)
    target_compile_definitions(asmjit PUBLIC ASMJIT_NO_SHM_OPEN=1)
endif()
