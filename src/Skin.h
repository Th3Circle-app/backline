#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
/** A complete visual theme. Each DAW profile maps to one Skin so switching the
    station's keymap also reskins the whole app to resemble that DAW. */
struct Skin
{
    juce::Colour windowBg, panel, control, accent, text, muted;
    juce::Colour ruler, headerTop, headerBottom, activeRow, activeStrip, rowEven, rowOdd;
    juce::Colour videoClip, videoStrip, audioClip, audioStrip, waveform;
    juce::Colour timecodeText, timecodeBg;
    bool      flatClips = false;     // Ableton-style flat fills + dark waveform vs. gradient fills
    int       buttonLook = 0;        // 0 glossy(Layback) 1 metallic(Logic) 2 beveled(ProTools) 3 flat(Ableton)
    float     buttonRadius = 5.0f;
    int       layout = 0;            // 0 default (viewer-top), 1 Logic (top control bar + docked movie)
    int       clipPalette = 0;       // region colours: 0 uniform(audioClip), 1 Pro Tools pastels, 2 Ableton brights
    bool      clipTitleBar = false;  // draw a region name strip atop each clip (DAW regions)
    bool      tintLanes = false;     // tint the whole lane with the track colour (Pro Tools)
    bool      logicHeaders = false;  // Logic-style track headers (colour icon + name + M/S/R bottom row)
    bool      barsRuler = false;     // bars/beats ruler (Logic) vs min:sec
    juce::Colour clipSelFill;                              // selected-region fill (transparent => brighten base)
    juce::Colour clipSelOutline = juce::Colours::white;    // selected-region outline
    juce::Colour playhead = juce::Colour (0xffff5a3c);     // playhead line colour
    juce::String name;

    enum Daw { Layback = 0, Logic, ProTools, Ableton };

    static juce::Colour clipColour (int palette, juce::Colour fallback, int idx)
    {
        if (palette == 1)   // Pro Tools muted pastels
        {
            static const juce::uint32 pt[] = { 0xff5f8f6a, 0xff5a82ad, 0xff8a6fae, 0xffab8a5a, 0xffac5f86, 0xff5f9aa0 };
            return juce::Colour (pt[(size_t) (idx % 6)]);
        }
        if (palette == 2)   // Ableton bright flats
        {
            static const juce::uint32 ab[] = { 0xffe0913d, 0xff5aa0d0, 0xff7bbf5a, 0xffd06fb0, 0xffd0bf4a, 0xff6f6fd6 };
            return juce::Colour (ab[(size_t) (idx % 6)]);
        }
        return fallback;
    }

    static Skin forDaw (Daw d)
    {
        Skin s;
        switch (d)
        {
            case Logic:   // 1:1 spec: light-grey chrome, #575757 surfaces, cool #2D2A35 lanes
                s.name = "Logic Pro";
                s.windowBg = juce::Colour (0xff2d2a35); s.panel = juce::Colour (0xff575757); s.control = juce::Colour (0xff6a6a6a);
                s.accent = juce::Colour (0xff4f86d8); s.text = juce::Colour (0xfff2f2f2); s.muted = juce::Colour (0xffa8a8a8);
                s.ruler = juce::Colour (0xff3b3b3b); s.headerTop = juce::Colour (0xff5e5e5e); s.headerBottom = juce::Colour (0xff525252);
                s.activeRow = juce::Colour (0xff34313b); s.activeStrip = juce::Colour (0xff4f86d8);
                s.rowEven = juce::Colour (0xff575757); s.rowOdd = juce::Colour (0xff525252);
                s.videoClip = juce::Colour (0xff48474b); s.videoStrip = juce::Colour (0xff616161);
                s.audioClip = juce::Colour (0xff48474b); s.audioStrip = juce::Colour (0xff616161);
                s.waveform = juce::Colour (0xff92befa); s.timecodeText = juce::Colour (0xffece7c9); s.timecodeBg = juce::Colour (0xff2b2a22);
                s.flatClips = false; s.buttonLook = 1; s.buttonRadius = 5.0f; s.layout = 1;
                s.clipTitleBar = true; s.clipSelFill = juce::Colour (0xff476ea6); s.clipSelOutline = juce::Colour (0xffc8a03c);
                s.playhead = juce::Colour (0xfffbec98); s.logicHeaders = true; s.barsRuler = true; break;

            case ProTools:   // near-black, teal/green accent, bright green waveforms
                s.name = "Pro Tools";
                s.windowBg = juce::Colour (0xff121212); s.panel = juce::Colour (0xff1c1c1c); s.control = juce::Colour (0xff2a2a2a);
                s.accent = juce::Colour (0xff14b07d); s.text = juce::Colour (0xffdcdcdc); s.muted = juce::Colour (0xff8a8a8a);
                s.ruler = juce::Colour (0xff1f1f1f); s.headerTop = juce::Colour (0xff242424); s.headerBottom = juce::Colour (0xff181818);
                s.activeRow = juce::Colour (0xff15352c); s.activeStrip = juce::Colour (0xff1fd0a0);
                s.rowEven = juce::Colour (0xff181818); s.rowOdd = juce::Colour (0xff1d1d1d);
                s.videoClip = juce::Colour (0xff2f6f8f); s.videoStrip = juce::Colour (0xff47a0c4);
                s.audioClip = juce::Colour (0xff1f8f6f); s.audioStrip = juce::Colour (0xff27c79a);
                s.waveform = juce::Colour (0xffaef0d6); s.timecodeText = juce::Colour (0xff36e0a8); s.timecodeBg = juce::Colour (0xff0a0a0a);
                s.flatClips = false; s.buttonLook = 2; s.buttonRadius = 3.0f; s.layout = 3;
                s.clipPalette = 1; s.clipTitleBar = true; s.tintLanes = true; break;   // beveled, per-track pastel lanes

            case Ableton:   // medium grey, orange accent, FLAT clips with dark waveforms
                s.name = "Ableton Live";
                s.windowBg = juce::Colour (0xff2b2b2b); s.panel = juce::Colour (0xff353535); s.control = juce::Colour (0xff424242);
                s.accent = juce::Colour (0xffff8a3d); s.text = juce::Colour (0xffe8e8e8); s.muted = juce::Colour (0xff9c9c9c);
                s.ruler = juce::Colour (0xff3c3c3c); s.headerTop = juce::Colour (0xff3c3c3c); s.headerBottom = juce::Colour (0xff323232);
                s.activeRow = juce::Colour (0xff4d3f2c); s.activeStrip = juce::Colour (0xffffa14d);
                s.rowEven = juce::Colour (0xff323232); s.rowOdd = juce::Colour (0xff363636);
                s.videoClip = juce::Colour (0xff5aa0d0); s.videoStrip = juce::Colour (0xff7ab8e0);
                s.audioClip = juce::Colour (0xffe0913d); s.audioStrip = juce::Colour (0xfff0a955);
                s.waveform = juce::Colour (0xff3a2a14); s.timecodeText = juce::Colour (0xffffb968); s.timecodeBg = juce::Colour (0xff1e1e1e);
                s.flatClips = true; s.buttonLook = 3; s.buttonRadius = 2.0f; s.layout = 2;
                s.clipPalette = 2; s.clipTitleBar = true; break;    // flat bright clips, video docked right

            case Layback:
            default:        // OUR flagship "Midnight Indigo Glass": violet/cyan, backlit-glass keys
                s.name = "Layback";
                s.windowBg = juce::Colour (0xff0b0a14); s.panel = juce::Colour (0xff15131f); s.control = juce::Colour (0xff211d30);
                s.accent = juce::Colour (0xff7c5cff); s.text = juce::Colour (0xffeae8f5); s.muted = juce::Colour (0xff7d7895);
                s.ruler = juce::Colour (0xff322c47); s.headerTop = juce::Colour (0xff241d3a); s.headerBottom = juce::Colour (0xff111020);
                s.activeRow = juce::Colour (0xff1d1830); s.activeStrip = juce::Colour (0xff7c5cff);
                s.rowEven = juce::Colour (0xff121120); s.rowOdd = juce::Colour (0xff0e0d18);
                s.videoClip = juce::Colour (0xff2a4f8c); s.videoStrip = juce::Colour (0xff3fb6ff);
                s.audioClip = juce::Colour (0xff4a3a8f); s.audioStrip = juce::Colour (0xff7c5cff);
                s.waveform = juce::Colour (0xff9fe7ff); s.timecodeText = juce::Colour (0xff8affe6); s.timecodeBg = juce::Colour (0xff080711);
                s.flatClips = false; s.buttonLook = 4; s.buttonRadius = 7.0f; s.clipTitleBar = true; break;   // backlit glass key

        }
        return s;
    }
};
