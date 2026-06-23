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
                               bool over, bool down) override
    {
        paintControlShape (g, b.getLocalBounds().toFloat().reduced (0.5f), backgroundColour, over, down);
    }

    // Loop / Snap render as lit DAW-style buttons (not checkboxes): on = accent-lit.
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const bool on = b.getToggleState();
        paintControlShape (g, r, on ? skin.accent : skin.control, over, down);
        g.setColour (on ? juce::Colours::white : skin.text);
        g.setFont (juce::Font (juce::FontOptions().withHeight (juce::jmin (13.0f, r.getHeight() * 0.46f))));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, false);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::Font (juce::FontOptions().withHeight (juce::jmin (13.5f, buttonHeight * 0.5f)));
    }

    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions().withHeight (14.0f)); }

private:
    // One control shape, drawn in the active DAW's idiom.
    void paintControlShape (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour bg, bool over, bool down)
    {
        auto c = bg;
        if (down)      c = c.darker (0.25f);
        else if (over) c = c.brighter (0.12f);
        const float rad = skin.buttonRadius;

        switch (skin.buttonLook)
        {
            case 3:  // Flat (Ableton): solid fill, thin border, near-square
                g.setColour (c); g.fillRoundedRectangle (r, rad);
                g.setColour (c.brighter (0.30f)); g.drawRoundedRectangle (r, rad, 1.0f);
                break;

            case 2:  // Beveled (Pro Tools): industrial, top bevel + dark outline
                g.setGradientFill (juce::ColourGradient (c.brighter (0.10f), r.getX(), r.getY(),
                                                         c.darker (0.24f),  r.getX(), r.getBottom(), false));
                g.fillRoundedRectangle (r, rad);
                g.setColour (c.brighter (0.45f));
                g.drawLine (r.getX() + rad, r.getY() + 0.8f, r.getRight() - rad, r.getY() + 0.8f, 1.0f);
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.drawRoundedRectangle (r, rad, 1.0f);
                break;

            case 1:  // Metallic (Logic): bright top sheen, soft rounded
                g.setGradientFill (juce::ColourGradient (c.brighter (0.32f), r.getX(), r.getY(),
                                                         c.darker (0.10f),  r.getX(), r.getBottom(), false));
                g.fillRoundedRectangle (r, rad);
                g.setColour (juce::Colours::white.withAlpha (0.20f));
                g.drawLine (r.getX() + rad, r.getY() + 1.0f, r.getRight() - rad, r.getY() + 1.0f, 1.3f);
                g.setColour (c.darker (0.55f));
                g.drawRoundedRectangle (r, rad, 1.0f);
                break;

            default: // Glossy (Layback)
                g.setGradientFill (juce::ColourGradient (c.brighter (0.06f), r.getX(), r.getY(),
                                                         c.darker (0.12f),  r.getX(), r.getBottom(), false));
                g.fillRoundedRectangle (r, rad);
                g.setColour (skin.windowBg.darker (0.4f));
                g.drawRoundedRectangle (r, rad, 1.0f);
                break;
        }
    }
};
