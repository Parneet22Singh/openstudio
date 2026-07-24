cmake_minimum_required(VERSION 3.22)

project(OpenStudio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# On multi-config generators (Visual Studio) default to a Release build so the
# audio thread runs optimised. Override with --config on the command line.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# JUCE — fetched at configure time. Pinned to a stable 8.x release.
# The download + shallow clone lands under the build dir on D:, not C:.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    # JUCE 7.0.12 (not 8.x): JUCE 8's graphics backend hard-requires Direct2D 1.3 /
    # DirectWrite 3 headers that MinGW-w64 does not ship, so JUCE 8 cannot compile
    # with this toolchain. JUCE 7 uses the software renderer and builds cleanly on
    # MinGW. All APIs OpenStudio uses (AudioProcessorGraph, Synthesiser, dsp filters)
    # are identical across 7.x and 8.x.
    GIT_TAG        7.0.12
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# The application target
# ---------------------------------------------------------------------------
juce_add_gui_app(OpenStudio
    PRODUCT_NAME "OpenStudio"
    COMPANY_NAME "OpenStudio"
    VERSION      "0.1.0")

juce_generate_juce_header(OpenStudio)

target_sources(OpenStudio PRIVATE
    Source/Main.cpp
    Source/AudioEngine.cpp
    Source/MainComponent.cpp)

target_compile_definitions(OpenStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OpenStudio,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OpenStudio,JUCE_VERSION>")

target_link_libraries(OpenStudio
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
