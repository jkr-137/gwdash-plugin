cmake_minimum_required(VERSION 3.29)

if(NOT CMAKE_GENERATOR MATCHES "Ninja|Makefile")
    set(CMAKE_GENERATOR_PLATFORM win32)
endif()

project(gwdash-plugin LANGUAGES CXX)

# Guild Wars is a 32-bit process, so a plugin DLL has to be 32-bit too.
if(NOT (CMAKE_SIZEOF_VOID_P EQUAL 4))
    message(FATAL_ERROR "This is a 32-bit-only project. Configure with `-A Win32`.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "Only the MSVC toolchain is supported (Toolbox requires toolset v143 or newer).")
endif()

if(MSVC_TOOLSET_VERSION LESS 143)
    message(FATAL_ERROR "Need Visual Studio 2022 toolset (v143) or newer; got ${MSVC_TOOLSET_VERSION}.")
endif()

# Set by CI from the release tag; keep in sync with the default for local builds.
if(NOT DEFINED GWDASH_VERSION)
    set(GWDASH_VERSION "0.1.0")
endif()

set(GWDASH_UPDATE_REPO "jkr-137/gwdash-plugin" CACHE STRING "GitHub repo the in-game updater checks")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT, matching Toolbox: nothing that owns CRT memory may cross the
# plugin/host boundary, and the ToolboxPlugin vtable is built so nothing does.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin/")

add_compile_definitions(
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    # glaze's float writer reaches for an x64-only intrinsic when it detects SSE.
    ZMIJ_USE_SIMD=0
)

add_compile_options(/permissive- /constexpr:depth2048 /constexpr:steps10000000 /templateDepth:2048 /bigobj /MP)

include(cmake/ToolboxSdk.cmake)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h" @ONLY)

# The payload. Lives in plugins/GWDash/ and is what auto-update replaces.
add_library(GWDashCore SHARED
    "src/GWDashPlugin.cpp"
    "src/GWDashPlugin.h"
    "src/Http.cpp"
    "src/Http.h"
    "src/Paths.cpp"
    "src/Paths.h"
    "src/PriceClient.cpp"
    "src/PriceClient.h"
    "src/PriceSnapshot.h"
    "src/Updater.cpp"
    "src/Updater.h")
target_include_directories(GWDashCore PRIVATE "src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(GWDashCore PRIVATE winhttp bcrypt)
set_target_properties(GWDashCore PROPERTIES OUTPUT_NAME "GWDash.core")
gwdash_add_plugin_dll(GWDashCore)

# The loader stub Toolbox actually sees. Its filename must never change: Toolbox
# remembers enabled plugins by filename, so renaming it would silently disable
# the plugin for existing users.
add_library(GWDashLoader SHARED
    "loader/Loader.cpp")
target_include_directories(GWDashLoader PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_target_properties(GWDashLoader PROPERTIES OUTPUT_NAME "GWDash")
gwdash_add_plugin_dll(GWDashLoader)
