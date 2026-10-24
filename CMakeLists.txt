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
# a submodule, write its patch again with `python tools/lib_patch.py save` before committing. Patches apply in name order,
# so a nested submodule's (RecompFrontend--...) comes after its parent's.
find_package(Git REQUIRED)
file(GLOB LIB_PATCHES "${CMAKE_SOURCE_DIR}/lib/patches/*.patch")
foreach(LIB_PATCH ${LIB_PATCHES})
    get_filename_component(LIB_NAME ${LIB_PATCH} NAME_WE)
    # A nested submodule's patch names its path with "--" for "/" (RecompFrontend--recompui--lib--RmlUi).
    string(REPLACE "--" "/" LIB_SUBDIR ${LIB_NAME})
    set(LIB_DIR "${CMAKE_SOURCE_DIR}/lib/${LIB_SUBDIR}")
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

# Online play (src/online/, docs/online.md): ENet for the connections, miniupnpc to open the host's port on the router.
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/enet ${CMAKE_BINARY_DIR}/enet)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/miniupnp/miniupnpc ${CMAKE_BINARY_DIR}/miniupnpc)

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

# Where the version and build ID are read from: the source tree, or the checkout a copy was made from (tools/build_linux.sh
# builds a copy without .git). safe.directory lets WSL's git read a checkout owned by the Windows user.
set(RUSH2_GIT_SOURCE "${CMAKE_SOURCE_DIR}" CACHE PATH "Git checkout the version and build ID are read from")

# The version shown in the launcher is the latest git tag (v1.0.8 -> 1.0.8), read when CMake configures; 0.1.0 without one.
set(RUSH2_VERSION "0.1.0")
execute_process(COMMAND git -c safe.directory=* describe --tags --abbrev=0 WORKING_DIRECTORY ${RUSH2_GIT_SOURCE}
                OUTPUT_VARIABLE RUSH2_GIT_TAG OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE RUSH2_GIT_RESULT)
if (RUSH2_GIT_RESULT EQUAL 0 AND RUSH2_GIT_TAG MATCHES "^v?([0-9]+[.][0-9]+[.][0-9]+)")
    set(RUSH2_VERSION ${CMAKE_MATCH_1})
endif()
target_compile_definitions(Rush2Recompiled PRIVATE RUSH2_VERSION="${RUSH2_VERSION}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RUSH2_GIT_SOURCE}/.git/packed-refs)

# The commit, for online play's compatibility key (src/online/online_net.cpp): only identical builds race together.
# Read again whenever HEAD moves.
set(RUSH2_BUILD_ID "unknown")
execute_process(COMMAND git -c safe.directory=* rev-parse --short=10 HEAD WORKING_DIRECTORY ${RUSH2_GIT_SOURCE}
                OUTPUT_VARIABLE RUSH2_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE RUSH2_GIT_COMMIT_RESULT)
if (RUSH2_GIT_COMMIT_RESULT EQUAL 0)
    set(RUSH2_BUILD_ID ${RUSH2_GIT_COMMIT})
endif()
target_compile_definitions(Rush2Recompiled PRIVATE RUSH2_BUILD_ID="${RUSH2_BUILD_ID}")
if (EXISTS ${RUSH2_GIT_SOURCE}/.git/logs/HEAD)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RUSH2_GIT_SOURCE}/.git/logs/HEAD)
endif()

target_sources(Rush2Recompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/config.cpp
    ${CMAKE_SOURCE_DIR}/src/data_location.cpp
    ${CMAKE_SOURCE_DIR}/src/data_files.cpp
    ${CMAKE_SOURCE_DIR}/src/atomics.cpp
    ${CMAKE_SOURCE_DIR}/src/threads.cpp
    ${CMAKE_SOURCE_DIR}/src/hitch_profiler.cpp
    ${CMAKE_SOURCE_DIR}/src/pak.cpp
    ${CMAKE_SOURCE_DIR}/src/hud.cpp
    ${CMAKE_SOURCE_DIR}/src/hud_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/hud_layout.cpp
    ${CMAKE_SOURCE_DIR}/src/interpolation.cpp
    ${CMAKE_SOURCE_DIR}/src/lod.cpp
    ${CMAKE_SOURCE_DIR}/src/draw_distance.cpp
    ${CMAKE_SOURCE_DIR}/src/cheats.cpp
    ${CMAKE_SOURCE_DIR}/src/fonts.cpp
    ${CMAKE_SOURCE_DIR}/src/texture_upscale.cpp
    ${CMAKE_SOURCE_DIR}/src/texture_upscale_images.cpp
    ${CMAKE_SOURCE_DIR}/src/texture_census.cpp
    ${CMAKE_SOURCE_DIR}/src/texture_derive.cpp
    ${CMAKE_SOURCE_DIR}/src/body_sheet.cpp
    ${CMAKE_SOURCE_DIR}/src/car_mods.cpp
    ${CMAKE_SOURCE_DIR}/src/car_probes.cpp
    ${CMAKE_SOURCE_DIR}/src/car_probes_bake.cpp
    ${CMAKE_SOURCE_DIR}/src/gltf_scene.cpp
    ${CMAKE_SOURCE_DIR}/src/track_mod/track_mod.cpp
    ${CMAKE_SOURCE_DIR}/src/track_mod/track_mod_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/track_mod/track_mod_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/arcade2049/arcade_data.cpp
    ${CMAKE_SOURCE_DIR}/src/arcade2049/arcade_glb.cpp
    ${CMAKE_SOURCE_DIR}/src/arcade2049/arcade_track.cpp
    ${CMAKE_SOURCE_DIR}/src/arcade2049/arcade_cars.cpp
    ${CMAKE_SOURCE_DIR}/src/arcade2049/arcade_install.cpp
    ${CMAKE_SOURCE_DIR}/src/arcade2049/arcade_checkpoints.cpp
    ${CMAKE_SOURCE_DIR}/lib/Clipper2/CPP/Clipper2Lib/src/clipper.engine.cpp
    ${CMAKE_SOURCE_DIR}/lib/Clipper2/CPP/Clipper2Lib/src/clipper.offset.cpp
    ${CMAKE_SOURCE_DIR}/lib/Clipper2/CPP/Clipper2Lib/src/clipper.rectclip.cpp
    ${CMAKE_SOURCE_DIR}/src/asset_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/asset_catalog_walk.cpp
    ${CMAKE_SOURCE_DIR}/lib/hqx/hq2x.c
    ${CMAKE_SOURCE_DIR}/lib/hqx/hq4x.c
    ${CMAKE_SOURCE_DIR}/src/players4.cpp
    ${CMAKE_SOURCE_DIR}/src/splitscreen.cpp
    ${CMAKE_SOURCE_DIR}/src/widescreen.cpp
    ${CMAKE_SOURCE_DIR}/src/input.cpp
    ${CMAKE_SOURCE_DIR}/src/rumble.cpp
    ${CMAKE_SOURCE_DIR}/src/light_bar.cpp
    ${CMAKE_SOURCE_DIR}/src/dualsense_haptics.cpp
    ${CMAKE_SOURCE_DIR}/src/controls.cpp
    ${CMAKE_SOURCE_DIR}/src/controls_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/controls_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/control_profiles.cpp
    ${CMAKE_SOURCE_DIR}/src/steam_haptics.cpp
    ${CMAKE_SOURCE_DIR}/src/steam_input_filter.cpp
    ${CMAKE_SOURCE_DIR}/src/linux_devices.cpp
    ${CMAKE_SOURCE_DIR}/src/linux_window.cpp
    ${CMAKE_SOURCE_DIR}/src/circuit.cpp
    ${CMAKE_SOURCE_DIR}/src/npc_cars.cpp
    ${CMAKE_SOURCE_DIR}/src/paint_colors.cpp
    ${CMAKE_SOURCE_DIR}/src/game_fixes.cpp
    ${CMAKE_SOURCE_DIR}/src/camera.cpp
    ${CMAKE_SOURCE_DIR}/src/games_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/setup.cpp
    ${CMAKE_SOURCE_DIR}/src/online_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/online/online_net.cpp
    ${CMAKE_SOURCE_DIR}/src/online/online_discovery.cpp
    ${CMAKE_SOURCE_DIR}/src/online/online_upnp.cpp
    ${CMAKE_SOURCE_DIR}/src/online/online_game.cpp
    ${CMAKE_SOURCE_DIR}/src/online/online_lobby.cpp
    ${CMAKE_SOURCE_DIR}/src/ram_watch.cpp
    ${CMAKE_SOURCE_DIR}/src/options_page.cpp
    ${CMAKE_SOURCE_DIR}/src/register_overlays.cpp
    ${CMAKE_SOURCE_DIR}/src/assets.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/rush2049_rom.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049dc/rush2049_dc.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049dc/rush2049_dc_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049dc/rush2049_dc_model.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049dc/rush2049_dc_pack.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049dc/rush2049_dc_tables.cpp
    ${CMAKE_SOURCE_DIR}/src/collectibles.cpp
    ${CMAKE_SOURCE_DIR}/src/progress_tab.cpp
    ${CMAKE_SOURCE_DIR}/src/unlocks.cpp
    ${CMAKE_SOURCE_DIR}/src/ghost.cpp
    ${CMAKE_SOURCE_DIR}/src/game_filter.cpp
    ${CMAKE_SOURCE_DIR}/src/race_record.cpp
    ${CMAKE_SOURCE_DIR}/src/race_setup.cpp
    ${CMAKE_SOURCE_DIR}/src/sim_run.cpp
    ${CMAKE_SOURCE_DIR}/src/test_race.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/ghost_logic.cpp
    ${CMAKE_SOURCE_DIR}/src/unlocks_shop.cpp
    ${CMAKE_SOURCE_DIR}/src/rush1/track1.cpp
    ${CMAKE_SOURCE_DIR}/src/rush1/track1_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/rush1/track1_audio.cpp
    ${CMAKE_SOURCE_DIR}/src/rush1/track1_buses.cpp
    ${CMAKE_SOURCE_DIR}/src/music.cpp
    ${CMAKE_SOURCE_DIR}/src/pause_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/engine_preview.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/engine2049.cpp
    ${CMAKE_SOURCE_DIR}/src/car_engines.cpp
    ${CMAKE_SOURCE_DIR}/src/rush1/rush1_rom.cpp
    ${CMAKE_SOURCE_DIR}/src/calspeed/calspeed_rom.cpp
    ${CMAKE_SOURCE_DIR}/src/calspeed/calspeed_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/calspeed/calspeed_cars.cpp
    ${CMAKE_SOURCE_DIR}/src/calspeed/calspeed_physics.cpp
    ${CMAKE_SOURCE_DIR}/src/calspeed/calspeed_walls.cpp
    ${CMAKE_SOURCE_DIR}/src/calspeed/calspeed_logos.cpp
    ${CMAKE_SOURCE_DIR}/src/calspeed/calspeed_scenery.cpp
    ${CMAKE_SOURCE_DIR}/src/calspeed/calspeed_billboards.cpp
    ${CMAKE_SOURCE_DIR}/src/smooth_collision.cpp
    ${CMAKE_SOURCE_DIR}/src/surface_fx.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049.cpp
    ${CMAKE_SOURCE_DIR}/src/track_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/prewarm.cpp
    ${CMAKE_SOURCE_DIR}/src/texture_prewarm.cpp
    ${CMAKE_SOURCE_DIR}/src/texture_origin.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049_art.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049_menu.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049_records.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049_collision.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049_movers.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049_movers_logic.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049_props.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/battle.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/battle_ai.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/battle_render.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/odometer.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/font2049.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/stunt2049.cpp
    ${CMAKE_SOURCE_DIR}/src/arrows.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049_audio.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/track2049_texanim.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/car2049.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/audio2049.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049dc/audio2049_dc.cpp
    ${CMAKE_SOURCE_DIR}/src/rush1/car1_decals.cpp
    ${CMAKE_SOURCE_DIR}/src/rush1/car1_stripes.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/wings.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/wings_rom.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/wings_state.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/wings_render.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/wings_sound.cpp
    ${CMAKE_SOURCE_DIR}/src/rush2049/wings_menu.cpp
    ${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp
)

target_include_directories(Rush2Recompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/lib/tomlplusplus
    ${CMAKE_SOURCE_DIR}/lib/hqx
    ${CMAKE_SOURCE_DIR}/lib/Clipper2/CPP/Clipper2Lib/include
    ${CMAKE_SOURCE_DIR}/lib/earcut/include
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

    # SDL3 under the SDL2 API: sdl2-compat's SDL2.dll forwards every SDL2 call (ours, the frontend's and RT64's) to the
    # SDL3.dll it ships with. SDL3 brings native drivers SDL2 lacks, such as the new Steam Controller (its puck,
    # 28DE:1304) without Steam running.
    set(SDL2_COMPAT_VERSION "2.32.74") # Bundles SDL3 3.4.18.
    FetchContent_Declare(
        sdl2
        URL https://github.com/libsdl-org/sdl2-compat/releases/download/release-${SDL2_COMPAT_VERSION}/sdl2-compat-devel-${SDL2_COMPAT_VERSION}-VC.zip
    )
    FetchContent_MakeAvailable(sdl2)
    target_include_directories(Rush2Recompiled PRIVATE ${sdl2_SOURCE_DIR}/include)
    target_link_directories(Rush2Recompiled PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)

    # Copy SDL2 (sdl2-compat), SDL3 and dxc DLLs to the output folder
    add_custom_command(TARGET Rush2Recompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${sdl2_SOURCE_DIR}/lib/x64/SDL3.dll"
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
    target_link_libraries(Rush2Recompiled PRIVATE SDL2 Winmm.lib hid.lib cfgmgr32.lib ole32.lib)

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
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # DualSense haptics play on the controller's sound card through PulseAudio (or PipeWire's pulse server), or
        # ALSA when the card is split by alsa-ucm-conf (src/dualsense_haptics.cpp). GTK (nfd's file pickers) to keep
        # its dialogs in front of the game (src/linux_window.cpp).
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse libpulse-simple alsa gtk+-3.0)
        target_link_libraries(Rush2Recompiled PRIVATE PkgConfig::PULSE)
    endif()
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

target_include_directories(Rush2Recompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/enet/include
    ${CMAKE_SOURCE_DIR}/lib/miniupnp/miniupnpc/include
)
if (WIN32)
    target_link_libraries(Rush2Recompiled PRIVATE ws2_32 iphlpapi)
endif()

target_link_libraries(Rush2Recompiled PRIVATE
    enet
    libminiupnpc-static
    RecompiledFuncs
    recompui
    recompinput
    librecomp
    ultramodern
    rt64
    nfd
)
# recompinput uses recompui too; GNU ld only resolves symbols from libraries listed after their user.
if (NOT WIN32)
    target_link_libraries(Rush2Recompiled PRIVATE recompui)
endif()

set_property(TARGET Rush2Recompiled PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
