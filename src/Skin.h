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
    juce::String name;

    enum Daw { Layback = 0, Logic, ProTools, Ableton };

    static Skin forDaw (Daw d)
    {
        Skin s;
        switch (d)
        {
            case Logic:   // charcoal greys, blue accent, soft gradients
                s.name = "Logic Pro";
                s.windowBg = juce::Colour (0xff232325); s.panel = juce::Colour (0xff2c2c2e); s.control = juce::Colour (0xff3a3a3c);
                s.accent = juce::Colour (0xff3f7fd6); s.text = juce::Colour (0xffe8e8ea); s.muted = juce::Colour (0xff9a9a9e);
                s.ruler = juce::Colour (0xff343436); s.headerTop = juce::Colour (0xff363638); s.headerBottom = juce::Colour (0xff2a2a2c);
                s.activeRow = juce::Colour (0xff39465c); s.activeStrip = juce::Colour (0xff5b9be8);
                s.rowEven = juce::Colour (0xff2a2a2c); s.rowOdd = juce::Colour (0xff2e2e30);
                s.videoClip = juce::Colour (0xff6b7280); s.videoStrip = juce::Colour (0xff8a93a0);
                s.audioClip = juce::Colour (0xff3f7fd6); s.audioStrip = juce::Colour (0xff58a06a);
                s.waveform = juce::Colour (0xffd6e6ff); s.timecodeText = juce::Colour (0xffe8e8ea); s.timecodeBg = juce::Colour (0xff1b1b1d);
                s.flatClips = false; s.buttonLook = 1; s.buttonRadius = 5.5f; s.layout = 1; break;   // metallic + top control bar

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
                s.flatClips = false; s.buttonLook = 2; s.buttonRadius = 3.0f; s.layout = 3; break;   // beveled + big-counter top bar

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
                s.flatClips = true; s.buttonLook = 3; s.buttonRadius = 2.0f; s.layout = 2; break;    // flat + video docked right

            case Layback:
            default:        // our own: deep blue-black, blue accent, blue/green clips
                s.name = "Layback";
                s.windowBg = juce::Colour (0xff0e0f13); s.panel = juce::Colour (0xff171a21); s.control = juce::Colour (0xff2b303b);
                s.accent = juce::Colour (0xff4a9eff); s.text = juce::Colour (0xffe6e8ec); s.muted = juce::Colour (0xff9aa0a6);
                s.ruler = juce::Colour (0xff1b1d22); s.headerTop = juce::Colour (0xff1c2029); s.headerBottom = juce::Colour (0xff13161d);
                s.activeRow = juce::Colour (0xff1d2735); s.activeStrip = juce::Colour (0xff63b3ed);
                s.rowEven = juce::Colour (0xff121419); s.rowOdd = juce::Colour (0xff15171d);
                s.videoClip = juce::Colour (0xff3b78c2); s.videoStrip = juce::Colour (0xff3b78c2);
                s.audioClip = juce::Colour (0xff2f9e6e); s.audioStrip = juce::Colour (0xff2f9e6e);
                s.waveform = juce::Colour (0xffd9ffec); s.timecodeText = juce::Colour (0xff8fd6ff); s.timecodeBg = juce::Colour (0xff0b0d11);
                s.flatClips = false; s.buttonLook = 0; s.buttonRadius = 5.0f; break;   // glossy
        }
        return s;
    }
};
