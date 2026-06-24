#pragma once

//==============================================================================
/** One audio segment on the timeline: a slice of the source song placed at a
    position. No time-stretch, so timeline duration == source seconds consumed.
*/
struct AudioClip
{
    double timelineStart = 0.0;   // position on the timeline (seconds)
    double sourceIn      = 0.0;   // in-point within the source song (seconds)
    double duration      = 0.0;   // length on the timeline / source consumed
    double fadeIn        = 0.0;   // fade-in length (seconds) from the clip start
    double fadeOut       = 0.0;   // fade-out length (seconds) to the clip end
    int    fadeInShape   = 0;     // 0 linear, 1 exponential, 2 s-curve (bell), 3 logarithmic
    int    fadeOutShape  = 0;
    float  gainDb        = 0.0f;  // per-clip gain in dB (0 = unity), pre-fader

    double timelineEnd() const { return timelineStart + duration; }
};

// Shortest a clip may be trimmed/split to (seconds). Shared so split and trim agree.
constexpr double kMinClipSeconds = 0.10;
