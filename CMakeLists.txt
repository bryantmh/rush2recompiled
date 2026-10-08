cmake_minimum_required(VERSION 3.20)

project(Rush2Recompiled LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-everything /W4")
endif()

# Opt out of constexpr mutex constructor on windows to prevent vcredist issues
if (WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
endif()

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if (WIN32)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/lib/")
endif()

set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(
    HLSL_CPU
    # SDL 2.26's SDL_endian.h redefines _m_prefetch, which is a builtin in recent clang.
    __PRFCHWINTRIN_H
)

# Changes to the submodules in lib/ are kept as lib/patches/<submodule>.patch, so they commit with this project and the
# submodules stay on their upstream commits. Each patch is applied to its submodule's working tree here. After editing
# a submodule, write its patch again with `python tools/lib_patch.py save` before committing.
find_package(Git REQUIRED)
file(GLOB LIB_PATCHES "${CMAKE_SOURCE_DIR}/lib/patches/*.patch")
foreach(LIB_PATCH ${LIB_PATCHES})
    get_filename_component(LIB_NAME ${LIB_PATCH} NAME_WE)
    set(LIB_DIR "${CMAKE_SOURCE_DIR}/lib/${LIB_NAME}")
    set(LIB_APPLIED "${CMAKE_BINARY_DIR}/lib_patches/${LIB_NAME}.patch") # The patch as it was last applied.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LIB_PATCH})

    # Nothing to do if the working tree already has the patch, as it does after editing the submodule and saving.
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${LIB_PATCH}
        WORKING_DIRECTORY ${LIB_DIR} RESULT_VARIABLE LIB_NOT_APPLIED OUTPUT_QUIET ERROR_QUIET)
    if (LIB_NOT_APPLIED)
        # The patch changed since it was applied (a pull, say): take the old version out first.
        if (EXISTS ${LIB_APPLIED})
            execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse ${LIB_APPLIED}
                WORKING_DIRECTORY ${LIB_DIR} OUTPUT_QUIET ERROR_QUIET)
        endif()
        execute_process(COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${LIB_PATCH}
            WORKING_DIRECTORY ${LIB_DIR} RESULT_VARIABLE LIB_PATCH_FAILED)
        if (LIB_PATCH_FAILED)
            message(FATAL_ERROR "lib/patches/${LIB_NAME}.patch doesn't apply to lib/${LIB_NAME}. If the submodule has "
                "edits of its own, save them with tools/lib_patch.py or discard them, then configure again.")
        endif()
        message(STATUS "Applied lib/patches/${LIB_NAME}.patch")

        # RT64's shader builds don't track the .hlsli files a shader includes, so rebuild them all.
        file(GLOB LIB_SHADERS "${LIB_DIR}/src/shaders/*.hlsl")
        if (LIB_SHADERS)
            file(TOUCH_NOCREATE ${LIB_SHADERS})
        endif()
    endif()
    configure_file(${LIB_PATCH} ${LIB_APPLIED} COPYONLY)
endforeach()

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

set(BUILD_SHARED_LIBS OFF)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)

target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)

# imgui trips -Wuninitialized-const-pointer, which recent clang treats as an error.
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(rt64 PRIVATE -Wno-error=uninitialized-const-pointer)
endif()

# RecompiledFuncs - the N64Recomp output (run N64Recomp us.toml first)
add_library(RecompiledFuncs STATIC)

target_compile_options(RecompiledFuncs PRIVATE
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
    -falign-functions=16
)

target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
)

file(GLOB FUNC_C_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)
target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})

# Main executable
add_executable(Rush2Recompiled)

target_sources(Rush2Recompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
    ${CMAKE_SOURCE_DIR}/src/data_location.cpp
    ${CMAKE_SOURCE_DIR}/src/data_files.cpp
    ${CMAKE_SOURCE_DIR}/src/atomics.cpp
    ${CMAKE_SOURCE_DIR}/src/threads.cpp
    ${CMAKE_SOURCE_DIR}/src/pak.cpp
    ${CMAKE_SOURCE_DIR}/src/hud.cpp
    ${CMAKE_SOURCE_DIR}/src/interpolation.cpp
    ${CMAKE_SOURCE_DIR}/src/lod.cpp
    ${CMAKE_SOURCE_DIR}/src/draw_distance.cpp
    ${CMAKE_SOURCE_DIR}/src/cheats.cpp
    ${CMAKE_SOURCE_DIR}/src/fonts.cpp
    ${CMAKE_SOURCE_DIR}/src/players4.cpp
    ${CMAKE_SOURCE_DIR}/src/splitscreen.cpp
    ${CMAKE_SOURCE_DIR}/src/widescreen.cpp
    ${CMAKE_SOURCE_DIR}/src/input.cpp
    ${CMAKE_SOURCE_DIR}/src/controls.cpp
    ${CMAKE_SOURCE_DIR}/src/controls_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/players_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/games_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/options_page.cpp
    ${CMAKE_SOURCE_DIR}/src/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/assets.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049_rom.cpp
    ${CMAKE_SOURCE_DIR}/src/collectibles.cpp
    ${CMAKE_SOURCE_DIR}/src/progress_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/unlocks.cpp
    ${CMAKE_SOURCE_DIR}/src/ghost.cpp
    ${CMAKE_SOURCE_DIR}/src/ghost_logic.cpp
    ${CMAKE_SOURCE_DIR}/src/unlocks_shop.cpp
    ${CMAKE_SOURCE_DIR}/src/track1.cpp
    ${CMAKE_SOURCE_DIR}/src/track1_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/track1_audio.cpp
    ${CMAKE_SOURCE_DIR}/src/music.cpp
    ${CMAKE_SOURCE_DIR}/src/engine_preview.cpp
    ${CMAKE_SOURCE_DIR}/src/engine2049.cpp
    ${CMAKE_SOURCE_DIR}/src/car_engines.cpp
    ${CMAKE_SOURCE_DIR}/src/rush1_rom.cpp
    ${CMAKE_SOURCE_DIR}/src/track2049.cpp
    ${CMAKE_SOURCE_DIR}/src/track2049_art.cpp
    ${CMAKE_SOURCE_DIR}/src/track2049_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/track2049_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/track2049_records.cpp
    ${CMAKE_SOURCE_DIR}/src/track2049_movers.cpp
    ${CMAKE_SOURCE_DIR}/src/track2049_movers_logic.cpp
    ${CMAKE_SOURCE_DIR}/src/track2049_props.cpp
    ${CMAKE_SOURCE_DIR}/src/track2049_audio.cpp
    ${CMAKE_SOURCE_DIR}/src/track2049_texanim.cpp
    ${CMAKE_SOURCE_DIR}/src/car2049.cpp
    ${CMAKE_SOURCE_DIR}/src/audio2049.cpp
    ${CMAKE_SOURCE_DIR}/src/car1_decals.cpp
    ${CMAKE_SOURCE_DIR}/src/car1_stripes.cpp
    ${CMAKE_SOURCE_DIR}/src/wings.cpp
    ${CMAKE_SOURCE_DIR}/src/wings_rom.cpp
    ${CMAKE_SOURCE_DIR}/src/wings_state.cpp
    ${CMAKE_SOURCE_DIR}/src/wings_render.cpp
    ${CMAKE_SOURCE_DIR}/src/wings_sound.cpp
    ${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp
)

target_include_directories(Rush2Recompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/sse2neon
    ${CMAKE_SOURCE_DIR}/lib/rt64/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/render
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompinput/include
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompui/include
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompui/src
    ${CMAKE_SOURCE_DIR}/lib/RecompFrontend/lib/SlotMap
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

if (CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    target_compile_options(Rush2Recompiled PRIVATE -march=nehalem -fno-strict-aliasing -fms-extensions)
else()
    target_compile_options(Rush2Recompiled PRIVATE -fno-strict-aliasing -fms-extensions)
endif()

if (MSVC)
    # Disable identical code folding, since this breaks mod function patching as multiple functions can get merged into one.
    target_link_options(Rush2Recompiled PRIVATE /OPT:NOICF)
endif()

if (WIN32)
    add_compile_definitions(NOMINMAX)
    include(FetchContent)

    set(SDL2_VERSION "2.30.3")
    FetchContent_Declare(
        sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
    )
    FetchContent_MakeAvailable(sdl2)
    target_include_directories(Rush2Recompiled PRIVATE ${sdl2_SOURCE_DIR}/include)
    target_link_directories(Rush2Recompiled PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)

    # Copy SDL2 and dxc DLLs to the output folder
    add_custom_command(TARGET Rush2Recompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:Rush2Recompiled>)

    # No console window; pass --show-console to get one.
    set_target_properties(Rush2Recompiled PROPERTIES
        LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
        LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
        LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
    target_link_libraries(Rush2Recompiled PRIVATE SDL2 Winmm.lib)

    # Executable icon (drawn by tools/build_icons.py). SDL also uses it for the window.
    target_sources(Rush2Recompiled PRIVATE ${CMAKE_SOURCE_DIR}/icons/app.rc)
else()
    find_package(SDL2 REQUIRED)
    add_compile_definitions("PLUME_SDL_VULKAN_ENABLED")
    add_compile_definitions("RT64_SDL_WINDOW_VULKAN")
    target_include_directories(Rush2Recompiled PRIVATE ${SDL2_INCLUDE_DIRS})
    find_package(Freetype REQUIRED)
    target_link_libraries(Rush2Recompiled PRIVATE ${FREETYPE_LIBRARIES} SDL2::SDL2)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
    target_link_libraries(Rush2Recompiled PRIVATE "-latomic -static-libstdc++" ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# Copy the UI assets and controller database next to the executable.
add_custom_command(TARGET Rush2Recompiled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:Rush2Recompiled>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/recompcontrollerdb.txt" $<TARGET_FILE_DIR:Rush2Recompiled>)

# TODO fix the rt64 CMake script so that this doesn't need to be duplicated here
# For DXC
set (DXC_COMMON_OPTS "-I${PROJECT_SOURCE_DIR}/src")
set (DXC_DXIL_OPTS "-Wno-ignored-attributes")
set (DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
set (DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
set (DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
set (DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T cs_6_0")
set (DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T gs_6_0")
set (DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3" "-fspv-target-env=vulkan1.1spirv1.4" "-fspv-extension=SPV_KHR_ray_tracing" "-fspv-extension=SPV_EXT_descriptor_indexing")

if (WIN32)
    set (DXC "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc.exe")
elseif (CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/x64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc-linux")
else()
    set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-linux")
endif()

set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)
set(RECOMP_FRONTEND_RT64_PATH ${CMAKE_SOURCE_DIR}/lib/rt64)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/RecompFrontend)

target_link_libraries(Rush2Recompiled PRIVATE
    RecompiledFuncs
    recompui
    recompinput
    librecomp
    ultramodern
    rt64
    nfd
)

set_property(TARGET Rush2Recompiled PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
