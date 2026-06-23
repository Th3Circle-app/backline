#pragma once

#include <vector>
#include <memory>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Clip.h"

//==============================================================================
/** One audio track in the UI model: a named imported song, its segments (clips),
    a waveform thumbnail, and mute/solo state. Owned by the controller; the engine
    plays it by id. */
struct AudioTrack
{
    juce::String name;
    juce::File   file;
    int          engineId     = -1;
    double       sourceLength  = 0.0;
    std::vector<AudioClip> clips;
    bool  mute = false;
    bool  solo = false;
    bool  recordArm = false;           // record-enable (visual; recording is Phase C)
    float volume = 1.0f;               // channel fader, linear (0 .. ~1.4)
    float pan    = 0.0f;               // -1 = hard left .. +1 = hard right
    std::vector<double> beatMarkers;   // onset times within the source (seconds)
    std::unique_ptr<juce::AudioThumbnail> thumb;
};

//==============================================================================
/** A video and the audio tracks that belong to it (one ad + its candidate
    songs). Each group is an independent mini-timeline; one group is "active"
    at a time (its video previews and its audio plays). */
struct VideoGroup
{
    juce::String name;
    juce::File   file;
    double       duration = 0.0;
    bool         expanded = true;
    bool         videoMute = false;
    bool         videoSolo = false;
    std::vector<std::unique_ptr<AudioTrack>> tracks;
    std::vector<double> cutMarkers;    // scene-cut times on the timeline (seconds)
};
