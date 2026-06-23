cmake_minimum_required(VERSION 3.22)

project(LaybackStation VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE lives in external/ (cloned, not committed). See docs/ROADMAP.md.
add_subdirectory(external/JUCE)

juce_add_gui_app(LaybackStation
    PRODUCT_NAME "Layback Station"
    COMPANY_NAME "Layback"
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
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LaybackStation,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LaybackStation,JUCE_VERSION>")

target_link_libraries(LaybackStation
    PRIVATE
        juce::juce_gui_extra
        juce::juce_audio_utils
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework QuartzCore"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
