# voxgpu: shadPS4 v0.21's renderer, Gnm and VideoOut drivers as a Windows static library.
# Built by build.ps1; the toolchain (clang, llvm-ar) comes from there.
cmake_minimum_required(VERSION 3.24)
project(voxgpu LANGUAGES C CXX)

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "voxgpu builds with clang only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# The static CRT: vox.exe's C objects are compiled by plain clang, which defaults to libcmt.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -gline-tables-only -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -gline-tables-only -DNDEBUG")
# Every library lands in one directory: build.ps1 reports it as the link line's inputs.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_options(-march=x86-64-v3 -w)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _TIMESPEC_DEFINED
    NTDDI_VERSION=0x0A000006 _WIN32_WINNT=0x0A00 WINVER=0x0A00
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE _SCL_SECURE_NO_WARNINGS
    BOOST_ASIO_STANDALONE)

# ---- Dependencies, configured like shadPS4's externals/CMakeLists.txt.
set(EXT ${CMAKE_CURRENT_SOURCE_DIR}/externals)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory(${EXT}/ext-boost EXCLUDE_FROM_ALL)

add_subdirectory(${EXT}/fmt EXCLUDE_FROM_ALL)

set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_AVX512F OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/sdl3 EXCLUDE_FROM_ALL)

set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/vulkan-headers EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/vma EXCLUDE_FROM_ALL)

set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/glslang EXCLUDE_FROM_ALL)

add_subdirectory(${EXT}/robin-map EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/xbyak EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/magic_enum EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/sirit EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/miniz EXCLUDE_FROM_ALL)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_MSVC_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/abseil-cpp EXCLUDE_FROM_ALL)

add_library(xxhash STATIC ${EXT}/xxhash/xxhash.c)
target_include_directories(xxhash PUBLIC ${EXT}/xxhash)

add_library(half INTERFACE)
target_include_directories(half INTERFACE ${EXT}/half/include)

# GCN headers are in tree upstream; a copy keeps the build inside port\gpu.
add_library(gcn INTERFACE)
target_include_directories(gcn INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/gcn/include)

# Zydis: the same libraries vox.exe already links (port\deps), so one copy ends up in the program.
get_filename_component(VOX_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/../deps ABSOLUTE)
add_library(zydis_prebuilt INTERFACE)
target_include_directories(zydis_prebuilt INTERFACE ${VOX_DEPS}/include)
target_compile_definitions(zydis_prebuilt INTERFACE ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD)
target_link_libraries(zydis_prebuilt INTERFACE ${VOX_DEPS}/lib/Zydis.lib ${VOX_DEPS}/lib/Zycore.lib)

# ---- Host shaders -> headers (shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    set(DEPFILE ${OUT}.d)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND $<TARGET_FILE:glslang-standalone> --target-env vulkan1.3
                -o ${OUT} --vn ${SYMBOL} --depfile ${DEPFILE} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS glslang-standalone ${SHADER_SRC}/${FILENAME} ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPFILE ${DEPFILE}
        BYPRODUCTS ${DEPFILE}
        VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

# ---- voxgpu.
file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp shadps4/common/io_file.cpp
    shadps4/common/native_clock.cpp shadps4/common/ntapi.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/signal_context.cpp shadps4/common/slot_vector.cpp shadps4/common/spin_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp
    shadps4/common/logging/classes.cpp)
file(GLOB_RECURSE SHIM CONFIGURE_DEPENDS shim/*.cpp)

add_library(voxgpu STATIC ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp
    shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp)
add_dependencies(voxgpu host_shaders)
target_include_directories(voxgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(voxgpu PRIVATE -Wno-invalid-offsetof -fno-strict-aliasing)
target_link_libraries(voxgpu PUBLIC
    Boost::headers fmt::fmt SDL3::SDL3-static Vulkan::Headers GPUOpen::VulkanMemoryAllocator
    tsl::robin_map xbyak::xbyak magic_enum::magic_enum sirit miniz xxhash half gcn zydis_prebuilt absl::flat_hash_map)
target_link_libraries(voxgpu PUBLIC mincore ws2_32 mswsock winmm imm32 version setupapi ole32 oleaut32
    uuid gdi32 user32 shell32 advapi32 cfgmgr32)

# ---- The window and Vulkan smoke test.
add_executable(voxgpu_test test/voxgpu_test.c)
target_include_directories(voxgpu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
get_filename_component(VOX_LLVM ${CMAKE_C_COMPILER} DIRECTORY)
target_link_libraries(voxgpu_test PRIVATE voxgpu
    ${VOX_LLVM}/../lib/clang/23/lib/windows/clang_rt.builtins-x86_64.lib)
set_target_properties(voxgpu_test PROPERTIES LINK_FLAGS "-fuse-ld=lld")
