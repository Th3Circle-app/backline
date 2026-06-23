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
        lcd = { bx, cy - 18, 200, 36 };
    }

    void paint (juce::Graphics& g) override
    {
        // Logic's control bar is a flat LIGHT grey with dark glyphs + a warm-black LCD.
        const juce::Colour barBg (0xffa5a5a5), btnBg (0xffaeaeae), glyph (0xff4c4c4c);
        const juce::Colour playLit (0xff7ec46f), cycleLit (0xff9f8138), rec (0xffb82d1e);
        const juce::Colour lcdBg (0xff2b2a22), lcdText (0xffece7c9), lcdLabel (0xff8c8770);

        g.fillAll (barBg);
        g.setColour (juce::Colour (0xff000000));
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        const bool playing = isPlaying && isPlaying();
        const bool cyc     = isCycle && isCycle();

        for (int i = 0; i < 5; ++i)
        {
            auto b = btn[i].toFloat();
            const bool lit = (i == 2 && playing) || (i == 4 && cyc);
            const juce::Colour base = (i == 2 && playing) ? playLit : (i == 4 && cyc) ? cycleLit : btnBg;
            g.setGradientFill (juce::ColourGradient (base.brighter (0.06f), b.getX(), b.getY(),
                                                     base.darker (0.10f),  b.getX(), b.getBottom(), false));
            g.fillRoundedRectangle (b, 5.0f);
            g.setColour (juce::Colours::black.withAlpha (0.28f));
            g.drawRoundedRectangle (b, 5.0f, 1.0f);

            auto c = b.reduced (b.getWidth() * 0.30f);
            g.setColour ((i == 3) ? rec : (lit ? juce::Colours::white : glyph));
            juce::Path p;
            switch (i)
            {
                case 0:  // rewind
                    g.fillRect (b.getX() + 7.0f, c.getY(), 2.0f, c.getHeight());
                    p.addTriangle (c.getRight(), c.getY(), c.getRight(), c.getBottom(), c.getX() + 3.0f, c.getCentreY());
                    g.fillPath (p); break;
                case 1:  // stop
                    g.fillRoundedRectangle (c.reduced (0.5f), 1.0f); break;
                case 2:  // play
                    p.addTriangle (c.getX(), c.getY(), c.getX(), c.getBottom(), c.getRight(), c.getCentreY());
                    g.fillPath (p); break;
                case 3:  // record (red circle)
                    g.fillEllipse (c); break;
                case 4:  // cycle
                    g.drawEllipse (c.reduced (0.5f), 1.6f);
                    p.addTriangle (c.getRight() - 2.0f, c.getY() - 1.5f, c.getRight() + 3.5f, c.getY() + 2.0f, c.getRight() - 2.0f, c.getY() + 4.5f);
                    g.fillPath (p); break;
            }
        }

        auto l = lcd.toFloat();
        g.setColour (lcdBg);
        g.fillRoundedRectangle (l, 5.0f);
        g.setColour (juce::Colours::black);
        g.drawRoundedRectangle (l, 5.0f, 1.0f);
        g.setColour (lcdLabel);
        g.setFont (juce::Font (juce::FontOptions().withHeight (8.5f)));
        g.drawText ("TIME", l.reduced (9.0f, 4.0f).removeFromTop (9.0f), juce::Justification::topLeft, false);
        g.setColour (lcdText);
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 19.0f, juce::Font::bold)));
        g.drawText (lcdMain, l.reduced (9.0f, 2.0f), juce::Justification::centredRight, false);
        if (lcdSub.isNotEmpty())
        {
            g.setColour (lcdLabel);
            g.setFont (juce::Font (juce::FontOptions().withHeight (9.0f)));
            g.drawText (lcdSub, l.reduced (9.0f, 4.0f), juce::Justification::bottomLeft, false);
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
                else if (i == 4) { if (onCycle)  onCycle(); }
                repaint();
                return;
            }
    }

private:
    Skin skin = Skin::forDaw (Skin::Logic);
    juce::Rectangle<int> btn[5], lcd;
    juce::String lcdMain { "0:00.00" }, lcdSub;
};
