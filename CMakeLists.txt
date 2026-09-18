cmake_minimum_required(VERSION 3.16)
project(XRadio CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Universal binary on macOS so one build runs on both Intel and Apple Silicon.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

# --- dependencies -----------------------------------------------------------
# X-Plane SDK: the folder containing CHeaders/ and Libraries/.
# XPMP2 ships a complete SDK, so by default we just use that one.
set(XPLANE_SDK "${CMAKE_CURRENT_SOURCE_DIR}/lib/XPMP2/lib/SDK"
    CACHE PATH "Path to the X-Plane SDK")
option(XRADIO_USE_XPMP2 "Render other pilots as CSL models using XPMP2" ON)

if(NOT EXISTS "${XPLANE_SDK}/CHeaders/XPLM/XPLMPlugin.h")
    message(FATAL_ERROR
        "X-Plane SDK not found at '${XPLANE_SDK}'.\n"
        "Run tools/fetch_deps.sh (or fetch_deps.ps1), or configure with "
        "-DXPLANE_SDK=<path to the SDK folder>.")
endif()

set(XR_SOURCES
    plugin/src/main.cpp
    plugin/src/net.cpp
    plugin/src/xpmp_bridge.cpp
)

add_library(XRadio SHARED ${XR_SOURCES})

target_include_directories(XRadio PRIVATE
    "${XPLANE_SDK}/CHeaders/XPLM"
    "${XPLANE_SDK}/CHeaders/Widgets"
    plugin/src
)

# SDK feature levels. XPLM400 == X-Plane 12 APIs.
target_compile_definitions(XRadio PRIVATE
    XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1)

# --- XPMP2 (CSL models, TCAS, instancing) -----------------------------------
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
    target_link_libraries(XRadio PRIVATE XPMP2)
    target_compile_definitions(XRadio PRIVATE XRADIO_USE_XPMP2=1)
endif()

# --- per-platform -----------------------------------------------------------
# X-Plane's SDK 3.0 layout looks for <plugin folder>/<platform>_x64/<plugin
# folder>.xpl -- the binary is named after the plugin, NOT win.xpl. Get this
# wrong and X-Plane skips the folder silently, with nothing in Log.txt.
set_target_properties(XRadio PROPERTIES
    PREFIX "" SUFFIX ".xpl" OUTPUT_NAME "XRadio")

if(WIN32)
    target_compile_definitions(XRadio PRIVATE IBM=1 _CRT_SECURE_NO_WARNINGS NOMINMAX)
    find_library(XPLM_LIBRARY NAMES XPLM_64.lib XPLM
                 PATHS "${XPLANE_SDK}/Libraries/Win" REQUIRED)
    target_link_libraries(XRadio PRIVATE ${XPLM_LIBRARY} wsock32 ws2_32 iphlpapi)
    set(XR_PLATDIR "win_x64")

elseif(APPLE)
    target_compile_definitions(XRadio PRIVATE APL=1)
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
    find_package(Threads REQUIRED)
    target_compile_definitions(XRadio PRIVATE LIN=1)
    target_compile_options(XRadio PRIVATE -fvisibility=hidden)
    target_link_libraries(XRadio PRIVATE dl Threads::Threads)
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
# X-Plane expects:  Resources/plugins/XRadio/<platform>_x64/<platform>.xpl
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

message(STATUS "XRadio -> ${XR_OUTDIR}  (XPMP2: ${XRADIO_USE_XPMP2})")
