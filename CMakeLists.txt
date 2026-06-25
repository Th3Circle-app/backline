cmake_minimum_required(VERSION 3.22)

project(LaybackStation VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE lives in external/ (cloned, not committed). See docs/ROADMAP.md.
add_subdirectory(external/JUCE)

# SoundTouch (LGPL) - offline time-stretch for "fit a cue to a scene length". Cloned in external/.
add_library(soundtouch STATIC
    external/soundtouch/source/SoundTouch/AAFilter.cpp
    external/soundtouch/source/SoundTouch/FIFOSampleBuffer.cpp
    external/soundtouch/source/SoundTouch/FIRFilter.cpp
    external/soundtouch/source/SoundTouch/InterpolateCubic.cpp
    external/soundtouch/source/SoundTouch/InterpolateLinear.cpp
    external/soundtouch/source/SoundTouch/InterpolateShannon.cpp
    external/soundtouch/source/SoundTouch/RateTransposer.cpp
    external/soundtouch/source/SoundTouch/SoundTouch.cpp
    external/soundtouch/source/SoundTouch/TDStretch.cpp
    external/soundtouch/source/SoundTouch/cpu_detect_x86.cpp)
target_include_directories(soundtouch PUBLIC external/soundtouch/include)
target_compile_definitions(soundtouch PUBLIC SOUNDTOUCH_FLOAT_SAMPLES
                                      PRIVATE SOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS ST_NO_EXCEPTION_HANDLING)
target_compile_options(soundtouch PRIVATE "SHELL:-include algorithm" "SHELL:-include cstring" -w)   # SoundTouch sources miss <algorithm>; silence its warnings

juce_add_gui_app(LaybackStation
    PRODUCT_NAME "Layback Station"
    COMPANY_NAME "Layback"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Layback Station records audio takes from your input device."
    BUNDLE_ID "app.layback.station")

target_sources(LaybackStation PRIVATE
    src/Main.cpp
    src/VideoView.mm
    src/TimelineComponent.cpp
    src/AudioEngine.cpp
    src/FfmpegTool.cpp)

target_compile_definitions(LaybackStation PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LaybackStation,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LaybackStation,JUCE_VERSION>")

target_link_libraries(LaybackStation
    PRIVATE
        juce::juce_gui_extra
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_dsp
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework QuartzCore"
        "-framework CoreAudioKit"
        "-framework MediaToolbox"
        soundtouch
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Bundle the `uv` installer (a single static binary) into the app so the one-click
# stem-splitter setup needs nothing preinstalled on a clean Mac. ffmpeg is left as an
# optional system escape hatch (Homebrew ffmpeg is dynamically linked); SoundTouch is
# already compiled in statically.
find_program(UV_BIN uv PATHS "$ENV{HOME}/.local/bin" /usr/local/bin /opt/homebrew/bin)
if(UV_BIN)
    add_custom_command(TARGET LaybackStation POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:LaybackStation>/Contents/Resources/bin"
        COMMAND ${CMAKE_COMMAND} -E copy "${UV_BIN}" "$<TARGET_BUNDLE_DIR:LaybackStation>/Contents/Resources/bin/uv"
        COMMENT "Bundling uv into the app for zero-prerequisite stem-splitter setup")
endif()
