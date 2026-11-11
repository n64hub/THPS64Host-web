# THPS recomp host, shared by THPS1-3 (ADR 0006): N64ModernRuntime + RT64 + N64Recomp output, one payload per code class
# (versions/classes.toml; docs/versions.md). This file is include()d by each game's recomp-host/CMakeLists.txt, which sets
#   THPS_FAMILY       thps1 | thps2 | thps3  (the executable is <family>_recomp; game/game.toml is the profile)
#   THPS_SKIP_INTRO   "game" (the profile's game/skip_intro.cpp) | "overlay" (the shared skip_intro_overlay.cpp)
#   THPS_GAME_EXTRA_SOURCES  optional: more of the profile's own C++ files for the engine (THPS1: game/name_entry.cpp)
# after project(). Sources are in decomp/common/host/src; the game's own are in <game>/recomp-host/game/; build output
# goes under <game>/recomp-host/build/. Generate the recompiled C first (tools/gen.py), then:
#
#   cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#         -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
#         -DRECOMP_RUNTIME_DIR=<runtime-dir> (tools/setup_runtime.py creates it)
#   cmake --build build/cmake
#
# Runtime pins (see README): N64ModernRuntime cdf5abb, rt64 4337374 (+ our patches: tools/setup_runtime.py).
#
# Windows (clang-cl from Visual Studio 2022, "C++ Clang Compiler for Windows") and Linux are configured by the same file;
# README "Windows" and docs/WINDOWS-TEST.md. A build with no ROM at all (CI, platform checks):
#   python3 -I tools/setup_runtime.py <dir>                  # the pinned runtime + our patches
#   cmake -S . -B build/cmake -G Ninja -DRECOMP_RUNTIME_DIR=<dir> -DTHPS_STUB_FUNCS=ON
# Windows: add  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl  from a "x64 Native Tools" prompt.
# Platform code: src/platform.cpp, src/crash.cpp, create_window() in src/main.cpp, src/rt64_context.cpp.

if (NOT THPS_FAMILY)
    message(FATAL_ERROR "set THPS_FAMILY (thps1|thps2|thps3) before including thps_host.cmake")
endif()
if (NOT DEFINED THPS_SKIP_INTRO)
    set(THPS_SKIP_INTRO "game")
endif()
get_filename_component(THPS_SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)   # decomp/common/host
get_filename_component(THPS_FIRSTRUN_DIR "${THPS_SHARED_DIR}/../firstrun" ABSOLUTE)   # decomp/common/firstrun (overlay_rom.{h,cpp})
set(THPS_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}")                                     # decomp/thps<N>/recomp-host
string(TOUPPER "${THPS_FAMILY}" THPS_FAMILY_UPPER)
set(THPS_EXE "${THPS_FAMILY}_recomp")
set(THPS_GEN_VERSIONS "${THPS_GAME_DIR}/tools/gen_versions.py")
if (NOT EXISTS "${THPS_GAME_DIR}/game/game.toml")
    message(FATAL_ERROR "${THPS_GAME_DIR}/game/game.toml (the game profile) is missing")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${THPS_GAME_DIR}/game/game.toml")
# Front-end overlays (THPS2/3): the profile says so; read it here without a TOML parser.
file(STRINGS "${THPS_GAME_DIR}/game/game.toml" THPS_OVERLAYS_LINE REGEX "^overlays[ ]*=")
if (THPS_OVERLAYS_LINE MATCHES "true")
    set(THPS_OVERLAYS TRUE)
else()
    set(THPS_OVERLAYS FALSE)
endif()
if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

# clang-cl / cl.exe take MSVC-style options; clang, gcc and clang from mingw take GNU-style ones.
set(THPS_MSVC_STYLE FALSE)
if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR (MSVC AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT))
    set(THPS_MSVC_STYLE TRUE)
    if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "On Windows use clang-cl (-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl), not cl.exe: "
                            "the generated C is 17 MB and upstream builds with clang-cl only")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# The page (tools/build_web.py, emcmake): the game in a browser through N64Recomp-browser's web support
# (-DN64RECOMP_BROWSER_DIR=<its checkout>, a game repository's lib/N64Recomp-browser): RT64 and the menu on WebGPU in the
# page's canvas, sound on the browser's audio thread, settings, saves and the ROM in the browser's storage. Everything
# is compiled with threads (pthreads), wasm exceptions, wasm SIMD and tail calls; with -msimd128 xxHash would take its
# NEON path through Emscripten's arm_neon.h, which doesn't compile in C++.
if (EMSCRIPTEN)
    set(N64RECOMP_BROWSER_DIR "" CACHE PATH "An N64Recomp-browser checkout (the page's build)")
    if (NOT EXISTS "${N64RECOMP_BROWSER_DIR}/frontend/CMakeLists.txt")
        message(FATAL_ERROR "Set -DN64RECOMP_BROWSER_DIR=<an N64Recomp-browser checkout> (tools/build_web.py does)")
    endif()
    add_compile_options(-pthread -fwasm-exceptions -msimd128 -mtail-call "-DXXH_HAS_INCLUDE(x)=0" -sUSE_SDL=2)
    include_directories(SYSTEM "${EMSCRIPTEN_SYSROOT}/include/SDL2")
    add_link_options(-pthread -fwasm-exceptions -mtail-call)
    # ccache when it is on PATH, and paths in the page relative to the checkout; before rt64 and the runtime are added.
    include("${N64RECOMP_BROWSER_DIR}/frontend/build_speed.cmake")
    # The shaders the page specializes at build time and builds before the game draws with them (its header says how
    # it was recorded); before rt64 is added.
    if (EXISTS "${THPS_GAME_DIR}/web/rt64_shaders.txt")
        set(RT64_PREBUILT_SHADER_LIST "${THPS_GAME_DIR}/web/rt64_shaders.txt")
    endif()
endif()

set(RECOMP_RUNTIME_DIR "" CACHE PATH "Directory holding lib/N64ModernRuntime and lib/rt64")
if (NOT RECOMP_RUNTIME_DIR)
    message(FATAL_ERROR "Set -DRECOMP_RUNTIME_DIR=<runtime-dir> (tools/setup_runtime.py creates it)")
endif()
# ROM-free build: stub RecompiledFuncs (tools/gen_stub.py) in place of tools/gen.py's output. Links, cannot play.
option(THPS_STUB_FUNCS "Link ROM-free stub RecompiledFuncs (tools/gen_stub.py) instead of tools/gen.py output: for CI and platform builds" OFF)
if (THPS_STUB_FUNCS)
    enable_testing()
    add_executable(thps_trigger_zones_test "${THPS_SHARED_DIR}/tests/trigger_zones_test.cpp")
    target_include_directories(thps_trigger_zones_test PRIVATE "${THPS_SHARED_DIR}/src")
    add_test(NAME trigger_zones COMMAND thps_trigger_zones_test)
    add_executable(thps_zone_ownership_test "${THPS_SHARED_DIR}/tests/zone_ownership_test.cpp")
    target_include_directories(thps_zone_ownership_test PRIVATE "${THPS_SHARED_DIR}/src")
    add_test(NAME zone_ownership COMMAND thps_zone_ownership_test)
    add_executable(thps_ride_v3_format_test "${THPS_SHARED_DIR}/tests/ride_v3_format_test.cpp")
    add_test(NAME ride_v3_format COMMAND thps_ride_v3_format_test)
    add_executable(thps_ride_step_state_test "${THPS_SHARED_DIR}/tests/ride_step_state_test.cpp")
    add_test(NAME ride_step_state COMMAND thps_ride_step_state_test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ride_step_profile COMMAND "${Python3_EXECUTABLE}" -B -I "${THPS_SHARED_DIR}/tests/ride_step_profile_test.py")
    add_test(NAME ride_harness COMMAND "${Python3_EXECUTABLE}" -B -I "${THPS_SHARED_DIR}/tests/ride_harness_test.py")
    add_executable(thps_ride_step_runtime_test "${THPS_SHARED_DIR}/src/ride.cpp" "${THPS_SHARED_DIR}/tests/ride_step_runtime_test.cpp")
    target_compile_definitions(thps_ride_step_runtime_test PRIVATE THPS_RIDE_STATE_TEST)
    find_package(Threads REQUIRED)
    target_link_libraries(thps_ride_step_runtime_test PRIVATE Threads::Threads)
    add_test(NAME ride_step_runtime COMMAND thps_ride_step_runtime_test "${CMAKE_CURRENT_BINARY_DIR}/ride-step-test-inputs")
endif()
# Split build (the THPS1 repository's docs/internals/recomp.md): the app holds no game code. The recompiled functions and the audio
# microcode are built on the user's machine from their ROM into a separate library (src/game_split.cpp, game-lib/), so this
# configuration needs no ROM, no N64Recomp output and no generated C: only the version header (tools/gen_versions.py header).
option(THPS_SPLIT_GAME_LIB "Engine only: the game code is a separately built library (first-run translation + local compile)" OFF)
if (THPS_SPLIT_GAME_LIB AND THPS_STUB_FUNCS)
    message(FATAL_ERROR "THPS_SPLIT_GAME_LIB and THPS_STUB_FUNCS are alternatives")
endif()
if (THPS_SPLIT_GAME_LIB)
    set(THPS_GEN_DIR_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/build/gen-split")
elseif (THPS_STUB_FUNCS)
    set(THPS_GEN_DIR_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/build/gen-stub")
else()
    set(THPS_GEN_DIR_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/build/gen")
endif()
set(THPS_GEN_DIR "${THPS_GEN_DIR_DEFAULT}" CACHE PATH "tools/gen.py (or gen_stub.py) output (<class>/RecompiledFuncs/, thps_versions.h)")
# A cached default follows THPS_STUB_FUNCS / THPS_SPLIT_GAME_LIB (a hand-set path is kept): build_windows.ps1 -Stub then
# -Rom reuses build/cmake, and the cached build/gen-stub would otherwise link the stub functions into the "real" build.
foreach(other "${CMAKE_CURRENT_SOURCE_DIR}/build/gen" "${CMAKE_CURRENT_SOURCE_DIR}/build/gen-stub" "${CMAKE_CURRENT_SOURCE_DIR}/build/gen-split")
    if (THPS_GEN_DIR STREQUAL other AND NOT THPS_GEN_DIR STREQUAL THPS_GEN_DIR_DEFAULT)
        message(STATUS "THPS_GEN_DIR: ${other} -> ${THPS_GEN_DIR_DEFAULT} (THPS_STUB_FUNCS=${THPS_STUB_FUNCS} THPS_SPLIT_GAME_LIB=${THPS_SPLIT_GAME_LIB})")
        set_property(CACHE THPS_GEN_DIR PROPERTY VALUE "${THPS_GEN_DIR_DEFAULT}")
        set(THPS_GEN_DIR "${THPS_GEN_DIR_DEFAULT}")
    endif()
endforeach()
set(NMR_DIR "${RECOMP_RUNTIME_DIR}/lib/N64ModernRuntime")
set(RT64_DIR "${RECOMP_RUNTIME_DIR}/lib/rt64")
set(FRONTEND_DIR "${RECOMP_RUNTIME_DIR}/lib/RecompFrontend")
# Launcher + settings menu (RecompFrontend recompui/recompinput). OFF by default: RecompFrontend
# has no license yet (N64Recomp/RecompFrontend issue #44), so binaries must not include it.
# Without it the host is CLI-only, with settings and bindings in files (README).
option(${THPS_FAMILY_UPPER}_WITH_RECOMPFRONTEND "Build the RecompFrontend launcher and settings menu (unlicensed upstream: local use only)" OFF)
if (${THPS_FAMILY_UPPER}_WITH_RECOMPFRONTEND)
    message(FATAL_ERROR "The RecompFrontend launcher is not part of the public tree (its upstream has no licence).")
endif()
set(THPS_WITH_FRONTEND ${${THPS_FAMILY_UPPER}_WITH_RECOMPFRONTEND})
if (THPS_WITH_FRONTEND AND WIN32)
    message(FATAL_ERROR "${THPS_FAMILY_UPPER}_WITH_RECOMPFRONTEND is not ported to Windows (unlicensed upstream, local macOS use only)")
endif()
if (THPS_WITH_FRONTEND)
    # Launcher/menu assets (fonts, icons; not game data, never committed): tools/fetch_ui_assets.sh.
    # Fetched at configure time when missing (<GAME>_FETCH_UI_ASSETS, curl from a pinned commit) into
    # a directory outside git (default: next to the runtime; recomp-host/ui-assets/ is gitignored too).
    set(THPS_UI_ASSETS_DIR "${RECOMP_RUNTIME_DIR}/ui-assets" CACHE PATH "recompui assets (tools/fetch_ui_assets.sh)")
    option(${THPS_FAMILY_UPPER}_FETCH_UI_ASSETS "Fetch missing UI assets (tools/fetch_ui_assets.sh) when configuring" ON)
    if (NOT EXISTS "${THPS_UI_ASSETS_DIR}/InterVariable.ttf" AND ${THPS_FAMILY_UPPER}_FETCH_UI_ASSETS)
        message(STATUS "Fetching UI assets into ${THPS_UI_ASSETS_DIR} (tools/fetch_ui_assets.sh)")
        execute_process(COMMAND sh "${THPS_SHARED_DIR}/tools/fetch_ui_assets.sh" "${THPS_UI_ASSETS_DIR}"
                        RESULT_VARIABLE fetch_result)
        if (NOT fetch_result EQUAL 0)
            message(FATAL_ERROR "tools/fetch_ui_assets.sh failed (${fetch_result})")
        endif()
    endif()
    if (NOT EXISTS "${THPS_UI_ASSETS_DIR}/InterVariable.ttf")
        message(FATAL_ERROR "${THPS_UI_ASSETS_DIR} has no UI assets: run tools/fetch_ui_assets.sh first")
    endif()
endif()

set(RSP_GEN_DIR "${THPS_GEN_DIR}/rsp")
# Run at every configure: --if-stale does nothing when the stamp matches its inputs, and regenerates an older checkout's
# stubs (which may include headers that no longer exist) instead of leaving them to fail the compile.
if (THPS_STUB_FUNCS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    message(STATUS "Stub RecompiledFuncs in ${THPS_GEN_DIR} (tools/gen_stub.py --if-stale)")
    execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${THPS_GAME_DIR}/tools/gen_stub.py"
                            --runtime "${RECOMP_RUNTIME_DIR}" --out "${THPS_GEN_DIR}" --if-stale
                    RESULT_VARIABLE stub_result)
    if (NOT stub_result EQUAL 0)
        message(FATAL_ERROR "tools/gen_stub.py failed (${stub_result})")
    endif()
endif()
# The stub and split modes have no tools/gen.py run that would refresh the version header, so it is rewritten at every
# configure (gen_versions.py leaves an unchanged file alone) and an edit of the tables re-runs the configure. Otherwise
# a header from an older checkout stays (e.g. without a newer CodeClass field, and the build fails).
if (THPS_SPLIT_GAME_LIB OR THPS_STUB_FUNCS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB thps_version_tables "${CMAKE_CURRENT_SOURCE_DIR}/versions/*.toml" "${CMAKE_CURRENT_SOURCE_DIR}/versions/*/*.toml")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${thps_version_tables}
                 "${THPS_SHARED_DIR}/../games.toml" "${THPS_SHARED_DIR}/../gen_versions.py" "${THPS_GEN_VERSIONS}")
    message(STATUS "Version header in ${THPS_GEN_DIR} (tools/gen_versions.py header)")
    file(MAKE_DIRECTORY "${THPS_GEN_DIR}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${THPS_GEN_VERSIONS}" header "${THPS_GEN_DIR}/thps_versions.h"
                    RESULT_VARIABLE hdr_result)
    if (NOT hdr_result EQUAL 0)
        message(FATAL_ERROR "tools/gen_versions.py header failed (${hdr_result})")
    endif()
endif()
if (NOT EXISTS "${THPS_GEN_DIR}/thps_versions.h")
    message(FATAL_ERROR "${THPS_GEN_DIR}/thps_versions.h is missing: run tools/gen.py first")
endif()
if (NOT THPS_SPLIT_GAME_LIB AND NOT EXISTS "${RSP_GEN_DIR}/n_aspMain.cpp")
    message(FATAL_ERROR "${RSP_GEN_DIR}/n_aspMain.cpp is missing: run tools/gen.py first (RSPRecomp)")
endif()
# Code classes whose payload was generated (tools/gen.py [CLASS ...]): one build holds as many as are there.
set(THPS_CLASSES "")
if (NOT THPS_SPLIT_GAME_LIB)
file(GLOB THPS_CLASS_INLS CONFIGURE_DEPENDS "${THPS_GEN_DIR}/*/RecompiledFuncs/recomp_overlays.inl")
foreach(inl IN LISTS THPS_CLASS_INLS)
    get_filename_component(d "${inl}" DIRECTORY)
    get_filename_component(d "${d}" DIRECTORY)
    get_filename_component(cls "${d}" NAME)
    list(APPEND THPS_CLASSES "${cls}")
endforeach()
if (NOT THPS_CLASSES)
    message(FATAL_ERROR "no generated code class under ${THPS_GEN_DIR}: run tools/gen.py first")
endif()
message(STATUS "${THPS_FAMILY_UPPER} code classes: ${THPS_CLASSES}")
endif()

add_subdirectory("${NMR_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/N64ModernRuntime")
set(RT64_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux: RT64 takes the SDL_Window itself (SDL_WINDOW_VULKAN) instead of X11 handles, as Zelda64Recomp does.
    set(RT64_SDL_WINDOW_VULKAN ON CACHE BOOL "" FORCE)
endif()
if (EMSCRIPTEN)
    add_subdirectory("${RT64_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/rt64" EXCLUDE_FROM_ALL)
    add_subdirectory("${N64RECOMP_BROWSER_DIR}/frontend" "${CMAKE_CURRENT_BINARY_DIR}/web_frontend")
    # fmt 11.1 (N64Recomp's) leans on <cstdlib> arriving through other headers, which Emscripten's libc++ doesn't do.
    if (TARGET fmt)
        target_compile_options(fmt PRIVATE -include cstdlib)
    endif()
else()
add_subdirectory("${RT64_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/rt64")
endif()
# [release packaging, Q27] Release builds link our own SDL2 (tools/build_sdl2.py, pinned 2.32.10; pass -DCMAKE_PREFIX_PATH=<its prefix>),
# never Homebrew's: Homebrew's `sdl2` is sdl2-compat, which dlopen()s libSDL3 at run time, and a user's Mac has no Homebrew.
# tools/package_split.py then copies libSDL2-2.0.0.dylib into the .app and refuses any other non-system dependency.
# Windows: ON links and ships SDL2.dll from that prefix instead of RT64's prebuilt 2.26.3 (mupen64plus-win32-deps).
option(THPS_OWN_SDL2 "Release build: our own SDL2 from tools/build_sdl2.py (macOS: refuse Homebrew's; Windows: instead of RT64's prebuilt copy)" OFF)
if (WIN32)
    # RT64 builds against its own bundled SDL2 2.26.3 (flat include/SDL.h, x64 import library) and sets the variables
    # in its own directory scope only. By default use the same files, so host and RT64 share one SDL2.dll.
    set(THPS_SDL2_DIR "${RT64_DIR}/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3")
    if (THPS_OWN_SDL2)
        # Our 2.32.10 build: the host compiles and links against it and SDL2.dll beside the exe is ours. RT64 still compiles against its
        # 2.26.3 headers and links its import library by bare name; both import libraries name SDL2.dll, and SDL2 keeps its ABI
        # backward compatible within 2.x, so RT64's imports resolve against the newer DLL at load time.
        find_package(SDL2 CONFIG REQUIRED)
        get_target_property(SDL2_INCLUDE_DIRS SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
        set(SDL2_LIBRARIES SDL2::SDL2)
        set(THPS_SDL2_DLL "$<TARGET_FILE:SDL2::SDL2>")
        message(STATUS "THPS_OWN_SDL2: SDL2 ${SDL2_VERSION} from ${SDL2_DIR}")
    else()
        set(SDL2_INCLUDE_DIRS "${THPS_SDL2_DIR}/include")
        set(SDL2_LIBRARIES "${THPS_SDL2_DIR}/lib/x64/SDL2.lib")
        set(THPS_SDL2_DLL "${THPS_SDL2_DIR}/lib/x64/SDL2.dll")
    endif()
elseif (EMSCRIPTEN)
    # Emscripten's port of SDL2 (-sUSE_SDL=2, above and in the page's link).
    set(SDL2_INCLUDE_DIRS "${EMSCRIPTEN_SYSROOT}/include/SDL2")
    set(SDL2_LIBRARIES "")
else()
    find_package(SDL2 REQUIRED)
endif()
if (THPS_OWN_SDL2 AND NOT WIN32)
    foreach(p IN LISTS SDL2_INCLUDE_DIRS SDL2_LIBRARIES SDL2_DIR)
        if (p MATCHES "^(/opt/homebrew|/usr/local)")
            message(FATAL_ERROR "THPS_OWN_SDL2: SDL2 resolved to ${p}. Build ours with tools/build_sdl2.py and pass -DCMAKE_PREFIX_PATH=<prefix>")
        endif()
    endforeach()
    message(STATUS "THPS_OWN_SDL2: SDL2 from ${SDL2_DIR}")
endif()

if (THPS_STUB_FUNCS)
    add_executable(thps_input_events_test "${THPS_SHARED_DIR}/tests/input_events_test.cpp")
    target_compile_definitions(thps_input_events_test PRIVATE SDL_MAIN_HANDLED)
    target_include_directories(thps_input_events_test PRIVATE "${THPS_SHARED_DIR}/src" ${SDL2_INCLUDE_DIRS})
    target_link_libraries(thps_input_events_test PRIVATE ${SDL2_LIBRARIES})
    add_test(NAME input_events COMMAND thps_input_events_test)
    if (WIN32)
        add_custom_command(TARGET thps_input_events_test POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${THPS_SDL2_DLL}" "$<TARGET_FILE_DIR:thps_input_events_test>"
            VERBATIM)
    endif()
endif()

if (THPS_WITH_FRONTEND)
# RecompFrontend b1a1477 (recompui launcher/menus + recompinput), as in CBFD-Recompiled's host.
# recompui compiles its shaders with rt64's build_*_shader functions, which use these variables
# from rt64's own directory scope: repeat them here.
set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH "${NMR_DIR}")
set(RECOMP_FRONTEND_RT64_PATH "${RT64_DIR}")
set(DXC_COMMON_OPTS "-I${RT64_DIR}/src")
set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
if (APPLE)
    set(DXC "DYLD_LIBRARY_PATH=${RT64_DIR}/src/contrib/dxc/lib/arm64" "${RT64_DIR}/src/contrib/dxc/bin/arm64/dxc-macos")
elseif (WIN32)
    set(DXC "${RT64_DIR}/src/contrib/dxc/bin/x64/dxc.exe")
else()
    set(DXC "LD_LIBRARY_PATH=${RT64_DIR}/src/contrib/dxc/lib/x64" "${RT64_DIR}/src/contrib/dxc/bin/x64/dxc-linux")
endif()
# recompui/src/api/ui_api_events.cpp includes "../../../../../patches/ui_funcs.h", i.e. it expects
# <root>/lib/RecompFrontend next to <root>/patches/ (the Zelda/Banjo/Conker layout): our shims.
file(COPY "${THPS_SHARED_DIR}/frontend-shim/" DESTINATION "${RECOMP_RUNTIME_DIR}/patches")
add_subdirectory("${FRONTEND_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/RecompFrontend")
endif()

# --- In-game settings menu (THPS_MENU) -----------------------------------------------------------------------
# RmlUi drawn over the game through RT64's render hooks (src/ui/): General / Controls / Graphics / Sound, opened with Esc or the pad's Back
# button. Derived from Zelda64Recomp's src/ui (GPL-3.0, see the games' NOTICE files). ON by default: it is what the released app shows, and it
# replaces the unlicensed RecompFrontend launcher. It adds ~35 s of build time and ~3.5 MB. The libraries (RmlUi, lunasvg, FreeType: sources,
# built static) and the OFL fonts are fetched by tools/setup_runtime.py into the runtime dir (the default setup does it); a configure that
# finds them missing runs `setup_runtime.py --ui-only` itself. -DTHPS_MENU=OFF builds without (CLI-only host, settings in the cfg files).
option(THPS_MENU "In-game settings menu (Esc / pad Back): RmlUi from Zelda64Recomp's UI" ON)
option(THPS_MENU_FETCH "Fetch the settings menu's libraries and fonts into the runtime dir when they are missing" ON)
if (THPS_MENU AND THPS_WITH_FRONTEND)
    message(STATUS "THPS_MENU: off (the RecompFrontend build has its own menus)")
    set(THPS_MENU OFF)
endif()
if (THPS_MENU)
    set(THPS_MENU_DEPS_DIR "${RECOMP_RUNTIME_DIR}/lib" CACHE PATH "Directory with RmlUi/, lunasvg/ and freetype/ sources (tools/setup_runtime.py)")
    set(THPS_MENU_FONTS_SRC "${RECOMP_RUNTIME_DIR}/ui-fonts" CACHE PATH "Directory with LatoLatin-*.ttf, NotoEmoji-Regular.ttf and promptfont/promptfont.ttf (SIL OFL; tools/setup_runtime.py)")
    if (THPS_MENU_FETCH AND (NOT EXISTS "${THPS_MENU_DEPS_DIR}/RmlUi/CMakeLists.txt" OR NOT EXISTS "${THPS_MENU_DEPS_DIR}/lunasvg/CMakeLists.txt"
                             OR NOT EXISTS "${THPS_MENU_DEPS_DIR}/freetype/CMakeLists.txt" OR NOT EXISTS "${THPS_MENU_FONTS_SRC}/promptfont/promptfont.ttf"))
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        message(STATUS "Fetching the settings menu's libraries and fonts into ${RECOMP_RUNTIME_DIR} (tools/setup_runtime.py --ui-only)")
        execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${THPS_SHARED_DIR}/tools/setup_runtime.py" "${RECOMP_RUNTIME_DIR}" --ui-only
                        RESULT_VARIABLE menu_fetch_result)
        if (NOT menu_fetch_result EQUAL 0)
            message(FATAL_ERROR "tools/setup_runtime.py --ui-only failed (${menu_fetch_result}). Fetch by hand, or build without the menu: -DTHPS_MENU=OFF")
        endif()
    endif()
    foreach(dep RmlUi lunasvg freetype)
        if (NOT EXISTS "${THPS_MENU_DEPS_DIR}/${dep}/CMakeLists.txt")
            message(FATAL_ERROR "THPS_MENU: ${THPS_MENU_DEPS_DIR}/${dep} is missing. Run: python3 -I tools/setup_runtime.py <runtime dir> --ui-only  (or -DTHPS_MENU=OFF)")
        endif()
    endforeach()
    if (NOT EXISTS "${THPS_MENU_FONTS_SRC}/promptfont/promptfont.ttf")
        message(FATAL_ERROR "THPS_MENU: no fonts in ${THPS_MENU_FONTS_SRC}. Run: python3 -I tools/setup_runtime.py <runtime dir> --ui-only  (or -DTHPS_MENU=OFF)")
    endif()
    # RT64's shader helpers (build_vertex_shader...) read these from RT64's own directory scope: repeat them here.
    set(DXC_COMMON_OPTS "-I${RT64_DIR}/src")
    set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
    set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
    set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
    set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
    if (APPLE)
        set(DXC "DYLD_LIBRARY_PATH=${RT64_DIR}/src/contrib/dxc/lib/arm64" "${RT64_DIR}/src/contrib/dxc/bin/arm64/dxc-macos")
    elseif (WIN32)
        set(DXC "${RT64_DIR}/src/contrib/dxc/bin/x64/dxc.exe")
    else()
        set(DXC "LD_LIBRARY_PATH=${RT64_DIR}/src/contrib/dxc/lib/x64" "${RT64_DIR}/src/contrib/dxc/bin/x64/dxc-linux")
    endif()
    # FreeType from source (static; no system or Homebrew dylib in a release bundle), under the name RmlUi looks for.
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(FT_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS_SAVED "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${THPS_MENU_DEPS_DIR}/freetype" "${CMAKE_CURRENT_BINARY_DIR}/ui/freetype" EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVED}")
    add_library(Freetype::Freetype ALIAS freetype)
    set(CMAKE_DISABLE_FIND_PACKAGE_Freetype ON)
    set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LUNASVG_BUILD_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory("${THPS_MENU_DEPS_DIR}/lunasvg" "${CMAKE_CURRENT_BINARY_DIR}/ui/lunasvg" EXCLUDE_FROM_ALL)
    set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
    set(RMLUI_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
    add_subdirectory("${THPS_MENU_DEPS_DIR}/RmlUi" "${CMAKE_CURRENT_BINARY_DIR}/ui/RmlUi" EXCLUDE_FROM_ALL)
    target_compile_definitions(rmlui_core PRIVATE LUNASVG_BUILD_STATIC)
    add_library(thps_ui STATIC
        "${THPS_SHARED_DIR}/src/ui/ui_host.cpp"
        "${THPS_SHARED_DIR}/src/ui/ui_renderer.cpp"
        "${THPS_MENU_DEPS_DIR}/RmlUi/Backends/RmlUi_Platform_SDL.cpp")
    # The shaders are RmlUi's two (Zelda64Recomp's, unchanged); RT64's helpers turn them into SPIR-V / DXIL headers (Metal comes from SPIR-V).
    # (RT64's helpers take the source relative to PROJECT_SOURCE_DIR, the game's recomp-host dir, and the output relative to the build dir.)
    file(RELATIVE_PATH THPS_MENU_SHADERS "${PROJECT_SOURCE_DIR}" "${THPS_SHARED_DIR}/ui/shaders")
    if (EMSCRIPTEN)
        # RT64 builds its shaders as WGSL on the web (RT64_WGSL), and so do its helpers here, with what it sets for its
        # own directory only.
        set(RT64_WGSL ON)
        set(RT64_PLUME_WGSL "${RT64_DIR}/src/contrib/plume/examples/cmake/modules/PlumeWGSL.cmake")
        target_compile_definitions(thps_ui PRIVATE RT64_WGSL=1)
    endif()
    build_vertex_shader(thps_ui "${THPS_MENU_SHADERS}/InterfaceVS.hlsl" "ui/shaders/InterfaceVS.hlsl")
    build_pixel_shader(thps_ui "${THPS_MENU_SHADERS}/InterfacePS.hlsl" "ui/shaders/InterfacePS.hlsl")
    target_compile_definitions(thps_ui PUBLIC THPS_MENU=1 RMLUI_SDL_VERSION_MAJOR=2 PRIVATE HLSL_CPU __PRFCHWINTRIN_H)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # As RT64 and the host executable on Linux: plume's RenderWindow is an SDL_Window* only with these (not on macOS or Windows).
        target_compile_definitions(thps_ui PRIVATE PLUME_SDL_VULKAN_ENABLED RT64_SDL_WINDOW_VULKAN)
    endif()
    target_include_directories(thps_ui PRIVATE
        "${THPS_SHARED_DIR}/src" "${THPS_GEN_DIR}"
        "${THPS_MENU_DEPS_DIR}/RmlUi/Include" "${THPS_MENU_DEPS_DIR}/RmlUi/Backends"
        "${NMR_DIR}/ultramodern/include" "${NMR_DIR}/librecomp/include" "${NMR_DIR}/N64Recomp/include" "${NMR_DIR}/thirdparty" "${NMR_DIR}/thirdparty/concurrentqueue"
        "${RT64_DIR}/include" "${RT64_DIR}/src" "${RT64_DIR}/src/contrib" "${RT64_DIR}/src/contrib/plume" "${RT64_DIR}/src/contrib/hlslpp/include"
        "${RT64_DIR}/src/rhi" "${RT64_DIR}/src/render" "${RT64_DIR}/src/common"
        "${CMAKE_BINARY_DIR}/ui/shaders")
    if (WIN32)
        target_include_directories(thps_ui PRIVATE "${RT64_DIR}/src/contrib/dxc/inc")
        target_compile_definitions(thps_ui PRIVATE NOMINMAX)
    endif()
    target_include_directories(thps_ui PRIVATE ${SDL2_INCLUDE_DIRS})
    target_compile_options(thps_ui PRIVATE -Wno-unused-parameter)
    target_link_libraries(thps_ui PUBLIC rmlui_core lunasvg Freetype::Freetype)
endif()

# --- Recompiled game code (generated, ROM-derived, never committed) ---------
# One static library per code class. Every function a class defines is renamed "<class>_<name>" by a
# force-included header (build/gen/<class>/namespace.h, tools/gen_versions.py), so the payloads of
# several ROM versions link into one executable; src/payload.cpp.in registers each with the host.
set(THPS_PAYLOAD_SOURCES "")
set(THPS_FUNCS_LIBS "")
foreach(cls IN LISTS THPS_CLASSES)
    set(cls_dir "${THPS_GEN_DIR}/${cls}")
    file(GLOB cls_funcs CONFIGURE_DEPENDS "${cls_dir}/RecompiledFuncs/funcs_*.c")
    add_library(thps_funcs_${cls} STATIC ${cls_funcs})
    target_include_directories(thps_funcs_${cls} PUBLIC "${NMR_DIR}/N64Recomp/include" "${cls_dir}/RecompiledFuncs" "${THPS_SHARED_DIR}/src")
    # Generated code: no warnings, line tables only (17 MB of C).
    if (THPS_MSVC_STYLE)
        target_compile_options(thps_funcs_${cls} PRIVATE /w "/FI${cls_dir}/namespace.h" /clang:-fno-strict-aliasing)
        set(payload_options "/FI${cls_dir}/namespace.h;/w")
    else()
        target_compile_options(thps_funcs_${cls} PRIVATE -w -gline-tables-only -fno-strict-aliasing -include "${cls_dir}/namespace.h")
        set(payload_options "-include;${cls_dir}/namespace.h;-w")
    endif()
    set(THPS_CLASS "${cls}")
    # The class's front-end overlay table (games with overlays: versions/<class>/overlay_table.inc, written by tools/gen.py), else none.
    set(THPS_OVERLAY_INCLUDE "")
    set(THPS_OVERLAY_DEPEND "")
    set(THPS_HAS_OVERLAYS 0)
    if (THPS_OVERLAYS)
        set(THPS_HAS_OVERLAYS 1)
        set(THPS_OVERLAY_TABLE "${THPS_GAME_DIR}/versions/${cls}/overlay_table.inc")
        if (NOT EXISTS "${THPS_OVERLAY_TABLE}")
            message(FATAL_ERROR "${THPS_OVERLAY_TABLE} is missing (tools/gen.py writes it)")
        endif()
        set(THPS_OVERLAY_INCLUDE "#include \"${THPS_OVERLAY_TABLE}\"")
        set(THPS_OVERLAY_DEPEND "${THPS_OVERLAY_TABLE}")
    endif()
    configure_file("${THPS_SHARED_DIR}/src/payload.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/payload_${cls}.cpp" @ONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/payload_${cls}.cpp" PROPERTIES
        COMPILE_OPTIONS "${payload_options}"
        INCLUDE_DIRECTORIES "${cls_dir}/RecompiledFuncs"
        OBJECT_DEPENDS "${THPS_OVERLAY_DEPEND}")
    list(APPEND THPS_PAYLOAD_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/payload_${cls}.cpp")
    list(APPEND THPS_FUNCS_LIBS thps_funcs_${cls})
endforeach()

# --- Host ------------------------------------------------------------------
if (THPS_SKIP_INTRO STREQUAL "overlay")
    set(THPS_SKIP_INTRO_SOURCE "${THPS_SHARED_DIR}/src/skip_intro_overlay.cpp")   # the legal screen is a front-end overlay (THPS2/3)
else()
    set(THPS_SKIP_INTRO_SOURCE "${THPS_GAME_DIR}/game/skip_intro.cpp")             # the game's own (THPS1: a vtable screen)
endif()
# The game's side of the draw distance setting (which frustum, which buffers). A game without one still builds: the setting
# is then accepted and changes nothing.
if (EXISTS "${THPS_GAME_DIR}/game/draw_distance.cpp")
    set(THPS_DRAW_DISTANCE_SOURCE "${THPS_GAME_DIR}/game/draw_distance.cpp" "${THPS_SHARED_DIR}/src/zone_ownership.cpp")   # the zone hooks, with the game's profile()
else()
    set(THPS_DRAW_DISTANCE_SOURCE "${THPS_SHARED_DIR}/src/draw_distance_none.cpp")
endif()
set(THPS_RUN_STATE_SOURCE "")
if (THPS_OVERLAYS)   # THPS2/3 (b_is_flip_in_game): the skater-in-control test the controller layer uses reads the game's run flag (versions: gRunActive)
    set(THPS_RUN_STATE_SOURCE "${THPS_SHARED_DIR}/src/run_state.cpp")
endif()
add_executable(${THPS_EXE}
    ${THPS_SHARED_DIR}/src/main.cpp
    ${THPS_SHARED_DIR}/src/versions.cpp
    ${THPS_PAYLOAD_SOURCES}
    ${THPS_SHARED_DIR}/src/hooks.cpp
    ${THPS_SHARED_DIR}/src/erz.cpp
    ${THPS_SHARED_DIR}/src/rt64_context.cpp
    ${THPS_SHARED_DIR}/src/dl_inspect.cpp
    ${THPS_SHARED_DIR}/src/border_crop.cpp
    ${THPS_SHARED_DIR}/src/fog.cpp
    ${THPS_SHARED_DIR}/src/dl_fixes.cpp
    ${THPS_SHARED_DIR}/src/ultra_stubs.cpp
    ${THPS_SHARED_DIR}/src/input.cpp
    ${THPS_SHARED_DIR}/src/ride.cpp
    ${THPS_SHARED_DIR}/src/platform.cpp
    ${THPS_SHARED_DIR}/src/audio.cpp
    ${THPS_SHARED_DIR}/src/music.cpp
    ${THPS_SHARED_DIR}/src/pak.cpp
    ${THPS_SHARED_DIR}/src/control.cpp
    ${THPS_SHARED_DIR}/src/gfx_config.cpp
    ${THPS_SHARED_DIR}/src/widescreen.cpp
    ${THPS_SHARED_DIR}/src/draw_distance.cpp
    ${THPS_DRAW_DISTANCE_SOURCE}
    ${THPS_SKIP_INTRO_SOURCE}
    ${THPS_RUN_STATE_SOURCE}
    ${THPS_GAME_EXTRA_SOURCES}
    ${THPS_SHARED_DIR}/src/crash.cpp
    ${THPS_SHARED_DIR}/src/settings.cpp
    ${THPS_SHARED_DIR}/src/cheats.cpp
    ${THPS_GAME_DIR}/game/cheats.cpp   # the game's cheats: which it has, its flags and hooks (src/cheats.cpp is the shared part)
    ${THPS_SHARED_DIR}/src/rom_check.cpp
    ${THPS_SHARED_DIR}/src/fps_counter.cpp
    ${THPS_SHARED_DIR}/src/interpolation.cpp
    ${THPS_SHARED_DIR}/src/interp_ids.cpp
    ${THPS_SHARED_DIR}/src/frametime.cpp
    ${THPS_SHARED_DIR}/src/hud_anchor.cpp
    ${THPS_SHARED_DIR}/src/latency.cpp
)
# RSPRecomp output (n_aspMain.cpp): generated, ROM-derived, no warnings. On x86-64 it inlines librecomp's rsp_vu_impl.hpp,
# whose SSE path needs SSSE3/SSE4.1 (_mm_shuffle_epi8, _mm_blendv_epi8) above the SSE2 baseline (first Windows build;
# arm64 uses sse2neon). Used wherever the file is compiled into the engine.
if (THPS_MSVC_STYLE)
    set(rsp_options "/w;/O2")
else()
    set(rsp_options "-w;-O2")
endif()
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    if (THPS_MSVC_STYLE)
        list(APPEND rsp_options "/clang:-msse4.1")
    else()
        list(APPEND rsp_options "-msse4.1")
    endif()
endif()
# Spike A: translate the game at start with LiveRecomp (sljit), no compiler at all. Needs the split engine.
option(THPS_LIVERECOMP "Split engine that can also translate the game in memory at start (--live-game): spike A, see the distribution plan" OFF)
# The RSP microcode has no JIT: the experiment links RSPRecomp's output statically (needs tools/gen.py's build/gen/rsp).
option(THPS_LIVE_STATIC_RSP "With THPS_LIVERECOMP: link the audio microcode statically (the RSP part of the game is ROM-derived C++)" OFF)
if (THPS_LIVERECOMP AND NOT THPS_SPLIT_GAME_LIB)
    message(FATAL_ERROR "THPS_LIVERECOMP needs THPS_SPLIT_GAME_LIB=ON")
endif()
set(THPS_MENU_FONTS_ARGS "")
if (THPS_MENU)
    set(THPS_MENU_FONTS_ARGS --menu-fonts "${THPS_MENU_FONTS_SRC}")   # the split build's resources/ folder (rebuilt by thps_dev_resources) carries the menu too
endif()
if (THPS_SPLIT_GAME_LIB)
    target_sources(${THPS_EXE} PRIVATE ${THPS_SHARED_DIR}/src/game_split.cpp ${THPS_SHARED_DIR}/src/game_imports_table.cpp
        ${THPS_SHARED_DIR}/src/firstrun.cpp ${THPS_SHARED_DIR}/src/firstrun_ui.cpp ${THPS_SHARED_DIR}/src/platform_ext.cpp   # first-run flow: ROM picker, build progress, errors (Q26)
        ${THPS_FIRSTRUN_DIR}/overlay_rom.cpp)   # THPS2/3: the front-end overlays, read from the user's ROM at first run (every game links it; THPS1 never calls it)
    target_compile_definitions(${THPS_EXE} PRIVATE THPS_SPLIT_GAME_LIB=1)
    # The ROM file dialog: nativefiledialog-extended, already built and linked by RT64 (zlib licence).
    target_include_directories(${THPS_EXE} PRIVATE "${RT64_DIR}/src/contrib/nativefiledialog-extended/src/include")
    target_link_libraries(${THPS_EXE} PRIVATE nfd)
    if (APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
        # The game library is compiled on the user's machine with the same minimum macOS version as the app.
        target_compile_definitions(${THPS_EXE} PRIVATE "THPS_MACOS_MIN=\"${CMAKE_OSX_DEPLOYMENT_TARGET}\"")
    endif()
    # The dev build's resources/ folder (what a release package carries in Contents/Resources: N64Recomp and RSPRecomp, the SDK the game
    # library is compiled from, the per-class recipes, NOTICE), generated next to the executable by every build so it cannot go stale
    # (tools/package_split.py --dev-resources). Needs the directory with the built N64Recomp and RSPRecomp: -DTHPS_TOOLS_DIR=<dir>, by
    # default <runtime>/../N64Recomp/build (tools/build_host.py's work dir and the project's vendor/ both have that layout), then the
    # game's build/work/N64Recomp/build: with a shared runtime elsewhere ($THPS_RUNTIME_DIR) the tools stay in build/work, and a
    # missed default left a stale resources/ (an older SDK: "built for another engine" at the first run; Windows RC3, 2026-10-17).
    set(THPS_TOOLS_DIR "" CACHE PATH "Directory with N64Recomp and RSPRecomp (split build: bundled into the dev resources/ folder)")
    set(THPS_TOOLS_DIR_DEFAULT "${THPS_TOOLS_DIR}")
    if (NOT THPS_TOOLS_DIR_DEFAULT)
        foreach(cand "${RECOMP_RUNTIME_DIR}/../N64Recomp/build" "${THPS_GAME_DIR}/build/work/N64Recomp/build")
            if (NOT THPS_TOOLS_DIR_DEFAULT AND EXISTS "${cand}")
                get_filename_component(THPS_TOOLS_DIR_DEFAULT "${cand}" ABSOLUTE)
            endif()
        endforeach()
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    # Windows: the first run compiles with the bundled llvm-mingw (<resources>/toolchain/bin/clang.exe), so the dev resources need it too.
    # Default: <runtime>/../llvm-mingw/llvm-mingw-* (`python -I tools/mingw_check.py --fetch-toolchain --work build/work`: the pinned release,
    # SHA-256 checked). Copied once (trimmed, ~470 MB); later builds keep the copy while the source directory is the same.
    set(THPS_TOOLCHAIN_ARGS "")
    if (WIN32)
        set(THPS_TOOLCHAIN_DIR "" CACHE PATH "Windows split build: llvm-mingw directory bundled as resources/toolchain (the first-run compiler)")
        set(THPS_TOOLCHAIN_DIR_DEFAULT "${THPS_TOOLCHAIN_DIR}")
        if (NOT THPS_TOOLCHAIN_DIR_DEFAULT)
            file(GLOB THPS_TOOLCHAIN_CANDIDATES LIST_DIRECTORIES true "${RECOMP_RUNTIME_DIR}/../llvm-mingw/llvm-mingw-*"
                 "${THPS_GAME_DIR}/build/work/llvm-mingw/llvm-mingw-*")
            if (THPS_TOOLCHAIN_CANDIDATES)
                list(GET THPS_TOOLCHAIN_CANDIDATES 0 THPS_TOOLCHAIN_DIR_DEFAULT)
                get_filename_component(THPS_TOOLCHAIN_DIR_DEFAULT "${THPS_TOOLCHAIN_DIR_DEFAULT}" ABSOLUTE)
            endif()
        endif()
        if (THPS_TOOLCHAIN_DIR_DEFAULT AND EXISTS "${THPS_TOOLCHAIN_DIR_DEFAULT}/bin/clang.exe")
            set(THPS_TOOLCHAIN_ARGS --toolchain "${THPS_TOOLCHAIN_DIR_DEFAULT}")
            message(STATUS "split build: bundling the first-run compiler from ${THPS_TOOLCHAIN_DIR_DEFAULT}")
        else()
            message(WARNING "split build: no llvm-mingw found (THPS_TOOLCHAIN_DIR, ${RECOMP_RUNTIME_DIR}/../llvm-mingw or build/work/llvm-mingw): the first run will say "
                            "the bundled compiler is missing. Fetch it: python -I tools/mingw_check.py --fetch-toolchain --work build/work")
        endif()
    endif()
    if (THPS_TOOLS_DIR_DEFAULT)
        add_custom_target(thps_dev_resources ALL
            COMMAND "${Python3_EXECUTABLE}" -I "${THPS_GAME_DIR}/tools/package_split.py" --dev-resources "$<TARGET_FILE_DIR:${THPS_EXE}>/resources"
                    --tools "${THPS_TOOLS_DIR_DEFAULT}" --runtime "${RECOMP_RUNTIME_DIR}" ${THPS_MENU_FONTS_ARGS} ${THPS_TOOLCHAIN_ARGS}
            COMMENT "Dev resources/ folder next to ${THPS_EXE} (tools, sdk, recipes)"
            VERBATIM)
        add_dependencies(thps_dev_resources ${THPS_EXE})
    else()
        message(WARNING "THPS_TOOLS_DIR not set, and no N64Recomp build in ${RECOMP_RUNTIME_DIR}/../N64Recomp/build or build/work/N64Recomp/build: "
                        "this split build does not refresh its resources/ folder, so an older one (an older SDK) may be left next to the executable "
                        "(pass -DTHPS_TOOLS_DIR=<dir with N64Recomp and RSPRecomp>; the first-run build needs it)")
    endif()
if (THPS_LIVERECOMP)
    # Context::from_symbol_file lives in N64Recomp's CLI config.cpp, not in the library.
    target_sources(${THPS_EXE} PRIVATE ${THPS_SHARED_DIR}/src/game_live.cpp "${NMR_DIR}/N64Recomp/src/config.cpp")
    target_compile_definitions(${THPS_EXE} PRIVATE THPS_LIVERECOMP=1)
    target_include_directories(${THPS_EXE} PRIVATE "${NMR_DIR}/N64Recomp/lib/tomlplusplus/include" "${NMR_DIR}/N64Recomp/src")
    target_link_libraries(${THPS_EXE} PRIVATE N64Recomp LiveRecomp)
    if (THPS_LIVE_STATIC_RSP)
        target_sources(${THPS_EXE} PRIVATE "${RSP_GEN_DIR}/n_aspMain.cpp")
        target_compile_definitions(${THPS_EXE} PRIVATE THPS_LIVE_STATIC_RSP=1)
        set_source_files_properties("${RSP_GEN_DIR}/n_aspMain.cpp" PROPERTIES COMPILE_OPTIONS "${rsp_options}")
    endif()
endif()
else()
    target_sources(${THPS_EXE} PRIVATE "${RSP_GEN_DIR}/n_aspMain.cpp")
    set_source_files_properties("${RSP_GEN_DIR}/n_aspMain.cpp" PROPERTIES COMPILE_OPTIONS "${rsp_options}")
endif()
target_include_directories(${THPS_EXE} PRIVATE
    "${NMR_DIR}/N64Recomp/include"
    "${NMR_DIR}/thirdparty"
    "${THPS_GEN_DIR}"
    "${THPS_SHARED_DIR}/src"
    "${THPS_FIRSTRUN_DIR}"
    "${THPS_GAME_DIR}/game"
    "${RT64_DIR}/src"
    "${RT64_DIR}/src/contrib"
    "${RT64_DIR}/src/contrib/plume"
    "${RT64_DIR}/src/contrib/imgui"
    "${RT64_DIR}/src/contrib/hlslpp/include"
    "${RT64_DIR}/src/contrib/mupen64plus-core/src/api"
    ${SDL2_INCLUDE_DIRS}
)
target_compile_definitions(${THPS_EXE} PRIVATE HLSL_CPU)
target_link_libraries(${THPS_EXE} PRIVATE ${THPS_FUNCS_LIBS} librecomp ultramodern rt64 ${SDL2_LIBRARIES})
if (WIN32)
    # RT64 links "SDL2" "SDL2main" by bare name and adds their folder with link_directories(), which is scoped to
    # RT64's own directory: the exe's link line needs the folder too (CI run 38179300742: lld-link "could not open SDL2.lib").
    target_link_directories(${THPS_EXE} PRIVATE "${THPS_SDL2_DIR}/lib/x64")
    # A GUI-subsystem exe, so a double-click opens no console window. Started from a terminal it attaches to that console
    # (platform.cpp early_init: AttachConsole), and pipes/files the parent hands it (the tests) work as before. The entry stays
    # main(): /ENTRY:mainCRTStartup instead of SDL2main's WinMain, which would need main renamed to SDL_main (SDL_MAIN_HANDLED
    # off) and would tie the exe to one of the two SDL2main.lib copies (RT64's 2.26.3 or ours). mingw's CRT maps WinMain to main.
    option(THPS_WIN_CONSOLE "Windows: build a console-subsystem exe (a console window opens with it; debugging)" OFF)
    if (NOT THPS_WIN_CONSOLE)
        set_target_properties(${THPS_EXE} PROPERTIES WIN32_EXECUTABLE ON)
        if (THPS_MSVC_STYLE)
            target_link_options(${THPS_EXE} PRIVATE "/ENTRY:mainCRTStartup")
        endif()
    endif()
endif()
if (APPLE)
    target_sources(${THPS_EXE} PRIVATE ${THPS_SHARED_DIR}/src/macos_window.mm)
    target_link_libraries(${THPS_EXE} PRIVATE "-framework AppKit" "-framework CoreFoundation")
endif()
if (WIN32)
    # The exe's own icon (Explorer, taskbar, Alt-Tab): tools/make_app_icon.py renders the game's game/icon.svg (or the procedural look game.toml
    # names) into an .ico, and a one-line resource script embeds it. Skipped with a warning if Python fails, so a missing icon never fails the build.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(thps_ico "${THPS_GEN_DIR}/app_icon.ico")
    set(thps_icon_script "${THPS_SHARED_DIR}/tools/make_app_icon.py")
    if (NOT EXISTS "${thps_ico}" OR "${thps_icon_script}" IS_NEWER_THAN "${thps_ico}" OR "${THPS_GAME_DIR}/game/game.toml" IS_NEWER_THAN "${thps_ico}"
        OR (EXISTS "${THPS_GAME_DIR}/game/icon.svg" AND "${THPS_GAME_DIR}/game/icon.svg" IS_NEWER_THAN "${thps_ico}")
        OR (EXISTS "${THPS_GAME_DIR}/game/icon-small.svg" AND "${THPS_GAME_DIR}/game/icon-small.svg" IS_NEWER_THAN "${thps_ico}"))
        message(STATUS "Drawing the app icon (tools/make_app_icon.py) into ${thps_ico}")
        file(MAKE_DIRECTORY "${THPS_GEN_DIR}")
        execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${thps_icon_script}" --game-toml "${THPS_GAME_DIR}/game/game.toml" --ico "${thps_ico}"
                        RESULT_VARIABLE thps_icon_result OUTPUT_QUIET)
    else()
        set(thps_icon_result 0)
    endif()
    if (thps_icon_result EQUAL 0 AND EXISTS "${thps_ico}")
        enable_language(RC)
        file(WRITE "${THPS_GEN_DIR}/app_icon.rc" "1 ICON \"${thps_ico}\"\n")
        target_sources(${THPS_EXE} PRIVATE "${THPS_GEN_DIR}/app_icon.rc")
    else()
        message(WARNING "tools/make_app_icon.py failed (${thps_icon_result}): the exe gets no icon")
    endif()
endif()
if (WIN32)
    # NOMINMAX as RT64 does (it sets it in its own scope; no WIN32_LEAN_AND_MEAN: RT64's dxcapi.h needs IStream);
    # _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR as RecompFrontend does, to avoid the vcredist mutex-constructor crash on
    # machines without a recent VC++ runtime . RT64 adds its DXC headers directory-wide on Windows.
    target_compile_definitions(${THPS_EXE} PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    target_include_directories(${THPS_EXE} PRIVATE "${RT64_DIR}/src/contrib/dxc/inc")
    target_link_libraries(${THPS_EXE} PRIVATE winmm shell32 ole32 user32)
    # RT64 loads dxil.dll and dxcompiler.dll at run time from the directory of the module holding its code (the exe);
    # its CMake copies them into its own binary dir only. We ship Microsoft's DXC v1.8.2407 pair, NOT RT64's contrib/dxc copies
    # (v1.7.2212: its dxil.dll licence forbids redistribution; v1.8.2407's has "Distributable Code" terms). tools/setup_runtime.py
    # (--dxc-only) downloads the release zip into <runtime>/dxc/v1.8.2407/x64 and checks the archive and both DLLs against pinned
    # SHA-256s (tools/dxc_pins.py); the pair must stay a pair (a new dxil.dll with an old dxcompiler.dll aborts at the first shader
    # link). The build-time dxc (headers, dxc.exe, set above) stays RT64's contrib/dxc: it only compiles the shaders that are embedded
    # as DXIL blobs, and D3D12 accepts them whichever dxil.dll signed them (tested on Windows 2026-10-13: an engine built with the contrib dxc, run beside the v1.8.2407 pair, passed THPS1's full suite 40/0/4).
    # SDL2.dll: RT64's 2.26.3, or ours with THPS_OWN_SDL2 (above).
    set(THPS_DXC_DIR "${RECOMP_RUNTIME_DIR}/dxc/v1.8.2407/x64" CACHE PATH "Directory with DXC v1.8.2407's dxil.dll and dxcompiler.dll (tools/setup_runtime.py --dxc-only)")
    if (NOT EXISTS "${THPS_DXC_DIR}/dxil.dll" OR NOT EXISTS "${THPS_DXC_DIR}/dxcompiler.dll")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        message(STATUS "Fetching DXC v1.8.2407's dxil.dll and dxcompiler.dll into ${RECOMP_RUNTIME_DIR} (tools/setup_runtime.py --dxc-only)")
        execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${THPS_SHARED_DIR}/tools/setup_runtime.py" "${RECOMP_RUNTIME_DIR}" --dxc-only
                        RESULT_VARIABLE dxc_fetch_result)
        if (NOT dxc_fetch_result EQUAL 0)
            message(FATAL_ERROR "tools/setup_runtime.py --dxc-only failed (${dxc_fetch_result}). Fetch by hand: python3 -I tools/setup_runtime.py <runtime dir> --dxc-only")
        endif()
    endif()
    # Refuse a pair that is not the pinned one (e.g. a THPS_DXC_DIR pointed at RT64's contrib/dxc/bin/x64).
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${THPS_SHARED_DIR}/tools/dxc_pins.py" verify "${THPS_DXC_DIR}"
                    RESULT_VARIABLE dxc_verify_result)
    if (NOT dxc_verify_result EQUAL 0)
        message(FATAL_ERROR "THPS_DXC_DIR (${THPS_DXC_DIR}): dxil.dll and dxcompiler.dll are not the pinned DXC v1.8.2407 pair (see above)")
    endif()
    add_custom_command(TARGET ${THPS_EXE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${THPS_DXC_DIR}/dxil.dll" "${THPS_DXC_DIR}/dxcompiler.dll"
                "${THPS_SDL2_DLL}" "$<TARGET_FILE_DIR:${THPS_EXE}>")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # crash.cpp names the recompiled functions with dladdr(): export them from the executable.
    set_target_properties(${THPS_EXE} PROPERTIES ENABLE_EXPORTS ON)
    # Same definitions RT64 compiles with (it sets them in its own directory scope): plume's RenderWindow is an SDL_Window*
    # only with PLUME_SDL_VULKAN_ENABLED, and RT64::Application::Core embeds it, so host and library must agree.
    target_compile_definitions(${THPS_EXE} PRIVATE PLUME_SDL_VULKAN_ENABLED RT64_SDL_WINDOW_VULKAN)
    find_package(Threads REQUIRED)
    target_link_libraries(${THPS_EXE} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
if (THPS_MENU)
    target_link_libraries(${THPS_EXE} PRIVATE thps_ui)
    target_compile_definitions(${THPS_EXE} PRIVATE THPS_MENU=1)
    # Menu documents, styles and OFL fonts, staged next to the executable as <exe>/resources/ui (a bare build; the engine looks for
    # <resources>/ui, platform.cpp resource_dir). tools/package_split.py puts the same files into the release package / dev resources.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB_RECURSE THPS_MENU_FILES CONFIGURE_DEPENDS "${THPS_SHARED_DIR}/ui/menu/*")
    if (NOT EMSCRIPTEN)
    add_custom_target(thps_menu_assets ALL
        COMMAND "${Python3_EXECUTABLE}" -I "${THPS_SHARED_DIR}/tools/menu_assets.py" "$<TARGET_FILE_DIR:${THPS_EXE}>/resources/ui" "${THPS_MENU_FONTS_SRC}"
        DEPENDS ${THPS_MENU_FILES} "${THPS_SHARED_DIR}/ui/FONTS-LICENSE.txt"
        COMMENT "Staging the in-game menu assets"
        VERBATIM)
    add_dependencies(thps_menu_assets ${THPS_EXE})
    if (TARGET thps_dev_resources)
        add_dependencies(thps_menu_assets thps_dev_resources)   # it replaces resources/ wholesale (and stages the menu itself): run after it
    endif()
    endif()
endif()
if (THPS_WITH_FRONTEND)
    target_sources(${THPS_EXE} PRIVATE ${THPS_SHARED_DIR}/src/frontend.cpp)
    target_compile_definitions(${THPS_EXE} PRIVATE THPS_FRONTEND=1)
    target_link_libraries(${THPS_EXE} PRIVATE recompui recompinput nfd)
    # recompui loads assets/ from the program's resource directory (the executable's directory
    # for a bare executable, Contents/Resources in an .app).
    add_custom_command(TARGET ${THPS_EXE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${THPS_UI_ASSETS_DIR}" "$<TARGET_FILE_DIR:${THPS_EXE}>/assets")
endif()

if (EMSCRIPTEN)
    # The page (lib/N64Recomp-browser/frontend): <exe>.html, .js, .wasm and .data, with coi_serviceworker.js beside it.
    # The menu's documents, styles and fonts go into its .data at /resources/ui, where resource_dir() finds them; no ROM
    # data: the player picks their dump on the page (main.cpp), and it stays in the browser's storage.
    target_link_libraries(${THPS_EXE} PRIVATE n64recomp_browser_file_picker)
    n64recomp_browser_frontend_page(${THPS_EXE})
    if (THPS_MENU)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(THPS_WEB_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/web-resources")
        add_custom_command(OUTPUT "${THPS_WEB_RESOURCES}/ui.stamp"
            COMMAND "${Python3_EXECUTABLE}" -I "${THPS_SHARED_DIR}/tools/menu_assets.py" "${THPS_WEB_RESOURCES}/ui" "${THPS_MENU_FONTS_SRC}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${THPS_WEB_RESOURCES}/ui.stamp"
            DEPENDS ${THPS_MENU_FILES} "${THPS_SHARED_DIR}/ui/FONTS-LICENSE.txt" "${THPS_SHARED_DIR}/tools/menu_assets.py"
            COMMENT "Staging the in-game menu assets for the page"
            VERBATIM)
        add_custom_target(thps_menu_assets_web DEPENDS "${THPS_WEB_RESOURCES}/ui.stamp")
        add_dependencies(${THPS_EXE} thps_menu_assets_web)
        target_link_options(${THPS_EXE} PRIVATE "SHELL:--preload-file ${THPS_WEB_RESOURCES}/ui@/resources/ui")
        set_property(TARGET ${THPS_EXE} APPEND PROPERTY LINK_DEPENDS "${THPS_WEB_RESOURCES}/ui.stamp")
    endif()
else()
# ROM-free unit check of the native ERZ decoder against the user's ROM (prints CRC results only).
add_executable(erz_selftest ${THPS_SHARED_DIR}/tools/erz_selftest.cpp ${THPS_SHARED_DIR}/src/erz.cpp)
endif()
