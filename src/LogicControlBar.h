#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Skin.h"

//==============================================================================
/** Logic-style control bar: a centred transport cluster (rewind / stop / play /
    record / cycle) and the signature LCD display. Drawn look-alike controls; no
    Logic artwork is used. */
class LogicControlBar : public juce::Component
{
public:
    std::function<void()> onRewind, onStop, onPlay, onRecord, onCycle;
    std::function<bool()> isPlaying, isCycle;

    LogicControlBar() { setInterceptsMouseClicks (true, false); }

    void setSkin (const Skin& s) { skin = s; repaint(); }
    void setPosition (const juce::String& primary, const juce::String& secondary)
    {
        if (primary != lcdMain || secondary != lcdSub) { lcdMain = primary; lcdSub = secondary; repaint(); }
    }

    void resized() override
    {
        const int d = 26, gap = 5, cy = getHeight() / 2;
        int bx = getWidth() / 2 - 250;
        for (int i = 0; i < 5; ++i) { btn[i] = { bx, cy - d / 2, d, d }; bx += d + gap; }
        bx += 10;
        lcd = { bx, cy - 18, 260, 36 };

        int lx = 12;                                    // left tool/view cluster (two groups of 3)
        for (int i = 0; i < 6; ++i) { leftBtns[i] = { lx, cy - 12, 24, 24 }; lx += 25; if (i == 2) lx += 10; }
        int rx = getWidth() - 12;                        // right view-toggle cluster (no VU meter here)
        for (int i = 0; i < 4; ++i) { rx -= 24; rightBtns[i] = { rx, cy - 12, 22, 24 }; rx -= 3; }
    }

    void paint (juce::Graphics& g) override
    {
        const juce::Colour barBg (0xff8d9094), btnBg (0xffa9adb0), glyph (0xff3c3f42);
        const juce::Colour cycleLit (0xff9f8138), rec (0xffe2342a);
        const juce::Colour lcdBg (0xff2b2a22), lcdText (0xffece7c9), lcdLabel (0xff8c8770);

        g.fillAll (barBg);
        g.setColour (juce::Colour (0xff000000));
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        auto chipBg = [&] (juce::Rectangle<int> b)
        {
            auto rf = b.toFloat();
            g.setGradientFill (juce::ColourGradient (btnBg.brighter (0.06f), rf.getX(), rf.getY(), btnBg.darker (0.10f), rf.getX(), rf.getBottom(), false));
            g.fillRoundedRectangle (rf, 4.0f);
            g.setColour (juce::Colours::black.withAlpha (0.28f));
            g.drawRoundedRectangle (rf, 4.0f, 1.0f);
        };

        for (int i = 0; i < 6; ++i)                      // left cluster: distinct glyphs
        {
            auto b = leftBtns[i]; chipBg (b);
            auto c = b.reduced (6).toFloat();
            g.setColour (glyph);
            switch (i)
            {
                case 0: for (int k = 0; k < 3; ++k) g.fillRect (c.getX(), c.getY() + (float) k * (c.getHeight() / 2.6f), c.getWidth(), 2.0f); break;          // library
                case 1: g.fillEllipse (c.getCentreX() - 1.5f, c.getY(), 3.0f, 3.0f); g.fillRect (c.getCentreX() - 1.0f, c.getY() + 5.0f, 2.0f, c.getHeight() - 5.0f); break;   // inspector (i)
                case 2: g.setFont (juce::Font (juce::FontOptions().withHeight (13.0f))); g.drawText ("?", b, juce::Justification::centred, false); break;       // help
                case 3: g.drawEllipse (c.reduced (1.0f), 1.6f); g.fillRect (c.getCentreX() - 1.0f, c.getY() + 1.0f, 2.0f, 4.0f); break;                          // smart (knob)
                case 4: for (int k = 0; k < 3; ++k) g.fillRect (c.getX() + (float) k * (c.getWidth() / 2.6f), c.getY(), 2.0f, c.getHeight()); break;            // mixer (faders)
                case 5: g.drawLine (c.getX(), c.getBottom(), c.getRight(), c.getY(), 2.0f); break;                                                              // editors (pencil)
            }
            if (i == 0) { g.setColour (rec); g.fillEllipse ((float) b.getRight() - 6.0f, (float) b.getY() + 2.0f, 4.0f, 4.0f); }   // Library red badge
        }

        for (int i = 0; i < 4; ++i)                      // right cluster: view toggles
        {
            auto b = rightBtns[i]; chipBg (b);
            auto c = b.reduced (6).toFloat();
            g.setColour (glyph);
            if      (i == 0) for (int k = 0; k < 3; ++k) g.fillRect (c.getX(), c.getY() + (float) k * (c.getHeight() / 2.6f), c.getWidth(), 2.0f);
            else if (i == 1) g.drawRect (c, 1.6f);
            else if (i == 2) g.drawEllipse (c.reduced (1.0f), 1.6f);
            else             g.fillRect (c.getCentreX() - 1.0f, c.getY(), 2.0f, c.getHeight());
        }

        const bool playing = isPlaying && isPlaying();
        const bool cyc     = isCycle && isCycle();

        for (int i = 0; i < 5; ++i)
        {
            auto b = btn[i].toFloat();
            const juce::Colour base = (i == 4 && cyc) ? cycleLit : btnBg;   // play is not green-tinted by default
            g.setGradientFill (juce::ColourGradient (base.brighter (0.06f), b.getX(), b.getY(),
                                                     base.darker (0.10f),  b.getX(), b.getBottom(), false));
            g.fillRoundedRectangle (b, 5.0f);
            g.setColour (juce::Colours::black.withAlpha (0.28f));
            g.drawRoundedRectangle (b, 5.0f, 1.0f);

            auto c = b.reduced (b.getWidth() * 0.30f);
            const bool lit = (i == 4 && cyc);
            g.setColour ((i == 3) ? rec : (lit ? juce::Colours::white : glyph));   // record always red
            juce::Path p;
            switch (i)
            {
                case 0: g.fillRect (b.getX() + 7.0f, c.getY(), 2.0f, c.getHeight());
                        p.addTriangle (c.getRight(), c.getY(), c.getRight(), c.getBottom(), c.getX() + 3.0f, c.getCentreY()); g.fillPath (p); break;
                case 1: g.fillRoundedRectangle (c.reduced (0.5f), 1.0f); break;
                case 2: p.addTriangle (c.getX(), c.getY(), c.getX(), c.getBottom(), c.getRight(), c.getCentreY()); g.fillPath (p); break;
                case 3: g.fillEllipse (c); break;
                case 4: g.drawEllipse (c.reduced (0.5f), 1.6f);
                        p.addTriangle (c.getRight() - 2.0f, c.getY() - 1.5f, c.getRight() + 3.5f, c.getY() + 2.0f, c.getRight() - 2.0f, c.getY() + 4.5f); g.fillPath (p); break;
            }
        }

        // LCD: two fields side by side (bars/beats + time)
        auto l = lcd.toFloat();
        g.setColour (lcdBg);   g.fillRoundedRectangle (l, 5.0f);
        g.setColour (juce::Colours::black); g.drawRoundedRectangle (l, 5.0f, 1.0f);
        auto left = l.removeFromLeft (l.getWidth() * 0.46f);
        g.setColour (juce::Colour (0x44000000)); g.fillRect (l.getX(), l.getY() + 4.0f, 1.0f, l.getHeight() - 8.0f);
        g.setColour (lcdLabel); g.setFont (juce::Font (juce::FontOptions().withHeight (8.0f)));
        g.drawText ("BARS  BEATS", left.reduced (8.0f, 3.0f).removeFromTop (9.0f), juce::Justification::topLeft, false);
        g.drawText ("MIN : SEC",   l.reduced (8.0f, 3.0f).removeFromTop (9.0f),   juce::Justification::topLeft, false);
        g.setColour (lcdText); g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 17.0f, juce::Font::bold)));
        g.drawText (lcdMain, left.reduced (8.0f, 2.0f), juce::Justification::centred, false);
        g.drawText (lcdSub,  l.reduced (8.0f, 2.0f),    juce::Justification::centred, false);
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
                else if (i == 4) { if (onCycle)  onCycle(); }
                repaint();
                return;
            }
    }

private:
    Skin skin = Skin::forDaw (Skin::Logic);
    juce::Rectangle<int> btn[5], lcd, leftBtns[6], rightBtns[4];
    juce::String lcdMain { "1  1" }, lcdSub { "0:00.00" };
};
