#pragma once

#include <vector>
#include <juce_core/juce_core.h>

//==============================================================================
/** Thin wrapper over a system FFmpeg binary, used as a fallback for any media
    format that Apple's AVFoundation / CoreAudio can't handle natively.

    NOTE for shipping: bundling FFmpeg has licensing implications (GPL if built
    with x264/x265). The clean formats (mp4/mov via AVFoundation, wav/mp3/m4a via
    CoreAudio) need no FFmpeg. This is the "exotic format" escape hatch. */
namespace FfmpegTool
{
    /** Locates an installed ffmpeg binary, or an invalid File if none is found. */
    juce::File find();

    /** Runs ffmpeg synchronously with the given args (exe is prepended). */
    bool runSync (const juce::File& ffmpeg, const juce::StringArray& args, juce::String& output);

    /** Detects hard scene cuts in a video; returns their timestamps (seconds). Synchronous (decodes the file). */
    std::vector<double> detectSceneCuts (const juce::File& ffmpeg, const juce::File& video, double threshold);
}
