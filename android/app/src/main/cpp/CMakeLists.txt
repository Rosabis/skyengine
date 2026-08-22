cmake_minimum_required(VERSION 3.22.1)
project(vmrp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(VMRP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../..")
set(DEPS_DIR "${VMRP_ROOT}/android/third_party")

function(vmrp_require_dir dir label)
    if(NOT EXISTS "${dir}")
        message(FATAL_ERROR "${label} not found: ${dir}. Run Gradle task fetchNativeDeps or unpack SDL2/unicorn into android/third_party.")
    endif()
endfunction()

# --- Unicorn (guest ARM only, static) ---
set(_OLD_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(UNICORN_ARCH "arm" CACHE STRING "Unicorn architectures" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
if(EXISTS "${DEPS_DIR}/unicorn/CMakeLists.txt")
    add_subdirectory("${DEPS_DIR}/unicorn" unicorn EXCLUDE_FROM_ALL)
elseif(EXISTS "${DEPS_DIR}/unicorn-2.1.4/CMakeLists.txt")
    add_subdirectory("${DEPS_DIR}/unicorn-2.1.4" unicorn EXCLUDE_FROM_ALL)
else()
    message(FATAL_ERROR "Unicorn source not found under ${DEPS_DIR}. Expected unicorn/ or unicorn-2.1.4/")
endif()
if(DEFINED _OLD_BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS ${_OLD_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)
else()
    unset(BUILD_SHARED_LIBS CACHE)
endif()

# --- SDL2 (shared, JNI entry) ---
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "" FORCE)
if(EXISTS "${DEPS_DIR}/SDL2/CMakeLists.txt")
    add_subdirectory("${DEPS_DIR}/SDL2" SDL2 EXCLUDE_FROM_ALL)
elseif(EXISTS "${DEPS_DIR}/SDL2-2.30.11/CMakeLists.txt")
    add_subdirectory("${DEPS_DIR}/SDL2-2.30.11" SDL2 EXCLUDE_FROM_ALL)
else()
    message(FATAL_ERROR "SDL2 source not found under ${DEPS_DIR}. Expected SDL2/ or SDL2-2.30.11/")
endif()

# --- 与桌面 skyengine 目标保持一致的编译清单（重构后 src/ 布局）---
set(SKYENGINE_AEX_SOURCES
  src/arm_ext/aex_support.c
  src/arm_ext/aex_pack.c
  src/arm_ext/aex_mem.c
  src/arm_ext/aex_screen.c
  src/arm_ext/aex_guard.c
  src/arm_ext/aex_exec.c
  src/arm_ext/aex_table.c
  src/arm_ext/aex_diag.c
  src/arm_ext/aex_timer.c
  src/arm_ext/aex_module.c
)
set(SKYENGINE_MYTHROAD_SOURCES
  src/mythroad/mythroad.c
  src/mythroad/encode.c
  src/mythroad/mr_unzip.c
  src/mythroad/mr_base64.c
  src/mythroad/mr_inflate.c
  src/mythroad/mr_graphics.c
  src/mythroad/string.c
  src/mythroad/printf.c
  src/mythroad/other.c
  src/mythroad/strtol.c
  src/mythroad/strtoul.c
  src/mythroad/dsm.c
  src/mythroad/fixR9.c
  src/mythroad/md5.c
  src/mythroad/mem.c
  src/mythroad/mr_pluto.c
  src/mythroad/src/mr_api.c
  src/mythroad/src/mr_debug.c
  src/mythroad/src/mr_do.c
  src/mythroad/src/mr_dump.c
  src/mythroad/src/mr_func.c
  src/mythroad/src/mr_gc.c
  src/mythroad/src/mr_mem.c
  src/mythroad/src/mr_opcodes.c
  src/mythroad/src/mr_object.c
  src/mythroad/src/mr_state.c
  src/mythroad/src/mr_string.c
  src/mythroad/src/mr_table.c
  src/mythroad/src/mr_tm.c
  src/mythroad/src/mr_undump.c
  src/mythroad/src/mr_vm.c
  src/mythroad/src/mr_zio.c
  src/mythroad/src/parser/mr_parser.c
  src/mythroad/src/parser/mr_lex.c
  src/mythroad/src/parser/mr_code.c
  src/mythroad/src/lib/mr_auxiliar.c
  src/mythroad/src/lib/mr_auxlib.c
  src/mythroad/src/lib/mr_baselib.c
  src/mythroad/src/lib/mr_iolib_target.c
  src/mythroad/src/lib/mr_socket_target.c
  src/mythroad/src/lib/mr_strlib.c
  src/mythroad/src/lib/mr_tablib.c
  src/mythroad/src/lib/mr_tcp_target.c
)

add_library(main SHARED
    "${VMRP_ROOT}/src/main.c"
    "${VMRP_ROOT}/src/e2e_control.c"
    "${VMRP_ROOT}/src/keyboard_input.c"
    "${VMRP_ROOT}/src/skyengine.c"
    "${VMRP_ROOT}/src/skyengine_args.c"
    "${VMRP_ROOT}/src/utils.c"
    "${VMRP_ROOT}/src/rbtree.c"
    "${VMRP_ROOT}/src/network.c"
    "${VMRP_ROOT}/src/file_lib.c"
    "${VMRP_ROOT}/src/native_platform_memory.c"
    "${VMRP_ROOT}/src/native_dsm_funcs.c"
    "${VMRP_ROOT}/src/native_text_widget.c"
    "${VMRP_ROOT}/src/native_modal_menu.c"
    "${VMRP_ROOT}/src/runtime_native_mythroad.c"
    "${VMRP_ROOT}/src/arm_ext_executor.c"
    "${CMAKE_CURRENT_LIST_DIR}/android_host.c"
)

foreach(src ${SKYENGINE_AEX_SOURCES})
    list(APPEND _SKYENGINE_AEX_ABS "${VMRP_ROOT}/${src}")
endforeach()
foreach(src ${SKYENGINE_MYTHROAD_SOURCES})
    list(APPEND _SKYENGINE_MYTHROAD_ABS "${VMRP_ROOT}/${src}")
endforeach()
target_sources(main PRIVATE ${_SKYENGINE_AEX_ABS} ${_SKYENGINE_MYTHROAD_ABS})

target_include_directories(main PRIVATE
    "${VMRP_ROOT}/src/include"
    "${VMRP_ROOT}/src/mythroad/include"
    "${VMRP_ROOT}/third_party/unicorn/include"
    "${CMAKE_CURRENT_LIST_DIR}"
)

# 与桌面 skyengine 目标对齐的宏。VMRP_SDL_AUDIO 启用 SDL 音频播放
# （native_dsm_funcs.c 在 __ANDROID__ 下改用 <SDL.h> 平铺头）。
target_compile_definitions(main PRIVATE
    TARGET_MOD
    MTK_MOD
    MR_PLAT_DRAWTEXT
    DSM_FULL
    TRUST_BINARIES
    __ANDROID__
    VMRP_SDL_AUDIO
)

target_compile_options(main PRIVATE -O2 -Wno-unused-parameter)

find_library(ANDROID_LIB android)
find_library(LOG_LIB log)
find_library(Z_LIB z)
find_library(M_LIB m)
find_library(DL_LIB dl)

target_link_libraries(main PRIVATE
    SDL2
    unicorn
    ${ANDROID_LIB}
    ${LOG_LIB}
    ${Z_LIB}
    ${M_LIB}
    ${DL_LIB}
)
