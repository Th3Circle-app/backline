#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Skin.h"
#include <functional>
#include <vector>

//==============================================================================
/** A guided onboarding tour: dims the window, spotlights one UI region at a time,
    and shows an explanation card with Back / Next / Skip. Steps are supplied by the
    host as {target-rect provider, title, body}; an empty target = centered card
    (intro/outro). Replayable from Help; auto-runs on first launch. */
class TourOverlay : public juce::Component
{
public:
    struct Step
    {
        std::function<juce::Rectangle<int>()> target;   // in host coords; {} => centered, no spotlight
        juce::String title, body;
    };

    std::function<void()> onClose;

    TourOverlay()
    {
        setInterceptsMouseClicks (true, true);
        auto mk = [this] (juce::TextButton& b, const juce::String& t) { b.setButtonText (t); addChildComponent (b); };
        mk (backBtn, "Back");  mk (nextBtn, "Next");  mk (skipBtn, "Skip tour");
        backBtn.onClick = [this] { go (-1); };
        nextBtn.onClick = [this] { go (+1); };
        skipBtn.onClick = [this] { finish(); };
    }

    void setSkin (const Skin& s) { skin = s; repaint(); }

    void start (std::vector<Step> s)
    {
        steps = std::move (s);
        idx = 0;
        setVisible (true);
        toFront (true);
        backBtn.setVisible (true); nextBtn.setVisible (true); skipBtn.setVisible (true);
        layoutCard();
        grabKeyboardFocus();
    }

    void relayout() { layoutCard(); }

    void paint (juce::Graphics& g) override
    {
        if (steps.empty()) return;
        const auto t = currentTarget();

        juce::Path dim;                                   // dim everything, punch a hole over the target
        dim.addRectangle (getLocalBounds().toFloat());
        if (! t.isEmpty()) dim.addRoundedRectangle (t.expanded (6).toFloat(), 9.0f);
        dim.setUsingNonZeroWinding (false);               // even-odd => inner rect subtracts
        g.setColour (juce::Colours::black.withAlpha (0.74f));
        g.fillPath (dim);

        if (! t.isEmpty())
        {
            g.setColour (skin.accent);
            g.drawRoundedRectangle (t.expanded (6).toFloat(), 9.0f, 2.0f);
        }

        // card
        auto c = card.toFloat();
        g.setColour (skin.panel.brighter (0.04f));     g.fillRoundedRectangle (c, 10.0f);
        g.setColour (skin.accent.withAlpha (0.5f));    g.drawRoundedRectangle (c.reduced (0.5f), 10.0f, 1.4f);

        auto inner = card.reduced (16, 13);
        g.setColour (skin.muted);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("STEP " + juce::String (idx + 1) + " OF " + juce::String ((int) steps.size()),
                    inner.removeFromTop (14), juce::Justification::topLeft, false);
        inner.removeFromTop (2);
        g.setColour (skin.text);
        g.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
        g.drawText (steps[(size_t) idx].title, inner.removeFromTop (24), juce::Justification::topLeft, false);
        inner.removeFromTop (4);
        g.setColour (skin.text.withAlpha (0.86f));
        g.setFont (juce::Font (juce::FontOptions (13.5f)));
        g.drawFittedText (steps[(size_t) idx].body, inner.removeFromTop (inner.getHeight() - 38),
                          juce::Justification::topLeft, 5);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey)                                        { finish(); return true; }
        if (k == juce::KeyPress::leftKey)                                          { go (-1); return true; }
        if (k == juce::KeyPress::rightKey || k == juce::KeyPress::spaceKey
            || k == juce::KeyPress::returnKey)                                     { go (+1); return true; }
        return false;
    }

    void mouseDown (const juce::MouseEvent&) override { grabKeyboardFocus(); }   // clicks land on the dim, not the app

private:
    juce::Rectangle<int> currentTarget() const
    {
        if (steps.empty() || ! steps[(size_t) idx].target) return {};
        return steps[(size_t) idx].target().getIntersection (getLocalBounds());
    }

    void go (int delta)
    {
        const int n = (int) steps.size();
        if (n == 0) return;
        if (idx + delta < 0) return;
        if (idx + delta >= n) { finish(); return; }
        idx += delta;
        layoutCard();
        repaint();
    }

    void finish() { setVisible (false); if (onClose) onClose(); }

    void layoutCard()
    {
        if (steps.empty()) return;
        const int cw = juce::jmin (380, getWidth() - 24);
        const int chh = 172;
        const auto t = currentTarget();
        int cx, cy;
        if (t.isEmpty())
        {
            cx = getWidth() / 2 - cw / 2;
            cy = getHeight() / 2 - chh / 2;
        }
        else
        {
            cx = juce::jlimit (12, juce::jmax (12, getWidth() - cw - 12), t.getCentreX() - cw / 2);
            cy = t.getBottom() + 16;
            if (cy + chh > getHeight() - 12) cy = juce::jmax (12, t.getY() - 16 - chh);   // flip above if no room
        }
        card = { cx, cy, cw, chh };

        auto row = card.reduced (14, 0).removeFromBottom (34).withTrimmedBottom (8);
        skipBtn.setBounds (row.removeFromLeft (88).withSizeKeepingCentre (88, 26));
        nextBtn.setBounds (row.removeFromRight (96).withSizeKeepingCentre (96, 26));
        row.removeFromRight (8);
        backBtn.setBounds (row.removeFromRight (76).withSizeKeepingCentre (76, 26));
        backBtn.setEnabled (idx > 0);
        nextBtn.setButtonText (idx == (int) steps.size() - 1 ? "Done" : "Next");
    }

    Skin skin = Skin::forDaw (Skin::Layback);
    std::vector<Step> steps;
    int idx = 0;
    juce::Rectangle<int> card;
    juce::TextButton backBtn, nextBtn, skipBtn;
};
