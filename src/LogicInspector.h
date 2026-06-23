#pragma once

#include <memory>
#include <functional>
#include <juce_gui_extra/juce_gui_extra.h>
#include "MixerView.h"   // ChannelStrip
#include "Track.h"
#include "AudioEngine.h"
#include "Skin.h"

//==============================================================================
/** Logic-style left Inspector: the selected track's channel strip beside the
    Master strip (fader + dB feel, pan, M/S, inserts, meter). */
class LogicInspector : public juce::Component
{
public:
    std::function<void (int, int, float)> onVolume, onPan;
    std::function<void (int, int, bool)>  onMute, onSolo;
    std::function<void (int, int)>        onFxMenu;
    std::function<void (float)>           onMasterVolume;

    LogicInspector()
    {
        master.name.setText ("Master", juce::dontSendNotification);
        master.onFader = [this] (float v) { if (onMasterVolume) onMasterVolume (v); };
        addAndMakeVisible (master);
    }

    void setEngine (AudioEngine* e) { engine = e; master.engine = e; }

    void setSkin (const Skin& s)
    {
        skin = s; master.skin = s; master.repaint();
        if (sel != nullptr) { sel->skin = s; sel->repaint(); }
        repaint();
    }

    void setSelection (const std::vector<std::unique_ptr<VideoGroup>>* g, int activeGroup, int selTrack)
    {
        groups = g; grp = activeGroup; trk = selTrack;
        rebuild();
    }

    void updateMeters()
    {
        if (engine == nullptr) return;
        if (sel != nullptr && sel->engineId >= 0)
        { const float pk = engine->getTrackPeak (sel->engineId); if (pk >= 0.0f) sel->setMeter (pk); }
        master.setMeter (engine->getMasterPeak());
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (skin.panel);
        g.setColour (skin.windowBg.darker (0.4f));
        g.fillRect (getWidth() - 1, 0, 1, getHeight());                 // right divider

        auto box = [&] (juce::Rectangle<int> b, const juce::String& title, const juce::StringArray& rows)
        {
            if (b.getHeight() < 22) return;
            g.setColour (skin.control.darker (0.12f));
            g.fillRoundedRectangle (b.toFloat(), 4.0f);
            g.setColour (skin.windowBg.darker (0.3f));
            g.drawRoundedRectangle (b.toFloat(), 4.0f, 1.0f);
            auto in = b.reduced (8, 5);
            g.setColour (skin.accent);
            g.setFont (juce::Font (juce::FontOptions().withHeight (11.0f)));
            g.drawText (title, in.removeFromTop (15), juce::Justification::topLeft, false);
            g.setColour (skin.muted);
            g.setFont (juce::Font (juce::FontOptions().withHeight (10.5f)));
            for (auto& row : rows) { if (in.getHeight() < 14) break; g.drawText (row, in.removeFromTop (15), juce::Justification::topLeft, false); }
        };

        auto a = paramArea;
        box (a.removeFromTop (72), "Quick Help", { selName.isNotEmpty() ? selName
                                                                        : juce::String ("Select a track to edit its"),
                                                   selName.isNotEmpty() ? juce::String ("Audio region selected.")
                                                                        : juce::String ("parameters + channel strip.") });
        a.removeFromTop (6);
        box (a.removeFromTop (juce::jmin (a.getHeight() / 2, 104)), "Region", { "Mute", "Loop", "Fade In / Out", "Gain" });
        a.removeFromTop (6);
        box (a, "Track", { "Icon", "Color", "Channel", "Freeze" });
    }

    void resized() override
    {
        if (engine != nullptr)
            master.fader.setValue (engine->getMasterGain(), juce::dontSendNotification);
        auto r = getLocalBounds().reduced (8);
        r.removeFromTop (22);                                          // header
        auto strips = r.removeFromBottom (juce::jmin (r.getHeight(), 320));   // channel strips at the bottom
        paramArea = r.withTrimmedBottom (6);                          // Quick Help / Region / Track panels above
        const int w = (strips.getWidth() - 8) / 2;
        if (sel != nullptr) sel->setBounds (strips.removeFromLeft (w));
        else                strips.removeFromLeft (w);
        strips.removeFromLeft (8);
        master.setBounds (strips.removeFromLeft (w));
    }

private:
    void rebuild()
    {
        sel.reset();
        selName = {};
        if (groups != nullptr && grp >= 0 && grp < (int) groups->size())
        {
            auto* G = (*groups)[(size_t) grp].get();
            if (trk >= 0 && trk < (int) G->tracks.size())
            {
                auto* t = G->tracks[(size_t) trk].get();
                selName = t->name;
                sel = std::make_unique<ChannelStrip> (false);
                sel->group = grp; sel->track = trk; sel->engineId = t->engineId;
                sel->engine = engine; sel->skin = skin;
                sel->name.setText (t->name, juce::dontSendNotification);
                sel->fader.setValue (t->volume, juce::dontSendNotification);
                sel->pan.setValue (t->pan, juce::dontSendNotification);
                sel->mute.setToggleState (t->mute, juce::dontSendNotification);
                sel->solo.setToggleState (t->solo, juce::dontSendNotification);
                if (engine != nullptr)
                    for (int i = 0, n = engine->trackPluginCount (t->engineId); i < n; ++i)
                        sel->fxNames.add (engine->trackPluginName (t->engineId, i));

                const int g2 = grp, t2 = trk;
                sel->onFader      = [this, g2, t2] (float v) { if (onVolume) onVolume (g2, t2, v); };
                sel->onPanChange  = [this, g2, t2] (float p) { if (onPan)  onPan  (g2, t2, p); };
                sel->onMuteToggle = [this, g2, t2] (bool b)  { if (onMute) onMute (g2, t2, b); };
                sel->onSoloToggle = [this, g2, t2] (bool b)  { if (onSolo) onSolo (g2, t2, b); };
                sel->onFxClick    = [this, g2, t2] { if (onFxMenu) onFxMenu (g2, t2); };
                addAndMakeVisible (*sel);
            }
        }
        resized();
        repaint();
    }

    AudioEngine* engine = nullptr;
    Skin skin = Skin::forDaw (Skin::Logic);
    const std::vector<std::unique_ptr<VideoGroup>>* groups = nullptr;
    int grp = -1, trk = -1;
    juce::Rectangle<int> paramArea;
    juce::String selName;
    std::unique_ptr<ChannelStrip> sel;
    ChannelStrip master { true };
};
