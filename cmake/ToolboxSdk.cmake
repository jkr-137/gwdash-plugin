include_guard()
include(FetchContent)

# Pulls the GWToolbox++ tree and builds the pieces a plugin needs out of it.
#
# Why not just add_subdirectory() the Toolbox project like the community plugin
# templates do: that configures GWToolboxdll, Core, RestClient and GWToolbox as
# well, which drags in every vcpkg port Toolbox uses (directxtex,
# discord-game-sdk, recastnavigation, uwebsockets, ...) and builds the whole
# host DLL just to link a few hundred KB of plugin base code. We instead reuse
# Toolbox's own imgui module (so struct layouts match the host exactly) and
# compile the plugin base sources directly.
#
# Toolbox does not promise ABI stability between releases, so the tag is pinned
# and bumped deliberately. See .github/workflows/build.yml for the scheduled
# build that flags upstream breakage.
set(GWDASH_TOOLBOX_TAG "8.32_Release" CACHE STRING "GWToolbox++ git tag to build the plugin against")

FetchContent_Declare(gwtoolbox
    GIT_REPOSITORY https://github.com/gwdevhub/GWToolboxpp.git
    GIT_TAG ${GWDASH_TOOLBOX_TAG}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)

# Populate only - do not add_subdirectory() the Toolbox project. Configuring it
# would pull every vcpkg port Toolbox uses and build the host DLL just so we can
# reuse a few hundred KB of plugin-base sources.
#
# FetchContent_Populate is the right tool for "download the sources, leave the
# rest to us". CMP0169 (CMake 3.30) soft-deprecates it in favour of
# MakeAvailable; we deliberately do not MakeAvailable, so silence the warning.
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(gwtoolbox)
if(NOT gwtoolbox_POPULATED)
    FetchContent_Populate(gwtoolbox)
endif()

if(NOT EXISTS "${gwtoolbox_SOURCE_DIR}/plugins/Base/ToolboxPlugin.h")
    message(FATAL_ERROR "GWToolbox++ checkout at ${gwtoolbox_SOURCE_DIR} looks incomplete")
endif()

# Toolbox's imgui: pinned v1.92.7-docking, its transparent-viewport patch, and
# GWToolboxdll/imconfig.h. All three matter - imconfig.h changes ImDrawVert and
# ImTextureID, so an ImGui built without it disagrees with the host about struct
# layouts and corrupts memory while sharing the host's context.
list(APPEND CMAKE_MODULE_PATH "${gwtoolbox_SOURCE_DIR}/cmake")
include(imgui)

find_package(glaze CONFIG REQUIRED)

# gwca.dll ships prebuilt in the Toolbox tree and is already loaded into the
# process by Toolbox itself. Declared here rather than via Toolbox's
# cmake/gwca.cmake because that one links minhook, which we have no use for (we
# install no hooks) and which would pull another vcpkg port in.
add_library(gwca SHARED IMPORTED GLOBAL)
set_target_properties(gwca PROPERTIES
    IMPORTED_IMPLIB "${gwtoolbox_SOURCE_DIR}/Dependencies/GWCA/lib/gwca.lib"
    IMPORTED_LOCATION "${gwtoolbox_SOURCE_DIR}/Dependencies/GWCA/bin/gwca.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${gwtoolbox_SOURCE_DIR}/Dependencies/GWCA/include")

# Mirrors Toolbox's own `plugin_base` target (cmake/gwtoolboxdll_plugins.cmake)
# minus the GWToolboxdll link, which upstream only needs for FontLoader::GetFont.
add_library(gwdash_plugin_base INTERFACE)

target_sources(gwdash_plugin_base INTERFACE
    "${gwtoolbox_SOURCE_DIR}/plugins/Base/dllmain.cpp"
    "${gwtoolbox_SOURCE_DIR}/plugins/Base/ToolboxPlugin.cpp"
    "${gwtoolbox_SOURCE_DIR}/plugins/Base/PluginUtils.cpp"
    "${gwtoolbox_SOURCE_DIR}/plugins/Base/ToolboxUIPlugin.cpp"
    # Plugins get their own copy of these; GWToolboxdll does not export them.
    "${gwtoolbox_SOURCE_DIR}/GWToolboxdll/ToolboxIni.cpp"
    "${gwtoolbox_SOURCE_DIR}/GWToolboxdll/Utils/SettingsDoc.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../compat/logger_stub.cpp")

target_include_directories(gwdash_plugin_base INTERFACE
    # Must come first: supplies the gwtoolboxdll_export.h that Toolbox generates
    # into its own build tree.
    "${CMAKE_CURRENT_LIST_DIR}/../compat"
    "${gwtoolbox_SOURCE_DIR}/plugins/Base"
    "${gwtoolbox_SOURCE_DIR}/GWToolboxdll")

target_link_libraries(gwdash_plugin_base INTERFACE imgui glaze::glaze gwca)

target_compile_definitions(gwdash_plugin_base INTERFACE BUILD_DLL)

# /wd4201 nameless struct/union, /wd4505 unreferenced local function: both come
# from the Toolbox and ImGui headers. No /WX here (unlike upstream) so a warning
# in a newer Toolbox release does not break our build outright.
target_compile_options(gwdash_plugin_base INTERFACE /W4 /wd4201 /wd4505 /Gy)
target_link_options(gwdash_plugin_base INTERFACE /OPT:REF /OPT:ICF /SAFESEH:NO)

function(gwdash_add_plugin_dll target)
    target_link_libraries(${target} PRIVATE gwdash_plugin_base)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/Od>)
    target_link_options(${target} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO>)
    set_target_properties(${target} PROPERTIES
        FOLDER "plugins/"
        LINK_FLAGS_DEBUG "/IGNORE:4098 /OPT:NOREF /OPT:NOICF")
endfunction()
