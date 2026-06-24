#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <cmath>
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
    juce::StringArray fxNames;   // cached insert names (paint must not lock the audio engine)

    juce::Slider fader, pan;
    juce::TextButton mute { "M" }, solo { "S" };
    juce::Label name;

    std::function<void (float)> onFader, onPanChange;
    std::function<void (bool)>  onMuteToggle, onSoloToggle;
    std::function<void()>       onFxClick, onSelectClick;

    explicit ChannelStrip (bool master) : isMaster (master)
    {
        name.setJustificationType (juce::Justification::centred);
        name.setFont (juce::Font (juce::FontOptions().withHeight (10.5f)));
        addAndMakeVisible (name);

        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setRange (0.0, 1.4, 0.001);
        fader.setDoubleClickReturnValue (true, 1.0);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.onValueChange = [this] { if (onFader) onFader ((float) fader.getValue()); repaint(); };
        addAndMakeVisible (fader);

        if (! master)
        {
            pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            pan.setRange (-1.0, 1.0, 0.01);
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            pan.onValueChange = [this] { if (onPanChange) onPanChange ((float) pan.getValue()); };
            addAndMakeVisible (pan);

            mute.setClickingTogglesState (true);
            mute.onClick = [this] { if (onMuteToggle) onMuteToggle (mute.getToggleState()); };
            solo.setClickingTogglesState (true);
            solo.onClick = [this] { if (onSoloToggle) onSoloToggle (solo.getToggleState()); };
            addAndMakeVisible (mute); addAndMakeVisible (solo);
        }
    }

    void setMeter (float pk)
    {
        const float v = juce::jlimit (0.0f, 1.0f, pk);
        meter = juce::jmax (v, meter * 0.80f);   // fast attack, slow decay
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! isMaster && insertArea.contains (e.getPosition())) { if (onFxClick) onFxClick(); return; }
        if (onSelectClick) onSelectClick();
    }

    juce::Rectangle<int> meterBounds, tickArea, insertArea, valueBox, inRow, sendsRow, outputRow, autoRow;

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        r.removeFromTop (5);                                // colour cap
        name.setBounds (r.removeFromBottom (15));
        if (! isMaster)
        {
            auto ms = r.removeFromBottom (18);
            mute.setBounds (ms.removeFromLeft (ms.getWidth() / 2 - 1));
            ms.removeFromLeft (2);
            solo.setBounds (ms);
            r.removeFromBottom (4);

            insertArea = r.removeFromTop (38); r.removeFromTop (3);
            inRow      = r.removeFromTop (14); r.removeFromTop (2);
            sendsRow   = r.removeFromTop (14); r.removeFromTop (2);
            outputRow  = r.removeFromTop (14); r.removeFromTop (2);
            autoRow    = r.removeFromTop (14); r.removeFromTop (3);
            pan.setBounds (r.removeFromTop (24).withSizeKeepingCentre (24, 24)); r.removeFromTop (2);
            valueBox = r.removeFromTop (13); r.removeFromTop (3);
        }
        meterBounds = r.removeFromRight (9);
        r.removeFromRight (2);
        tickArea = r.removeFromRight (16);
        r.removeFromRight (2);
        fader.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (skin.control.darker (0.25f));
        g.fillRoundedRectangle (r.toFloat().reduced (1.0f), 4.0f);
        g.setColour (isMaster ? skin.accent : skin.audioStrip);      // colour cap
        g.fillRect (r.getX() + 3, r.getY() + 3, r.getWidth() - 6, 3);

        if (! isMaster && ! insertArea.isEmpty())                    // insert slots
        {
            auto ia = insertArea;
            for (int i = 0; i < 2; ++i)
            {
                auto slot = ia.removeFromTop (17).reduced (2, 1); ia.removeFromTop (1);
                g.setColour (skin.control.brighter (0.04f));
                g.fillRoundedRectangle (slot.toFloat(), 3.0f);
                g.setColour (skin.windowBg.darker (0.2f));
                g.drawRoundedRectangle (slot.toFloat(), 3.0f, 1.0f);
                const juce::String label = (i < fxNames.size()) ? fxNames[i] : (i == 0 ? juce::String ("EQ") : juce::String());
                g.setColour (label.isEmpty() ? skin.muted.withAlpha (0.5f) : skin.text);
                g.setFont (juce::Font (juce::FontOptions().withHeight (9.5f)));
                g.drawText (label.isEmpty() ? juce::String ("-") : label, slot.reduced (5, 0), juce::Justification::centredLeft, true);
            }

            auto rowSlot = [&] (juce::Rectangle<int> b, const juce::String& t, juce::Colour txt)   // Logic strip rows
            {
                if (b.isEmpty()) return;
                auto rf = b.toFloat().reduced (2.0f, 1.0f);
                g.setColour (skin.control.brighter (0.04f));
                g.fillRoundedRectangle (rf, 3.0f);
                g.setColour (skin.windowBg.darker (0.2f));
                g.drawRoundedRectangle (rf, 3.0f, 1.0f);
                g.setColour (txt);
                g.setFont (juce::Font (juce::FontOptions().withHeight (9.5f)));
                g.drawText (t, b.reduced (6, 0), juce::Justification::centredLeft, true);
            };
            rowSlot (inRow,     "In 1-2",     skin.text);
            rowSlot (sendsRow,  "Sends",      skin.muted);
            rowSlot (outputRow, "Stereo Out", skin.text);
            rowSlot (autoRow,   "Read",       juce::Colour (0xff5fbf6f));
        }

        if (! valueBox.isEmpty())                                    // two dB value boxes (input | output)
        {
            auto vbx = valueBox;
            auto right = vbx.removeFromRight (vbx.getWidth() / 2 - 1);
            vbx.removeFromRight (2);
            const double v  = fader.getValue();
            const float  db = 20.0f * (float) std::log10 (juce::jmax (1.0e-4, v));
            g.setColour (juce::Colour (0xff2a2a2a));
            g.fillRoundedRectangle (vbx.toFloat(), 2.0f);
            g.fillRoundedRectangle (right.toFloat(), 2.0f);
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::plain)));
            g.setColour (skin.text);
            g.drawText ("0.0", vbx, juce::Justification::centred, false);
            g.setColour (db > 0.05f ? juce::Colours::red : juce::Colour (0xff7fcf7f));
            g.drawText (juce::String (db, 1), right, juce::Justification::centred, false);
        }

        if (! tickArea.isEmpty())                                    // dB scale beside the fader
        {
            g.setColour (skin.muted.withAlpha (0.65f));
            g.setFont (juce::Font (juce::FontOptions().withHeight (7.5f)));
            for (int m : { 0, 6, 12, 24, 48 })
            {
                const int y = tickArea.getY() + (int) (((float) m / 60.0f) * tickArea.getHeight());
                g.drawText (juce::String (m), tickArea.getX(), y, tickArea.getWidth(), 9, juce::Justification::centredRight, false);
            }
        }

        if (! meterBounds.isEmpty())                                 // segmented peak meter
        {
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRect (meterBounds);
            const int segs = juce::jmax (1, meterBounds.getHeight() / 4);
            const int lit  = (int) (meter * (float) segs);
            for (int s = 0; s < lit; ++s)
            {
                const float f = (float) s / (float) segs;
                g.setColour (f > 0.9f ? juce::Colours::red : f > 0.66f ? juce::Colour (0xffe0b020) : juce::Colour (0xff4ad07a));
                g.fillRect (meterBounds.getX(), meterBounds.getBottom() - (s + 1) * 4 + 1, meterBounds.getWidth(), 3);
            }
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
        {
            const float pk = s->isMaster ? engine->getMasterPeak()
                                         : (s->engineId >= 0 ? engine->getTrackPeak (s->engineId) : 0.0f);
            if (pk >= 0.0f) s->setMeter (pk);   // -1 => the audio lock was busy this tick; hold last
        }
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
        if (! master && engine != nullptr && s->engineId >= 0)       // cache insert names once (off the paint path)
            for (int i = 0, cnt = engine->trackPluginCount (s->engineId); i < cnt; ++i)
                s->fxNames.add (engine->trackPluginName (s->engineId, i));
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
