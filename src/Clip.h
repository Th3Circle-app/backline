#pragma once

#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>

//==============================================================================
/** One audio segment on the timeline: a slice of the source song placed at a
    position. duration == timeline length; with time-stretch the source seconds
    consumed = duration / stretchRatio (a baked buffer plays in place of the source).
*/
struct AudioClip
{
    double timelineStart = 0.0;   // position on the timeline (seconds)
    double sourceIn      = 0.0;   // in-point within the source song (seconds)
    double duration      = 0.0;   // length on the timeline
    double fadeIn        = 0.0;   // fade-in length (seconds) from the clip start
    double fadeOut       = 0.0;   // fade-out length (seconds) to the clip end
    int    fadeInShape   = 0;     // 0 linear, 1 exponential, 2 s-curve (bell), 3 logarithmic
    int    fadeOutShape  = 0;
    float  gainDb        = 0.0f;  // per-clip gain in dB (0 = unity), pre-fader
    double stretchRatio  = 1.0;   // output/source length ratio (1 = none); source secs = duration/stretchRatio
    double pitchSemitones = 0.0;  // pitch shift in semitones (0 = none), baked together with the stretch
    double speedFadeIn   = 0.0;   // tape-style spin-up at the clip head (seconds), pitch+tempo ramp
    double speedFadeOut  = 0.0;   // tape-style slow-down at the clip tail (seconds)
    double bakedSrcSeconds = 0.0; // source seconds the bake consumed (0 = none; needed to re-bake on load)
    std::shared_ptr<juce::AudioBuffer<float>> stretched;   // baked stretch / speed-fade, plays in place of the source

    double timelineEnd() const { return timelineStart + duration; }
};

// Shortest a clip may be trimmed/split to (seconds). Shared so split and trim agree.
constexpr double kMinClipSeconds = 0.10;
