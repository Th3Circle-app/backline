#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "Skin.h"

//==============================================================================
/** App-wide theme driven by the current Skin: cohesive palette, rounded gradient
    buttons, styled popup menus / scrollbars, clean fonts. Call applySkin() to
    reskin the whole app when the DAW profile changes. */
class LaybackLookAndFeel : public juce::LookAndFeel_V4
{
public:
    Skin skin = Skin::forDaw (Skin::Layback);

    LaybackLookAndFeel() { applySkin (skin); }

    void applySkin (const Skin& s)
    {
        skin = s;
        setColour (juce::ResizableWindow::backgroundColourId, s.windowBg);
        setColour (juce::TextButton::buttonColourId,    s.control);
        setColour (juce::TextButton::buttonOnColourId,  s.accent.withAlpha (0.85f));
        setColour (juce::TextButton::textColourOffId,   s.text);
        setColour (juce::TextButton::textColourOnId,    juce::Colours::white);
        setColour (juce::ToggleButton::textColourId,    s.text);
        setColour (juce::ToggleButton::tickColourId,    s.accent);
        setColour (juce::ToggleButton::tickDisabledColourId, s.control.brighter (0.2f));
        setColour (juce::Label::textColourId,           s.text);
        setColour (juce::PopupMenu::backgroundColourId, s.panel.brighter (0.04f));
        setColour (juce::PopupMenu::textColourId,       s.text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, s.accent);
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::ScrollBar::thumbColourId,      s.control.brighter (0.15f));
        setColour (juce::ScrollBar::trackColourId,      s.windowBg);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = backgroundColour;
        if (shouldDrawButtonAsDown)             c = c.darker (0.25f);
        else if (shouldDrawButtonAsHighlighted) c = c.brighter (0.12f);

        g.setGradientFill (juce::ColourGradient (c.brighter (0.06f), r.getX(), r.getY(),
                                                 c.darker (0.12f),  r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (skin.windowBg.darker (0.4f));
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::Font (juce::FontOptions().withHeight (juce::jmin (13.5f, buttonHeight * 0.5f)));
    }

    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions().withHeight (14.0f)); }
};
