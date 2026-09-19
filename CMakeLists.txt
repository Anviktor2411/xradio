cmake_minimum_required(VERSION 3.16)
project(XRadio C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Universal binary on macOS so one build runs on both Intel and Apple Silicon.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

# --- options ----------------------------------------------------------------
# X-Plane SDK: the folder containing CHeaders/ and Libraries/.
# XPMP2 ships a complete SDK, so by default we just use that one.
set(XPLANE_SDK "${CMAKE_CURRENT_SOURCE_DIR}/lib/XPMP2/lib/SDK"
    CACHE PATH "Path to the X-Plane SDK")
option(XRADIO_USE_XPMP2   "Render other pilots as CSL models using XPMP2"   ON)
option(XRADIO_USE_VOICE   "Voice over the radio (Opus + miniaudio)"        ON)
option(XRADIO_BUILD_TESTS "Build the test harness (needs no X-Plane)"      OFF)

if(NOT EXISTS "${XPLANE_SDK}/CHeaders/XPLM/XPLMPlugin.h")
    message(FATAL_ERROR
        "X-Plane SDK not found at '${XPLANE_SDK}'.\n"
        "Run tools/fetch_deps.sh (or fetch_deps.ps1), or configure with "
        "-DXPLANE_SDK=<path to the SDK folder>.")
endif()

# --- dependencies -----------------------------------------------------------
if(XRADIO_USE_XPMP2)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/XPMP2/CMakeLists.txt")
        message(FATAL_ERROR
            "XPMP2 not found in lib/XPMP2.\n"
            "Run tools/fetch_deps.sh (or fetch_deps.ps1), or configure with "
            "-DXRADIO_USE_XPMP2=OFF to build without 3D traffic.")
    endif()
    # XPMP2 finds the SDK through this cache variable.
    set(XPlaneSDK_DIR "${XPLANE_SDK}" CACHE PATH "" FORCE)
    add_subdirectory(lib/XPMP2 EXCLUDE_FROM_ALL)
endif()

if(XRADIO_USE_VOICE)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/opus/CMakeLists.txt" OR
       NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/miniaudio/miniaudio.h")
        message(FATAL_ERROR
            "Opus and/or miniaudio not found in lib/.\n"
            "Run tools/fetch_deps.sh (or fetch_deps.ps1), or configure with "
            "-DXRADIO_USE_VOICE=OFF to build without voice.")
    endif()
    # Just the static library, nothing else Opus can build.
    set(OPUS_BUILD_SHARED_LIBRARY        OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS              OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING               OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE   OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    if(APPLE)
        # CPU-specific intrinsics and a universal (arm64 + x86_64) build do not
        # mix; the plain C paths are more than fast enough for a few voices.
        set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(lib/opus EXCLUDE_FROM_ALL)
endif()

# --- common settings for anything that compiles the plugin sources ----------
set(XR_PLUGIN_SOURCES
    plugin/src/main.cpp
    plugin/src/net.cpp
    plugin/src/server.cpp
    plugin/src/ui.cpp
    plugin/src/upnp.cpp
    plugin/src/voice.cpp
    plugin/src/xpmp_bridge.cpp
)

# The same relay server as a standalone program, for a machine that should
# stay up without X-Plane running -- and so tools/test_server_parity.py can
# hold it to the same behaviour as the Python one. Needs no X-Plane SDK.
add_executable(xradio_server tools/server_main.cpp plugin/src/server.cpp
                             plugin/src/net.cpp plugin/src/upnp.cpp)
target_include_directories(xradio_server PRIVATE plugin/src)
find_package(Threads REQUIRED)
target_link_libraries(xradio_server PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(xradio_server PRIVATE wsock32 ws2_32 iphlpapi)
    target_compile_definitions(xradio_server PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# Applies SDK feature level, platform defines and the optional libraries.
function(xradio_configure_target tgt)
    target_include_directories(${tgt} PRIVATE
        "${XPLANE_SDK}/CHeaders/XPLM"
        "${XPLANE_SDK}/CHeaders/Widgets"
        plugin/src)
    # SDK feature levels. XPLM400 == X-Plane 12 APIs.
    target_compile_definitions(${tgt} PRIVATE
        XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1)
    if(WIN32)
        target_compile_definitions(${tgt} PRIVATE IBM=1 _CRT_SECURE_NO_WARNINGS NOMINMAX)
    elseif(APPLE)
        target_compile_definitions(${tgt} PRIVATE APL=1)
    else()
        target_compile_definitions(${tgt} PRIVATE LIN=1)
    endif()

    if(XRADIO_USE_VOICE)
        target_link_libraries(${tgt} PRIVATE opus)
        target_include_directories(${tgt} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib/miniaudio")
        target_compile_definitions(${tgt} PRIVATE XRADIO_USE_VOICE=1)
        # miniaudio loads its backends (WASAPI, CoreAudio, ALSA/Pulse) at run
        # time, so nothing platform-specific is linked here.
        if(UNIX AND NOT APPLE)
            target_link_libraries(${tgt} PRIVATE m)
        endif()
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(${tgt} PRIVATE Threads::Threads)
    if(UNIX AND NOT APPLE)
        target_link_libraries(${tgt} PRIVATE dl)
    endif()
endfunction()

# --- the plugin -------------------------------------------------------------
add_library(XRadio SHARED ${XR_PLUGIN_SOURCES})
xradio_configure_target(XRadio)

if(XRADIO_USE_XPMP2)
    target_link_libraries(XRadio PRIVATE XPMP2)
    target_compile_definitions(XRadio PRIVATE XRADIO_USE_XPMP2=1)
endif()

# X-Plane's SDK 3.0 layout looks for <plugin folder>/<platform>_x64/<plugin
# folder>.xpl -- the binary is named after the plugin, NOT win.xpl. Get this
# wrong and X-Plane skips the folder silently, with nothing in Log.txt.
set_target_properties(XRadio PROPERTIES
    PREFIX "" SUFFIX ".xpl" OUTPUT_NAME "XRadio")

if(WIN32)
    find_library(XPLM_LIBRARY NAMES XPLM_64.lib XPLM
                 PATHS "${XPLANE_SDK}/Libraries/Win" REQUIRED)
    target_link_libraries(XRadio PRIVATE ${XPLM_LIBRARY} wsock32 ws2_32 iphlpapi)
    set(XR_PLATDIR "win_x64")

elseif(APPLE)
    target_compile_options(XRadio PRIVATE -fvisibility=hidden)
    find_library(XPLM_LIBRARY XPLM PATHS "${XPLANE_SDK}/Libraries/Mac" REQUIRED)
    find_library(IOKIT_LIBRARY IOKit REQUIRED)
    find_library(CORE_FOUNDATION_LIBRARY CoreFoundation REQUIRED)
    target_link_libraries(XRadio PRIVATE
        ${XPLM_LIBRARY} ${IOKIT_LIBRARY} ${CORE_FOUNDATION_LIBRARY})
    # X-Plane loads .xpl bundles; -undefined error keeps link errors honest.
    target_link_options(XRadio PRIVATE -undefined error)
    set(XR_PLATDIR "mac_x64")

else()   # Linux
    target_compile_options(XRadio PRIVATE -fvisibility=hidden)
    # Do NOT add -shared here: CMake already passes it for a SHARED library,
    # and a second one can push the link into producing an executable
    # ("cannot find entry symbol _start").
    #
    # Do NOT use -nodefaultlibs either. The old X-Plane sample flags assume a
    # C plugin; with C++ it drops libstdc++ and libc from the link and every
    # std::string and strcpy reference comes back undefined. Statically
    # linking the C++ runtime instead also means the plugin does not care
    # which libstdc++ the host happens to ship.
    target_link_options(XRadio PRIVATE -rdynamic -static-libgcc -static-libstdc++)
    # XPLM symbols stay unresolved on purpose: X-Plane supplies them at load
    # time, so --no-undefined must not be used on this platform.
    set(XR_PLATDIR "lin_x64")
endif()

# --- output layout ----------------------------------------------------------
# X-Plane expects:  Resources/plugins/XRadio/<platform>_x64/XRadio.xpl
set(XR_OUTDIR "${CMAKE_BINARY_DIR}/XRadio/${XR_PLATDIR}")
set_target_properties(XRadio PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${XR_OUTDIR}"
    RUNTIME_OUTPUT_DIRECTORY "${XR_OUTDIR}")
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${cfg} CFG)
    set_target_properties(XRadio PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${CFG} "${XR_OUTDIR}"
        RUNTIME_OUTPUT_DIRECTORY_${CFG} "${XR_OUTDIR}")
endforeach()

# XPMP2 needs related.txt / Doc8643.txt / MapIcons.png next to the plugin.
if(XRADIO_USE_XPMP2)
    add_custom_command(TARGET XRadio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/lib/XPMP2/Resources"
                "${CMAKE_BINARY_DIR}/XRadio/Resources"
        COMMENT "Copying XPMP2 resources into XRadio/Resources")
endif()

message(STATUS "XRadio -> ${XR_OUTDIR}  (XPMP2: ${XRADIO_USE_XPMP2}, voice: ${XRADIO_USE_VOICE})")

# --- tests ------------------------------------------------------------------
# The real plugin code linked against a stub X-Plane (tools/fake_sdk) so it can
# run as an ordinary executable: against a real server, against a hostile
# server, under several monitor layouts, and through the voice codec. Configure
# with -DXRADIO_BUILD_TESTS=ON -DXRADIO_USE_XPMP2=OFF -DXPLANE_SDK=tools/fake_sdk
if(XRADIO_BUILD_TESTS)
    if(XRADIO_USE_XPMP2)
        message(FATAL_ERROR "XRADIO_BUILD_TESTS needs -DXRADIO_USE_XPMP2=OFF (the stub SDK has no XPMP2)")
    endif()

    function(xradio_test_exe name)
        add_executable(${name} ${ARGN} ${XR_PLUGIN_SOURCES} tools/harness/xplm_stub.cpp)
        xradio_configure_target(${name})
        target_include_directories(${name} PRIVATE tools/harness)
        # No audio hardware on a CI runner: miniaudio's null backend keeps the
        # capture/playback threads running so the whole pipeline is exercised.
        target_compile_definitions(${name} PRIVATE XRADIO_NULL_AUDIO=1)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${name} PRIVATE -g -O1 -fsanitize=address,undefined
                                                    -fno-omit-frame-pointer)
            target_link_options(${name}    PRIVATE -fsanitize=address,undefined)
        endif()
    endfunction()

    # The hosted server now listens on a home machine's port, so it gets the
    # sanitizers too: tools/harness/fuzz_client.py attacks this binary.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(xradio_server PRIVATE -g -O1 -fsanitize=address,undefined
                                                     -fno-omit-frame-pointer)
        target_link_options(xradio_server    PRIVATE -fsanitize=address,undefined)
    endif()

    xradio_test_exe(plugin_harness tools/harness/main.cpp)
    xradio_test_exe(placement_test tools/harness/placement_test.cpp)
    xradio_test_exe(voice_test     tools/harness/voice_test.cpp)
    xradio_test_exe(settings_test  tools/harness/settings_test.cpp)
    xradio_test_exe(hosting_test   tools/harness/hosting_test.cpp)

    # Pure maths, no plugin sources needed.
    add_executable(smoothing_test tools/harness/smoothing_test.cpp)
    target_include_directories(smoothing_test PRIVATE plugin/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(smoothing_test PRIVATE -g -O1 -fsanitize=address,undefined)
        target_link_options(smoothing_test    PRIVATE -fsanitize=address,undefined)
    endif()
endif()
