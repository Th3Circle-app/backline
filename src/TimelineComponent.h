#pragma once

#include <vector>
#include <memory>
#include <juce_gui_basics/juce_gui_basics.h>
#include "Clip.h"
#include "Track.h"
#include "Skin.h"

namespace juce { class AudioThumbnail; }

//==============================================================================
/** Multi-video timeline: a stack of video groups, each = a video track (header
    with disclosure + its own audio Mute/Solo) and, when expanded, its audio
    tracks (per-track M/S) + a "+ Import Track" row. A "+ Add Video" row sits at
    the bottom. One group is active (highlighted); clicking a video activates it.

    The model is owned by the controller; this view reads it via a pointer and
    emits intents through callbacks. Clip times are drawn on the active group's
    time scale. */
class TimelineComponent : public juce::Component
{
public:
    TimelineComponent() = default;

    void setGroups (const std::vector<std::unique_ptr<VideoGroup>>* groupsPtr);
    void setActiveGroup (int g);
    void setSelection (int group, int track, int clip);
    void setPlayhead (double seconds);
    void setLoop (bool enabled, double start, double end);
    void setSnapEnabled (bool b);
    void setSkin (const Skin& s);
    int  contentHeight() const;   // total stacked height of all rows (for a scroll viewport)

    std::function<void (double)> onSeek;
    std::function<void()>        onScrubStart;
    std::function<void()>        onScrubEnd;
    std::function<void()>        onEditBegin;     // fired once when a clip/loop drag actually starts changing (for undo)
    std::function<void (int, int, int, AudioClip)> onClipChanged;   // group, track, clip, value
    std::function<void (int, int, int, double)>    onClipMenu;      // group, track, clip, time
    std::function<void (int, int, int)>            onClipSelected;  // group, track, clip
    std::function<void (int)>                      onToggleExpand;  // group
    std::function<void (int)>                      onImportTrack;   // group
    std::function<void (int, int)>                 onTrackMute;     // group, track
    std::function<void (int, int)>                 onTrackSolo;     // group, track
    std::function<void (int, int)>                 onTrackRecord;   // group, track (record-enable)
    std::function<void (int, int, float)>          onTrackVolume;   // group, track, linear gain (header fader)
    std::function<void (int)>                      onVideoMute;     // group
    std::function<void (int)>                      onVideoSolo;     // group
    std::function<void (int)>                      onActivateGroup; // group
    std::function<void()>                          onAddVideo;
    std::function<void (bool, double, double)>      onLoopChanged;
    std::function<void (double)>                    onLoopMenu;      // right-click lane: timeline time (seconds)
    std::function<void (int, int)>                  onTrackMenu;     // right-click an audio track header (group, track)
    std::function<void (int)>                       onGroupMenu;     // right-click a video header (group)

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

private:
    enum class Drag { None, Loop, Move, TrimLeft, TrimRight, HeaderVol };
    struct Row { enum Kind { Video, Audio, Import, AddVideo }; Kind kind; int group; int track; int y; int h; };

    int    numGroups() const;
    const VideoGroup* activeG() const;
    std::vector<Row> buildRows() const;
    double rawTimelineLength() const;
    double timelineLength() const;
    double pixelsPerSecond() const;
    double xForTime (double t) const;
    double timeForX (double x) const;
    juce::Rectangle<float> videoClipRectAt (int rowYpos, double durationSeconds) const;
    juce::Rectangle<float> clipRectAt (int rowYpos, const AudioClip&) const;
    juce::Rectangle<int>   mBox (int rowYpos) const;
    juce::Rectangle<int>   sBox (int rowYpos) const;
    juce::Rectangle<int>   rBox (int rowYpos) const;   // Logic record-enable box (empty otherwise)
    juce::Rectangle<int>   vBox (int rowYpos) const;   // Logic header volume slider track (empty otherwise)
    juce::Rectangle<int>   disclosureRectAt (int rowYpos) const;
    void seekFromMouse (const juce::MouseEvent&);
    void drawMS (juce::Graphics&, int rowYpos, bool mute, bool solo);
    double applySnap (double ts) const;

    const std::vector<std::unique_ptr<VideoGroup>>* groups = nullptr;
    int activeGroup = -1;
    int selGroup = -1, selTrack = -1, selClip = -1;

    double playhead = 0.0;
    bool   snapEnabled = true;

    Drag      dragMode = Drag::None;
    int       dragGroup = -1, dragTrack = -1, dragClip = -1;
    AudioClip dragStartClip;
    int       dragStartX = 0;
    juce::Rectangle<int> dragVBox;     // header-volume slider track being dragged
    double    dragPps = 0.0, frozenLen = 0.0;
    bool      draggedClip = false;

    bool   loopEnabled = false; double loopStart = 0.0, loopEnd = 0.0;
    bool   movedDuringLoop = false; double loopAnchor = 0.0, loopClickTime = 0.0;
    bool   loopResizing = false;    // dragging an existing loop edge vs. creating a new region

    Skin   skin = Skin::forDaw (Skin::Layback);

    int                  headerW     = 184;   // widened to 280 for Logic-style headers
    static constexpr int rulerHeight = 22;
    static constexpr int rowHeight   = 60;
    static constexpr int importRowH  = 26;
    static constexpr int edgePx      = 7;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineComponent)
};
