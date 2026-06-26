#pragma once

#include <vector>
#include <memory>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Clip.h"

//==============================================================================
/** One volume-automation breakpoint: a linear gain value at a timeline position. */
struct AutoPoint
{
    double time  = 0.0;    // timeline position (seconds)
    float  value = 1.0f;   // linear gain (0 .. ~1.4), matches the fader range
};

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
    float send   = 0.0f;               // post-fader send to the FX/aux bus (0..1)
    int   output = -1;                 // output routing: -1 = master, else a bus index
    int   mixGroup = 0;                // mixer link group (0 = none, 1..4 = linked faders/mute/solo)
    std::vector<double> beatMarkers;   // onset times within the source (seconds)
    std::vector<AutoPoint> volumeAuto;   // volume automation breakpoints (sorted by time)
    bool  automationOn = false;        // read the volume envelope instead of the static fader
    std::unique_ptr<juce::AudioThumbnail> thumb;
};

//==============================================================================
/** A named timeline marker (scene/hit point) for syncing music to picture. */
struct Marker
{
    double       time = 0.0;   // position on the timeline (seconds)
    juce::String name;
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
    std::vector<Marker> markers;       // user hit-point markers on the timeline
    double videoOffset = 0.0;          // where the film starts on the timeline (seconds); slide to offset picture vs music
    bool   videoLocked = false;        // locked = the film can't be dragged (Scott's lock/unlock); unlocked by default so you can grab it
};
