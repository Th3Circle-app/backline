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

    double timelineEnd() const { return timelineStart + duration; }
};

// Shortest a clip may be trimmed/split to (seconds). Shared so split and trim agree.
constexpr double kMinClipSeconds = 0.10;
