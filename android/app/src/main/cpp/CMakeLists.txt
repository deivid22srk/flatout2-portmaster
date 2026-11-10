cmake_minimum_required(VERSION 3.22)
project(fo2rt C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

add_library(fo2rt SHARED
    core/shim_common.cpp
    core/elf_loader.cpp
    core/guest_thread.cpp
    core/entry_jump.S
    shim/auto_thunks.S         # generated: generic guest->bionic forwarding thunks
    shim/glibc_shim.cpp
    shim/glibc_shim_ctype.cpp
    shim/glibc_shim_manual.cpp
    shim/sdl2_shim.cpp
    shim/sdl_video_egl.cpp
    shim/sdl_audio_opensl.cpp
    bridge/fo2_bridge.cpp
    shim/auto_imports.cpp      # generated: import table + generic bionic thunks (scripts inside cpp/gen_imports.py)
)

target_include_directories(fo2rt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    # SDL2 headers exactly as the port was built against (layouts/enum values are ABI).
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../runtime/sdk/include
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv2)
find_library(opensles-lib OpenSLES)

target_link_libraries(fo2rt
    ${log-lib}
    ${android-lib}
    ${egl-lib}
    ${gles-lib}
    ${opensles-lib}
)

# Shims must not read the compiler's own TLS (guest threads run with a guest TCB in TPIDR_EL0),
# and must not be instrumented in ways that assume bionic TLS.
target_compile_options(fo2rt PRIVATE
    -fvisibility=hidden
    -fno-stack-protector
    -fno-omit-frame-pointer
    -O2
    -Wall
    -Wno-unused-parameter
)
