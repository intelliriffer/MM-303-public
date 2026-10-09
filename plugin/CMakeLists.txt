cmake_minimum_required(VERSION 3.15)
if(WIN32)
    # Windows SDK for Windows 7 and up
    set(CMAKE_SYSTEM_VERSION 7.1 CACHE STRING INTERNAL FORCE)
    add_compile_definitions(__amd64__=1)
endif()

project(MM303 VERSION 0.12.3)


### Dependency versions ###

set(LIB_JUCE_TAG "8.0.5")
set(LIB_CLAP_TAG "a3227c52fa082d2df33f4377ed1e050dd8a52595")

### Gui setup
# check if the GUI variable is set
if(NOT DEFINED GUI)
    # no GUI set defaults to midilabv2
    set(GUI "midilabv2")
endif()
set(GUI_THEME_HEADER "gui/${GUI}/Gui.h")
set(GUI_THEME ${GUI})

# Add the GUI_THEME_HEADER macro definition
add_definitions(-DGUI_THEME_HEADER="${GUI_THEME_HEADER}")

# JUCE's bundled opus requires this under -Ofast/-ffast-math (NaN bit-trick).
add_definitions(-DFLOAT_APPROX=1)

# Keep the C++ runtime inside the plugin: only glibc stays dynamic, so the
# binary cannot exceed the host's libstdc++ (e.g. GLIBCXX_3.4.29).
add_link_options(-static-libstdc++ -static-libgcc)

### IDE Generator pre-config ###

# Xcode: Disable automatic build scheme generation globally.
#        Instead, we explicitely specify which targets require schemes.
set(CMAKE_XCODE_GENERATE_SCHEME OFF)

# IDEs:  Enable grouping of source files into folders in IDEs.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# IDEs:  Create a folder in the IDE with the JUCE Module code.
option(JUCE_ENABLE_MODULE_SOURCE_GROUPS "Show all module sources in IDE projects" ON)


### Dependency management ###

include(FetchContent)

# Keep dependencies outside of the "Build" directory.
# This allows to do a clean build of the project without re-downloading or
# rebuilding the dependencies.
# Overridable per-arch (e.g. -DMM303_DEPS_DIR=.../lib-aarch64) so armhf and
# aarch64 builds don't share a JUCE-Build tree (arch-specific CMake cache).
if(NOT DEFINED MM303_DEPS_DIR)
    set(MM303_DEPS_DIR "${PROJECT_SOURCE_DIR}/lib")
endif()
set(FETCHCONTENT_BASE_DIR "${MM303_DEPS_DIR}" CACHE PATH "External dependencies path." FORCE)

FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${LIB_JUCE_TAG}
    GIT_SHALLOW    TRUE
    GIT_CONFIG     advice.detachedHead=false
    SOURCE_DIR     "${FETCHCONTENT_BASE_DIR}/JUCE"
    SUBBUILD_DIR   "${FETCHCONTENT_BASE_DIR}/JUCE-Subbuild"
    BINARY_DIR     "${FETCHCONTENT_BASE_DIR}/JUCE-Build")

FetchContent_Declare(juce-clap-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        ${LIB_CLAP_TAG}
    GIT_CONFIG     advice.detachedHead=false)

FetchContent_MakeAvailable(juce juce-clap-extensions)

# gui resources BinaryData via CMakeLists.txt
add_subdirectory(src/gui/${GUI})

### Plugin Project Setup ###

# VST2 build: point JUCE at our own hand-written ABI declarations
# (templates/vst2-abi). Setup must happen before `juce_add_plugin`.
juce_set_vst2_sdk_path("${PROJECT_SOURCE_DIR}/../templates/vst2-abi")
set(PLUGIN_FORMATS AU VST VST3 LV2)

# `juce_add_plugin` adds a static library target with the name passed as the first argument.
# This target is a normal CMake target, but has a lot of extra properties set
# up by default. As well as this shared code static library, this function adds targets for each of
# the formats specified by the FORMATS arguments. This function accepts many optional arguments.
# Check the readme at `docs/CMake API.md` in the JUCE repo for the full list.

juce_add_plugin("${PROJECT_NAME}"
    FORMATS ${PLUGIN_FORMATS} Standalone        # The formats to build. Valid formats: Standalone Unity VST3 AU AUv3 AAX VST LV2.
                                                # AU and AUv3 plugins will only be enabled when building on macOS.
    PRODUCT_NAME "MM-303"                       # The name of the final executable, which can differ from the target name.
    PLUGIN_NAME "MM-303"                        # Name of the plugin that will be displayed in the DAW. Can differ from PRODUCT_NAME.

    ICON_BIG "${PROJECT_SOURCE_DIR}/img/ICON_BIG.png"

    # ICON_BIG                                  # ICON_* arguments specify a path to an image file to use as an icon for the Standalone.
    # ICON_SMALL
    COMPANY_NAME MockbaMod                      # The name of this target's author. The value is inherited from JUCE_COMPANY_NAME.
    COMPANY_WEBSITE "https://github.com/MockbaMod/MM-303"  # The address of a website related to this target in some way.
                                                # The value is inherited from JUCE_COMPANY_WEBSITE.
    COMPANY_EMAIL "contact@mockbamod.com"       # An email address for this target's author. The value is inherited from JUCE_COMPANY_EMAIL.
    PLUGIN_MANUFACTURER_CODE Mmbk               # A four-character manufacturer id with at least one upper-case character.
                                                # GarageBand 10.3 requires the first letter to be upper-case, and the remaining letters to be lower-case.
    PLUGIN_CODE Mm03                            # A unique four-character plugin id with exactly one upper-case character.
                                                # GarageBand 10.3 requires the first letter to be upper-case, and the remaining letters to be lower-case.

    IS_SYNTH TRUE                               # Is this a synth or an effect?
    NEEDS_MIDI_INPUT TRUE                       # Does the plugin need midi input?
    NEEDS_MIDI_OUTPUT TRUE                      # Does the plugin need midi output?
    IS_MIDI_EFFECT FALSE                        # Is this plugin a MIDI effect?
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE           # Does the editor need keyboard focus?

    LV2URI https://github.com/MockbaMod/MM-303

    VST3_CATEGORIES "Generator Instrument"      # Should be one or more, separated by spaces, of the following:
                                                # Fx, Instrument, Analyzer, Delay, Distortion, Drum, Dynamics, EQ, External,
                                                # Filter, Generator, Mastering, Modulation, Mono, Network, NoOfflineProcess,
                                                # OnlyOfflineProcess, OnlyRT, Pitch Shift, Restoration, Reverb, Sampler, Spatial,
                                                # Stereo, Surround, Synth, Tools, Up-Downmix
    AU_MAIN_TYPE "kAudioUnitType_MusicDevice"   # Should be one or more, separated by spaces, of the following:
                                                # kAudioUnitType_Effect, kAudioUnitType_FormatConverter, kAudioUnitType_Generator,
                                                # kAudioUnitType_MIDIProcessor, kAudioUnitType_Mixer, kAudioUnitType_MusicDevice,
                                                # kAudioUnitType_MusicEffect, kAudioUnitType_OfflineEffect, kAudioUnitType_Output,
                                                # kAudioUnitType_Panner

    COPY_PLUGIN_AFTER_BUILD FALSE)              # Should the plugin be installed to a default location after building?
# NOTE: Writing to the default install locations might require administrator privileges on Windows.

clap_juce_extensions_plugin(
    TARGET "${PROJECT_NAME}"
    CLAP_ID "com.mockbamod.${PROJECT_NAME}"
    CLAP_FEATURES "instrument" "synthesizer")

# Single source of truth for the default MM-303.conf: embed the tracked file
# (templates/MM-303.conf) into the plugin so the runtime default always matches
# the file that ships next to the .so.  Edit the file, rebuild, done.
set(_mm303_conf "${PROJECT_SOURCE_DIR}/../templates/MM-303.conf")
if(EXISTS "${_mm303_conf}")
    file(READ "${_mm303_conf}" _mm303_conf_text)
    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/MM303DefaultConfig.h"
        CONTENT "#pragma once\nstatic const char* kDefaultConfigText = R\"MM303CONF(\n${_mm303_conf_text}\n)MM303CONF\";\n")
    target_include_directories("${PROJECT_NAME}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()

# SMF -> Akai .mpcpattern converter (tools/midi_to_mpcpattern.cpp).  Built as a
# small standalone static lib (it lives outside plugin/src) and linked into the
# plugin; its include dir exposes the header to plugin/src.
add_library(mm303_mpcpattern STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../tools/midi_to_mpcpattern.cpp")
target_include_directories(mm303_mpcpattern PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../tools")
target_compile_features(mm303_mpcpattern PRIVATE cxx_std_17)
set_target_properties(mm303_mpcpattern PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries("${PROJECT_NAME}" PUBLIC mm303_mpcpattern)

# Set the C++ language standard requirenment for the "shared code" library target.
# Setting this to PUBLIC ensures that all dependent targets will inherit the specified C++ standard.
target_compile_features("${PROJECT_NAME}" PUBLIC cxx_std_17)

# Disable compiler extensions for the project targets (e.g. use -std=c++17 instead of -std=gnu++17).
get_property(project_targets DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)

set_target_properties(${project_targets} PROPERTIES CXX_EXTENSIONS OFF)

# `juce_generate_juce_header` will create a JuceHeader.h for a given target, which will be generated
# into your build tree. The include path for this header will be automatically added to the target.
# NOTE: JuceHeader.h is generated when the target is built.

juce_generate_juce_header("${PROJECT_NAME}")

# `target_compile_definitions` adds some preprocessor definitions to our target.
# JUCE modules also make use of compile definitions to switch certain features on/off,
# so if there's a particular feature you need that's not on by default, check the module header
# for the correct flag to set here.
# These definitions will be visible both to your code, and also the JUCE module code.

target_compile_definitions("${PROJECT_NAME}"
    PUBLIC
        # JUCE_WEB_BROWSER and JUCE_USE_CURL would be on by default, but you might not need them.
        JUCE_WEB_BROWSER=0  # If you remove this, add `NEEDS_WEB_BROWSER TRUE` to the `juce_add_plugin` call
        JUCE_USE_CURL=0     # If you remove this, add `NEEDS_CURL TRUE` to the `juce_add_plugin` call
        JUCE_VST3_CAN_REPLACE_VST2=0)

# If your target needs extra binary assets, you can add them here.
# NOTE: Conversion to binary-data happens when the target is built.

# juce_add_binary_data(Assets       # Name of a static library target that will include all the binary resources
# HEADER_NAME ...               # Name of the generated header. "BinaryData.h" is used by default
# NAMESPACE ...                 # Namespace of the generated binary data class. "BinaryData" is used by default
#   SOURCES ...)                    # List of asset files of any kind that should be built into the static library

# `target_link_libraries` links libraries and JUCE modules to other libraries or executables. Here,
# we're linking our executable target to the `juce::juce_audio_utils` module. Inter-module
# dependencies are resolved automatically.

target_link_libraries("${PROJECT_NAME}"
    PRIVATE
        BinaryData
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Add the subdirectory with source files.
add_subdirectory(src)
include_directories(src)

# --- MpcPreset shim (Force preset management) --------------------------------
# The shim owns the exported VST2 entry: the plugin's own VSTPluginMain is
# renamed VSTPluginMainReal on the VST target, and the shim provides the real
# VSTPluginMain wrapping it with +4 preset control params. Built as a separate
# static lib so the rename does not apply to the shim itself.
set(_preset_dir "${CMAKE_CURRENT_SOURCE_DIR}/../templates/preset")
set(_rtmidi_dir "${CMAKE_CURRENT_SOURCE_DIR}/../templates/rtmidi")
set(_vst2abi   "${CMAKE_CURRENT_SOURCE_DIR}/../templates/vst2-abi/pluginterfaces/vst2.x")
if(EXISTS "${_preset_dir}/preset_shim.cpp" AND TARGET "${PROJECT_NAME}_VST")
    add_library(mm303_preset_shim STATIC
        "${_preset_dir}/preset.cpp"
        "${_preset_dir}/preset_shim.cpp"
        "${_rtmidi_dir}/RtMidi.cpp")
    target_include_directories(mm303_preset_shim PRIVATE "${_preset_dir}" "${_rtmidi_dir}" "${_vst2abi}")
    target_compile_features(mm303_preset_shim PRIVATE cxx_std_17)
    target_compile_options(mm303_preset_shim PRIVATE -fPIC)
    # RtMidi ALSA backend for the virtual MIDI output port (libasound).
    target_compile_definitions(mm303_preset_shim PRIVATE __LINUX_ALSA__)
    # PUBLIC so the final VST target links libasound too (static shim).
    target_link_libraries(mm303_preset_shim PUBLIC asound)
    # Runtime-only inner params are deliberately NOT configured here. The plugin
    # declares them non-automatable in createParameterLayout() (single source of
    # truth); the shim resolves that set from effCanBeAutomated at open, so
    # nothing is hard-wired by index. Momentary triggers are unchanged.
    target_compile_definitions(mm303_preset_shim PRIVATE
        MM303_MOMENTARY_A=39 MM303_MOMENTARY_B=40 MM303_MOMENTARY_C=41)
    # Host-sync diagnostics are off in normal builds (no log file on the
    # device); enable with -DMM303_SHIM_LOG=ON when debugging on hardware.
    option(MM303_SHIM_LOG "preset-shim host-sync debug logging" OFF)
    if(MM303_SHIM_LOG)
        target_compile_definitions(mm303_preset_shim PRIVATE MM303_SHIM_LOG=1)
    endif()
    target_compile_definitions("${PROJECT_NAME}_VST" PRIVATE VSTPluginMain=VSTPluginMainReal)
    # --whole-archive: nothing references the shim's VSTPluginMain, so a plain
    # static link would drop its object; force-include it.
    target_link_libraries("${PROJECT_NAME}_VST" PRIVATE
        "-Wl,--whole-archive" mm303_preset_shim "-Wl,--no-whole-archive")
    message(STATUS "MM-303: MpcPreset shim linked into ${PROJECT_NAME}_VST")
endif()

### IDE Generator post-config ###

# IDEs:  Place source groups for project targets into a "Targets" folder (to reduce visual clutter).
# Xcode: Generate build schemes for project targets.
set_target_properties(${project_targets} PROPERTIES
    FOLDER "Targets"
    XCODE_GENERATE_SCHEME ON)

# IDEs:  Move the "shared code" source group (main target) out of the "Targets" folder
#        to the top level.
set_target_properties("${PROJECT_NAME}" PROPERTIES FOLDER "")

# IDEs:  Organise source file grouping.
get_target_property(project_sources "${PROJECT_NAME}" SOURCES)

#   If JuceHeader.h is generated, remove it from the source file list and handle it individually.
get_target_property(juce_library_code "${PROJECT_NAME}" JUCE_GENERATED_SOURCES_DIRECTORY)
set(juce_header "${juce_library_code}/JuceHeader.h")

list(REMOVE_ITEM project_sources "${juce_header}")

#   Place JuceHeader.h and the associated CMake Rule file into a "JUCE Library Code" folder.
source_group("JUCE Library Code" FILES "${juce_header}")
source_group("JUCE Library Code/CMake Rules" FILES "${juce_header}.rule")

#   Generate source groups that follow the organisation of source file directories.
source_group(TREE "${PROJECT_SOURCE_DIR}/src" FILES ${project_sources})

#   Place Binary Data related source files into the root of their target folder.
if(TARGET Assets)
    get_target_property(project_assets Assets SOURCES)
    source_group("" FILES ${project_assets})
endif()
