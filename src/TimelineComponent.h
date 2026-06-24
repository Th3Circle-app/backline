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
    enum class EditTool { Smart, Trim, Grab, Scrub, Zoom };   // Pro Tools-style edit tools
    void setEditTool (EditTool t) { editTool = t; }
    void setShuffle (bool b) { shuffle = b; }                 // Shuffle edit mode: moves snap to adjacent clip edges
    void setAutomationMode (bool b) { showAutomation = b; repaint(); }   // show/edit volume envelopes
    int  contentHeight() const;   // total stacked height of all rows (for a scroll viewport)
    int  contentWidth() const;    // total pixel width at the current zoom (for a scroll viewport)
    void setViewportWidth (int w);// visible content width of the scroll viewport (for fit/zoom)
    void zoomBy (double factor);  // multiply the px/s zoom (>1 = zoom in)
    void zoomToFit();             // refit the current content to the visible width

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
    std::function<void (int, int, float)>          onTrackPan;      // group, track, -1..+1 (header pan knob)
    std::function<void (int)>                      onVideoMute;     // group
    std::function<void (int)>                      onVideoSolo;     // group
    std::function<void (int)>                      onActivateGroup; // group
    std::function<void()>                          onAddVideo;
    std::function<void (bool, double, double)>      onLoopChanged;
    std::function<void()>                           onZoomChanged;   // zoom changed -> parent should resize the timeline
    std::function<void (int)>                       onMarkerRename;  // double-clicked a ruler marker (index)
    std::function<void (int, int, std::vector<AutoPoint>)> onAutoEdit;   // group, track, new volume envelope
    std::function<void (double)>                    onLoopMenu;      // right-click lane: timeline time (seconds)
    std::function<void (int, int)>                  onTrackMenu;     // right-click an audio track header (group, track)
    std::function<void (int)>                       onGroupMenu;     // right-click a video header (group)

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void refitZoom();   // set the fixed zoom so current content fits the visible width
    float autoY (int rowY, float v) const          // y for an envelope value (0..1.4) within an audio row
    { const float pad = 5.0f, hh = (float) rowHeight - 2.0f * pad; return (float) rowY + (float) rowHeight - pad - juce::jlimit (0.0f, 1.0f, v / 1.4f) * hh; }
    float autoValFromY (int rowY, float y) const
    { const float pad = 5.0f, hh = juce::jmax (1.0f, (float) rowHeight - 2.0f * pad); return juce::jlimit (0.0f, 1.4f, ((float) rowY + (float) rowHeight - pad - y) / hh * 1.4f); }
    enum class Drag { None, Loop, Move, TrimLeft, TrimRight, HeaderVol, HeaderPan, FadeIn, FadeOut, ClipGain, AutoPt };
    static constexpr float kClipGainRange = 18.0f;   // +/- dB mapped across the clip height
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
    juce::Rectangle<int>   panBox (int rowYpos) const; // Logic header pan knob hit area (empty otherwise)
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
    float     dragStartPan = 0.0f;
    juce::Rectangle<float> dragClipRect;   // the dragged clip's rect (for vertical clip-gain mapping)
    juce::Rectangle<int> dragVBox;     // header-volume slider track being dragged
    double    dragPps = 0.0, frozenLen = 0.0;
    bool      draggedClip = false;

    bool   loopEnabled = false; double loopStart = 0.0, loopEnd = 0.0;
    bool   movedDuringLoop = false; double loopAnchor = 0.0, loopClickTime = 0.0;
    bool   loopResizing = false;    // dragging an existing loop edge vs. creating a new region

    Skin   skin = Skin::forDaw (Skin::Layback);

    double zoomPps = 0.0;   // fixed zoom in pixels/second (0 = not yet fit to window)
    int    viewportW = 0;   // visible content width of the scroll viewport
    EditTool editTool = EditTool::Smart;   // active edit tool (PT skin); Smart = zone-based default
    bool     shuffle  = false;             // Shuffle edit mode (butt clips against neighbours)
    bool     showAutomation = false;       // volume-automation overlay + edit mode
    std::vector<AutoPoint> dragAuto;       // working copy of the envelope being edited
    int      dragAutoGroup = -1, dragAutoTrack = -1, dragAutoIdx = -1, dragAutoRowY = 0;
    double   snapClipToEdges (double ts, double dur) const;   // snap a moved clip to nearby clip boundaries

    int                  headerW     = 184;   // widened to 280 for Logic-style headers
    static constexpr int rulerHeight = 22;
    static constexpr int rowHeight   = 60;
    static constexpr int importRowH  = 26;
    static constexpr int edgePx      = 7;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineComponent)
};
