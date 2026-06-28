#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Skin.h"

//==============================================================================
/** Backline's own transport + counter bar: a glassy indigo control strip with the
    BACKLINE wordmark, backlit-glass transport keys, and a big glowing timecode
    counter + bars/beats sub-counter. Drawn primitives, the house identity. */
class BacklineControlBar : public juce::Component
{
public:
    std::function<void()> onRewind, onStop, onPlay, onRecord, onLoop;
    std::function<void()> onCounterClick;   // click the LCD to choose the readout format
    std::function<void (int)> onCounterDrag; // drag the LCD vertically to nudge tempo
    std::function<void()> onCounterRelease;  // drag ended (persist)
    std::function<bool()> isPlaying, isLoop;

    BacklineControlBar() { setInterceptsMouseClicks (true, false); }

    void setSkin (const Skin& s) { skin = s; repaint(); }
    void setPosition (const juce::String& main, const juce::String& sub)
    {
        if (main != mainCtr || sub != subCtr) { mainCtr = main; subCtr = sub; repaint(); }
    }
    void setCounterLabels (const juce::String& a, const juce::String& b)
    {
        if (a != pLabel || b != sLabel) { pLabel = a; sLabel = b; repaint(); }
    }

    void resized() override
    {
        const int cy = getHeight() / 2;
        int bx = 150;                                            // transport cluster (after the wordmark)
        const int d = 30, gap = 7;
        for (int i = 0; i < 5; ++i) { btn[i] = { bx, cy - d / 2, d, d }; bx += d + gap; }
        bx += 14;
        mainLcd = { bx, cy - 20, 208, 40 };                     // big timecode
        subLcd  = { bx + 214, cy - 20, 96, 40 };                // bars|beats
    }

    void paint (juce::Graphics& g) override
    {
        const juce::Colour barTop = skin.headerTop.brighter (0.05f), barBot = skin.windowBg.darker (0.2f);
        g.setGradientFill (juce::ColourGradient (barTop, 0.0f, 0.0f, barBot, 0.0f, (float) getHeight(), false));
        g.fillAll();

        // BACKLINE wordmark + accent dot
        g.setColour (skin.text);
        g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        g.drawText ("BACKLINE", 18, 0, 110, getHeight(), juce::Justification::centredLeft, false);
        g.setColour (skin.accent);
        g.fillEllipse (118.0f, (float) getHeight() / 2.0f - 3.0f, 6.0f, 6.0f);

        const bool playing = isPlaying && isPlaying();
        const bool loop    = isLoop && isLoop();

        // backlit-glass transport keys: rewind/RTZ, stop, play, record, cycle
        for (int i = 0; i < 5; ++i)
        {
            const bool lit = (i == 2 && playing) || (i == 4 && loop);
            glassKey (g, btn[i], lit, i == 3 ? juce::Colour (0xffe2342a) : skin.accent);
            auto c = btn[i].toFloat().reduced (btn[i].getWidth() * 0.30f);
            g.setColour (i == 3 ? juce::Colour (0xffe2342a) : (lit ? juce::Colours::white : skin.text));
            juce::Path p;
            switch (i)
            {
                case 0: g.fillRect (btn[i].getX() + 8.0f, c.getY(), 2.0f, c.getHeight());                         // |< RTZ
                        p.addTriangle (c.getRight(), c.getY(), c.getRight(), c.getBottom(), c.getX() + 3.0f, c.getCentreY()); g.fillPath (p); break;
                case 1: g.fillRoundedRectangle (c.reduced (0.5f), 1.0f); break;                                   // stop
                case 2: p.addTriangle (c.getX(), c.getY(), c.getX(), c.getBottom(), c.getRight(), c.getCentreY()); g.fillPath (p); break;   // play
                case 3: g.fillEllipse (c); break;                                                                 // record
                case 4: g.drawEllipse (c.reduced (0.5f), 1.8f);                                                   // cycle
                        p.addTriangle (c.getRight() - 2.0f, c.getY() - 1.5f, c.getRight() + 3.5f, c.getY() + 2.0f, c.getRight() - 2.0f, c.getY() + 4.5f); g.fillPath (p); break;
            }
        }

        led (g, mainLcd, pLabel, mainCtr, 25.0f);
        led (g, subLcd,  sLabel, subCtr, 15.0f);

        // the "lay-back line" accent hairline under the bar
        g.setGradientFill (juce::ColourGradient (skin.accent, 0.0f, 0.0f, juce::Colour (0xff3fe0ff), (float) getWidth(), 0.0f, false));
        g.fillRect (0, getHeight() - 2, getWidth(), 2);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < 5; ++i)
            if (btn[i].contains (e.getPosition()))
            {
                if      (i == 0) { if (onRewind) onRewind(); }
                else if (i == 1) { if (onStop)   onStop(); }
                else if (i == 2) { if (onPlay)   onPlay(); }
                else if (i == 3) { if (onRecord) onRecord(); }
                else if (i == 4) { if (onLoop)   onLoop(); }
                repaint(); return;
            }
        if (mainLcd.contains (e.getPosition()) || subLcd.contains (e.getPosition())) { lcdHit = true; lcdMoved = false; lcdLastY = e.y; }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! lcdHit) return;
        const int d = lcdLastY - e.y; lcdLastY = e.y;
        if (d != 0) { lcdMoved = true; if (onCounterDrag) onCounterDrag (d); }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (! lcdHit) return;
        if (! lcdMoved) { if (onCounterClick) onCounterClick(); }
        else if (onCounterRelease) onCounterRelease();
        lcdHit = false;
    }

private:
    void glassKey (juce::Graphics& g, juce::Rectangle<int> b, bool lit, juce::Colour glow)
    {
        auto rf = b.toFloat();
        g.setGradientFill (juce::ColourGradient (skin.control.brighter (lit ? 0.35f : 0.12f), rf.getX(), rf.getY(),
                                                 skin.control.darker (0.25f), rf.getX(), rf.getBottom(), false));
        g.fillRoundedRectangle (rf, 7.0f);
        if (lit) { g.setColour (glow.withAlpha (0.55f)); g.drawRoundedRectangle (rf.reduced (0.5f), 7.0f, 1.6f); }
        else     { g.setColour (juce::Colours::black.withAlpha (0.4f)); g.drawRoundedRectangle (rf, 7.0f, 1.0f); }
        g.setColour (juce::Colours::white.withAlpha (0.10f));            // glassy top sheen
        g.fillRoundedRectangle (rf.withTrimmedBottom (rf.getHeight() * 0.55f).reduced (2.0f, 1.5f), 5.0f);
    }

    void led (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label, const juce::String& value, float sz)
    {
        auto rf = r.toFloat();
        g.setColour (skin.timecodeBg); g.fillRoundedRectangle (rf, 5.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f)); g.drawRoundedRectangle (rf, 5.0f, 1.0f);
        g.setColour (skin.accent.withAlpha (0.8f)); g.setFont (juce::Font (juce::FontOptions().withHeight (8.0f)));
        g.drawText (label, rf.reduced (7.0f, 4.0f).removeFromTop (9.0f), juce::Justification::topLeft, false);
        g.setColour (skin.timecodeText);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), sz, juce::Font::bold)));
        g.drawText (value, rf.reduced (7.0f, 2.0f), juce::Justification::centredRight, false);
    }

    Skin skin = Skin::forDaw (Skin::Layback);
    juce::Rectangle<int> btn[5], mainLcd, subLcd;
    bool lcdHit = false, lcdMoved = false; int lcdLastY = 0;
    juce::String mainCtr { "00:00:00:00" }, subCtr { "1|1" };
    juce::String pLabel { "TIMECODE" }, sLabel { "BARS|BEATS" };
};
