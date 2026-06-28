#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Skin.h"

//==============================================================================
/** Pro Tools-style transport bar: edit-tool cluster, transport, and the signature
    twin green LED counters. Drawn look-alike controls (no Avid artwork). */
class ProToolsControlBar : public juce::Component
{
public:
    std::function<void()> onRewind, onStop, onPlay, onRecord, onLoop;
    std::function<void()> onCounterClick;       // click the LCD to choose the readout format
    std::function<void (int)> onTool, onMode;   // edit-tool selected; edit-mode selected (3 = Grid)
    std::function<bool()> isPlaying, isLoop;
    int selTool = 4, selMode = 1;               // Smart tool + Slip mode are the defaults

    ProToolsControlBar() { setInterceptsMouseClicks (true, false); }

    void setSkin (const Skin& s) { skin = s; repaint(); }
    void setPosition (const juce::String& main, const juce::String& sub)
    {
        if (main != mainCtr || sub != subCtr) { mainCtr = main; subCtr = sub; repaint(); }
    }

    void resized() override
    {
        const int cy = getHeight() / 2;
        int lx = 12;                                              // edit-tool cluster (left)
        for (int i = 0; i < 5; ++i) { tool[i] = { lx, cy - 12, 24, 24 }; lx += 26; }

        int bx = getWidth() / 2 - 200;                            // transport cluster
        const int d = 28, gap = 5;
        for (int i = 0; i < 5; ++i) { btn[i] = { bx, cy - d / 2, d, d }; bx += d + gap; }
        bx += 12;
        mainLcd = { bx, cy - 19, 200, 38 };                      // Main counter (timecode) — large
        subLcd  = { bx + 206, cy - 19, 88, 38 };                 // Sub counter (bars)

        int rx = getWidth() - 12;                                 // edit-mode cluster (right)
        for (int i = 0; i < 4; ++i) { rx -= 26; mode[i] = { rx, cy - 12, 24, 24 }; rx -= 3; }
    }

    void paint (juce::Graphics& g) override
    {
        const juce::Colour barTop (0xff2c2c2c), barBot (0xff1d1d1d), chip (0xff343434), glyph (0xffc8c8c8);
        const juce::Colour rec (0xffe23a2a), loopLit (0xff27c79a), sel (0xff2f7d63);   // selected tool/mode highlight
        const juce::Colour lcdBg (0xff04140a), lcdGrn (0xff36ff86), lcdLbl (0xff2f8f5a);

        g.setGradientFill (juce::ColourGradient (barTop, 0.0f, 0.0f, barBot, 0.0f, (float) getHeight(), false));
        g.fillAll();
        g.setColour (juce::Colours::black);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        auto bevel = [&] (juce::Rectangle<int> b, juce::Colour base)
        {
            auto rf = b.toFloat();
            g.setGradientFill (juce::ColourGradient (base.brighter (0.12f), rf.getX(), rf.getY(), base.darker (0.22f), rf.getX(), rf.getBottom(), false));
            g.fillRoundedRectangle (rf, 4.0f);
            g.setColour (base.brighter (0.4f)); g.drawLine (rf.getX() + 3, rf.getY() + 0.8f, rf.getRight() - 3, rf.getY() + 0.8f, 1.0f);
            g.setColour (juce::Colours::black.withAlpha (0.6f)); g.drawRoundedRectangle (rf, 4.0f, 1.0f);
        };

        for (int i = 0; i < 5; ++i)                              // edit tools: zoom/trim/grab/scrub/smart
        {
            bevel (tool[i], i == selTool ? sel : chip);
            auto c = tool[i].reduced (7).toFloat();
            g.setColour (i == selTool ? juce::Colours::white : glyph);
            switch (i)
            {
                case 0: g.drawEllipse (c.removeFromTop (c.getWidth()).reduced (1.0f), 1.4f); g.drawLine (c.getCentreX(), c.getCentreY(), c.getRight(), c.getBottom(), 1.4f); break; // zoom
                case 1: g.drawLine (c.getX(), c.getY(), c.getRight(), c.getBottom(), 1.4f); g.drawLine (c.getX(), c.getBottom(), c.getRight(), c.getY(), 1.4f); break;             // trim
                case 2: g.drawRoundedRectangle (c, 2.0f, 1.4f); break;                                                                                                            // grab
                case 3: g.drawLine (c.getX(), c.getCentreY(), c.getRight(), c.getCentreY(), 1.4f); break;                                                                          // scrub
                case 4: { juce::Path a; a.addTriangle (c.getX(), c.getY(), c.getX() + c.getWidth() * 0.66f, c.getCentreY(), c.getX(), c.getBottom()); g.fillPath (a); } break;          // smart (pointer)
            }
        }

        const bool playing = isPlaying && isPlaying();
        const bool loop    = isLoop && isLoop();
        for (int i = 0; i < 5; ++i)                              // transport: rewind/stop/play/record/loop
        {
            const bool lit = (i == 2 && playing) || (i == 4 && loop);
            bevel (btn[i], lit ? (i == 4 ? loopLit : juce::Colour (0xff2f9e6e)) : chip);
            auto c = btn[i].toFloat().reduced (btn[i].getWidth() * 0.30f);
            g.setColour ((i == 3) ? rec : (lit ? juce::Colours::white : glyph));
            juce::Path p;
            switch (i)
            {
                case 0: g.fillRect (btn[i].getX() + 8.0f, c.getY(), 2.0f, c.getHeight());
                        p.addTriangle (c.getRight(), c.getY(), c.getRight(), c.getBottom(), c.getX() + 3.0f, c.getCentreY()); g.fillPath (p); break;
                case 1: g.fillRoundedRectangle (c.reduced (0.5f), 1.0f); break;
                case 2: p.addTriangle (c.getX(), c.getY(), c.getX(), c.getBottom(), c.getRight(), c.getCentreY()); g.fillPath (p); break;
                case 3: g.fillEllipse (c); break;
                case 4: g.drawEllipse (c.reduced (0.5f), 1.6f);
                        p.addTriangle (c.getRight() - 2.0f, c.getY() - 1.5f, c.getRight() + 3.5f, c.getY() + 2.0f, c.getRight() - 2.0f, c.getY() + 4.5f); g.fillPath (p); break;
            }
        }

        auto led = [&] (juce::Rectangle<int> r, const juce::String& label, const juce::String& value, float sz)
        {
            auto rf = r.toFloat();
            g.setColour (lcdBg); g.fillRoundedRectangle (rf, 4.0f);
            g.setColour (juce::Colours::black); g.drawRoundedRectangle (rf, 4.0f, 1.0f);
            g.setColour (lcdLbl); g.setFont (juce::Font (juce::FontOptions().withHeight (7.5f)));
            g.drawText (label, rf.reduced (6.0f, 3.0f).removeFromTop (8.0f), juce::Justification::topLeft, false);
            g.setColour (lcdGrn); g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), sz, juce::Font::bold)));
            g.drawText (value, rf.reduced (6.0f, 2.0f), juce::Justification::centredRight, false);
        };
        led (mainLcd, "TIMECODE",   mainCtr, 24.0f);
        led (subLcd,  "BARS|BEATS", subCtr, 14.0f);

        for (int i = 0; i < 4; ++i)                              // edit-mode cluster (right): shuffle/slip/spot/grid
        {
            bevel (mode[i], i == selMode ? sel : chip);
            auto c = mode[i].reduced (7).toFloat();
            g.setColour (i == selMode ? juce::Colours::white : glyph);
            if      (i == 0) g.drawRect (c, 1.4f);
            else if (i == 1) g.drawLine (c.getX(), c.getCentreY(), c.getRight(), c.getCentreY(), 1.4f);
            else if (i == 2) g.drawEllipse (c.reduced (1.0f), 1.4f);
            else             for (int k = 0; k < 3; ++k) g.fillRect (c.getX() + (float) k * (c.getWidth() / 2.6f), c.getY(), 2.0f, c.getHeight());
        }
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
        for (int i = 0; i < 5; ++i)                              // edit-tool palette (remembers selection)
            if (tool[i].contains (e.getPosition())) { selTool = i; if (onTool) onTool (i); repaint(); return; }
        for (int i = 0; i < 4; ++i)                              // edit-mode (Grid = snap on, others = off)
            if (mode[i].contains (e.getPosition())) { selMode = i; if (onMode) onMode (i); repaint(); return; }
        if ((mainLcd.contains (e.getPosition()) || subLcd.contains (e.getPosition())) && onCounterClick) onCounterClick();
    }

private:
    Skin skin = Skin::forDaw (Skin::ProTools);
    juce::Rectangle<int> tool[5], btn[5], mode[4], mainLcd, subLcd;
    juce::String mainCtr { "00:00:00" }, subCtr { "1|1" };
};
