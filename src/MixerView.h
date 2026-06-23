#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <juce_gui_extra/juce_gui_extra.h>
#include "Track.h"
#include "AudioEngine.h"
#include "Skin.h"

//==============================================================================
/** One mixer channel strip: name, insert (FX) slots, pan, mute/solo, a volume
    fader and a live peak meter. The Master strip omits pan/mute/solo/inserts. */
struct ChannelStrip : public juce::Component
{
    int  group = -1, track = -1, engineId = -1;
    bool isMaster = false;
    AudioEngine* engine = nullptr;
    Skin skin = Skin::forDaw (Skin::Layback);
    float meter = 0.0f;

    juce::Slider fader, pan;
    juce::TextButton mute { "M" }, solo { "S" }, fx { "Inserts" };
    juce::Label name;

    std::function<void (float)> onFader, onPanChange;
    std::function<void (bool)>  onMuteToggle, onSoloToggle;
    std::function<void()>       onFxClick, onSelectClick;

    explicit ChannelStrip (bool master) : isMaster (master)
    {
        name.setJustificationType (juce::Justification::centred);
        name.setFont (juce::Font (juce::FontOptions().withHeight (11.0f)));
        addAndMakeVisible (name);

        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setRange (0.0, 1.4, 0.001);
        fader.setDoubleClickReturnValue (true, 1.0);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.onValueChange = [this] { if (onFader) onFader ((float) fader.getValue()); };
        addAndMakeVisible (fader);

        if (! master)
        {
            pan.setSliderStyle (juce::Slider::LinearHorizontal);
            pan.setRange (-1.0, 1.0, 0.01);
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            pan.onValueChange = [this] { if (onPanChange) onPanChange ((float) pan.getValue()); };
            addAndMakeVisible (pan);

            mute.setClickingTogglesState (true);
            mute.onClick = [this] { if (onMuteToggle) onMuteToggle (mute.getToggleState()); };
            solo.setClickingTogglesState (true);
            solo.onClick = [this] { if (onSoloToggle) onSoloToggle (solo.getToggleState()); };
            fx.onClick    = [this] { if (onFxClick) onFxClick(); };
            addAndMakeVisible (mute); addAndMakeVisible (solo); addAndMakeVisible (fx);
        }
    }

    void setMeter (float pk)
    {
        const float v = juce::jlimit (0.0f, 1.0f, pk);
        meter = juce::jmax (v, meter * 0.80f);   // fast attack, slow decay
        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override { if (onSelectClick) onSelectClick(); }

    int fxNamesY = 0;
    juce::Rectangle<int> meterBounds;

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        name.setBounds (r.removeFromTop (16));
        r.removeFromTop (4);
        if (! isMaster)
        {
            fx.setBounds (r.removeFromTop (18));
            fxNamesY = r.getY();
            r.removeFromTop (3 * 12 + 2);          // room for up to 3 insert names
            pan.setBounds (r.removeFromTop (16));
            r.removeFromTop (3);
            auto ms = r.removeFromTop (20);
            mute.setBounds (ms.removeFromLeft (ms.getWidth() / 2 - 1));
            ms.removeFromLeft (2);
            solo.setBounds (ms);
            r.removeFromTop (4);
        }
        auto meterCol = r.removeFromRight (10);
        meterBounds = meterCol.reduced (1);
        r.removeFromRight (3);
        fader.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (skin.control.darker (0.25f));
        g.fillRoundedRectangle (r.toFloat().reduced (1.0f), 4.0f);
        g.setColour (isMaster ? skin.accent : skin.audioStrip);     // colour cap
        g.fillRect (r.getX() + 3, r.getY() + 3, r.getWidth() - 6, 3);

        if (! isMaster && engine != nullptr && engineId >= 0)       // insert names
        {
            const int cnt = engine->trackPluginCount (engineId);
            g.setFont (juce::Font (juce::FontOptions().withHeight (9.5f)));
            for (int i = 0; i < juce::jmin (cnt, 3); ++i)
            {
                g.setColour (skin.text.withAlpha (0.85f));
                g.drawText (engine->trackPluginName (engineId, i),
                            r.getX() + 5, fxNamesY + i * 12, r.getWidth() - 10, 11,
                            juce::Justification::centredLeft, true);
            }
        }

        if (! meterBounds.isEmpty())                                // peak meter
        {
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRect (meterBounds);
            const int h = (int) (meterBounds.getHeight() * meter);
            const juce::Colour mc = meter > 0.92f ? juce::Colours::red
                                  : meter > 0.65f ? juce::Colours::orange
                                                  : juce::Colour (0xff4ad07a);
            g.setColour (mc);
            g.fillRect (meterBounds.getX(), meterBounds.getBottom() - h, meterBounds.getWidth(), h);
        }
    }
};

//==============================================================================
/** The mixer panel: a row of channel strips for the active group + a Master. */
class MixerView : public juce::Component
{
public:
    std::function<void (int, int, float)> onVolume;   // (group, track, linear gain)
    std::function<void (int, int, float)> onPan;      // (group, track, -1..1)
    std::function<void (int, int, bool)>  onMute;     // (group, track, newState)
    std::function<void (int, int, bool)>  onSolo;
    std::function<void (int, int)>        onFxMenu;
    std::function<void (int, int)>        onSelect;
    std::function<void (float)>           onMasterVolume;

    MixerView() = default;

    void setEngine (AudioEngine* e) { engine = e; }
    void setSkin (const Skin& s)
    {
        skin = s;
        for (auto& st : strips) { st->skin = s; st->repaint(); }
        repaint();
    }

    void setModel (const std::vector<std::unique_ptr<VideoGroup>>* g, int active)
    {
        groups = g; activeGroup = active;
        rebuild();
    }

    void rebuild()
    {
        strips.clear();
        if (groups != nullptr && activeGroup >= 0 && activeGroup < (int) groups->size())
        {
            auto* grp = (*groups)[(size_t) activeGroup].get();
            for (int t = 0; t < (int) grp->tracks.size(); ++t)
                addStrip (activeGroup, t, grp->tracks[(size_t) t].get(), false);
        }
        addStrip (-1, -1, nullptr, true);   // master
        resized();
        repaint();
    }

    // Refresh control values from the model without recreating strips (e.g. after a
    // mute/solo toggle from the timeline).
    void syncFromModel()
    {
        if (groups == nullptr) return;
        for (auto& s : strips)
        {
            if (s->isMaster) continue;
            if (activeGroup < 0 || activeGroup >= (int) groups->size()) continue;
            auto* grp = (*groups)[(size_t) activeGroup].get();
            if (s->track < 0 || s->track >= (int) grp->tracks.size()) continue;
            auto* tr = grp->tracks[(size_t) s->track].get();
            s->fader.setValue (tr->volume, juce::dontSendNotification);
            s->pan.setValue (tr->pan, juce::dontSendNotification);
            s->mute.setToggleState (tr->mute, juce::dontSendNotification);
            s->solo.setToggleState (tr->solo, juce::dontSendNotification);
            s->repaint();
        }
    }

    void updateMeters()
    {
        if (engine == nullptr) return;
        for (auto& s : strips)
            s->setMeter (s->isMaster ? engine->getMasterPeak()
                                     : (s->engineId >= 0 ? engine->getTrackPeak (s->engineId) : 0.0f));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (skin.panel);
        g.setColour (skin.windowBg.darker (0.4f));
        g.fillRect (0, 0, getWidth(), 1);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 6);
        const int n = (int) strips.size();
        if (n == 0) return;
        const int masterW = 92;
        const int tracks   = juce::jmax (1, n - 1);
        const int avail    = juce::jmax (0, r.getWidth() - masterW - 6);
        const int each     = juce::jlimit (58, 130, avail / tracks);
        for (auto& s : strips)
        {
            if (s->isMaster) s->setBounds (r.removeFromRight (masterW));
            else { s->setBounds (r.removeFromLeft (each)); r.removeFromLeft (2); }
        }
    }

private:
    void addStrip (int g, int t, AudioTrack* tr, bool master)
    {
        auto s = std::make_unique<ChannelStrip> (master);
        s->group = g; s->track = t; s->engineId = tr ? tr->engineId : -1;
        s->engine = engine; s->skin = skin;
        if (! master && tr != nullptr)
        {
            s->name.setText (tr->name, juce::dontSendNotification);
            s->fader.setValue (tr->volume, juce::dontSendNotification);
            s->pan.setValue (tr->pan, juce::dontSendNotification);
            s->mute.setToggleState (tr->mute, juce::dontSendNotification);
            s->solo.setToggleState (tr->solo, juce::dontSendNotification);
        }
        else
        {
            s->name.setText ("Master", juce::dontSendNotification);
            s->fader.setValue (engine != nullptr ? engine->getMasterGain() : 1.0, juce::dontSendNotification);
        }

        const int g2 = g, t2 = t;
        s->onFader      = [this, g2, t2, master] (float v) { if (master) { if (onMasterVolume) onMasterVolume (v); } else if (onVolume) onVolume (g2, t2, v); };
        s->onPanChange  = [this, g2, t2] (float p)  { if (onPan)  onPan  (g2, t2, p); };
        s->onMuteToggle = [this, g2, t2] (bool b)   { if (onMute) onMute (g2, t2, b); };
        s->onSoloToggle = [this, g2, t2] (bool b)   { if (onSolo) onSolo (g2, t2, b); };
        s->onFxClick    = [this, g2, t2] { if (onFxMenu) onFxMenu (g2, t2); };
        s->onSelectClick= [this, g2, t2] { if (onSelect) onSelect (g2, t2); };

        addAndMakeVisible (*s);
        strips.push_back (std::move (s));
    }

    const std::vector<std::unique_ptr<VideoGroup>>* groups = nullptr;
    int activeGroup = -1;
    AudioEngine* engine = nullptr;
    Skin skin = Skin::forDaw (Skin::Layback);
    std::vector<std::unique_ptr<ChannelStrip>> strips;
};
