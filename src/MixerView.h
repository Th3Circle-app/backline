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
    bool isBus = false;   // an output-bus strip (master-style layout, bus callbacks)
    int  busIdx = -1;
    int  group = -1, track = -1, engineId = -1;
    bool isMaster = false;
    AudioEngine* engine = nullptr;
    Skin skin = Skin::forDaw (Skin::Layback);
    float meter = 0.0f;
    float peakHold = 0.0f;       // slow-falling peak for the numeric readout + hold tick
    juce::StringArray fxNames;   // cached insert names (paint must not lock the audio engine)

    juce::Slider fader, pan;
    juce::TextButton mute { "M" }, solo { "S" };
    juce::Label name;

    std::function<void (float)> onFader, onPanChange;
    std::function<void (bool)>  onMuteToggle, onSoloToggle;
    std::function<void()>       onFxClick, onSelectClick;
    std::function<void (bool)>  onRecordArmToggle;   // track R cell
    std::function<void()>       onBounceClick;       // master Bnc cell
    std::function<void (float)> onSendChange;        // FX-bus send level
    std::function<void (int)>   onGroupChange;       // mixer link group (cycles 0..4)
    std::function<void()>       onOutputClick;       // output-routing slot (choose Master / bus)
    juce::String                outputLabel { "Stereo Out" };   // current output destination shown in the slot
    bool  recArmed = false;
    float sendLevel = 0.0f;
    int   groupId = 0;

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
        else
        {
            mute.setClickingTogglesState (true);
            mute.onClick = [this] { if (onMuteToggle) onMuteToggle (mute.getToggleState()); };
            addAndMakeVisible (mute);   // master shows M only
        }
    }

    void setMeter (float pk)
    {
        const float v = juce::jlimit (0.0f, 1.0f, pk);
        meter = juce::jmax (v, meter * 0.80f);                 // fast attack, slow decay
        peakHold = (v >= peakHold) ? v : juce::jmax (v, peakHold - 0.0035f);   // peak-hold, slow fall
        repaint();
    }

    void setSendFromX (int x)
    {
        sendLevel = juce::jlimit (0.0f, 1.0f, (float) (x - sendsRow.getX()) / (float) juce::jmax (1, sendsRow.getWidth()));
        if (onSendChange) onSendChange (sendLevel);
        repaint();
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if ((! isMaster || isBus) && insertArea.contains (e.getPosition())) { if (onFxClick) onFxClick(); return; }   // insert FX (tracks + buses)
        if (isMaster && ! isBus && riRow.contains (e.getPosition())) { if (onBounceClick) onBounceClick(); return; }   // Bnc -> bounce
        if (! isMaster && sendsRow.contains (e.getPosition())) { setSendFromX (e.x); return; }              // FX-bus send
        if (! isMaster && groupRow.contains (e.getPosition())) { groupId = (groupId + 1) % 5; if (onGroupChange) onGroupChange (groupId); repaint(); return; }   // cycle link group
        if (! isMaster && ! isBus && outputRow.contains (e.getPosition())) { if (onOutputClick) onOutputClick(); return; }   // choose output bus
        if (! isMaster && ! riRow.isEmpty())
        {
            auto rl = riRow; auto rR = rl.removeFromLeft (rl.getWidth() / 2 - 1);   // R cell = left half
            if (rR.contains (e.getPosition())) { recArmed = ! recArmed; if (onRecordArmToggle) onRecordArmToggle (recArmed); repaint(); return; }
        }
        if (onSelectClick) onSelectClick();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! isMaster && sendsRow.contains (e.getMouseDownPosition())) setSendFromX (e.x);
    }

    juce::Rectangle<int> meterBounds, tickArea, insertArea, valueBox, inRow, sendsRow, outputRow, groupRow, autoRow, riRow, peakLabel;

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        r.removeFromTop (5);                                // colour cap
        name.setBounds (r.removeFromBottom (15));
        {
            auto ms = r.removeFromBottom (18);             // M/S (master = M only)
            if (isMaster) mute.setBounds (ms.withSizeKeepingCentre (ms.getWidth() / 2, 18));
            else { mute.setBounds (ms.removeFromLeft (ms.getWidth() / 2 - 1)); ms.removeFromLeft (2); solo.setBounds (ms); }
        }
        r.removeFromBottom (2);
        riRow = r.removeFromBottom (14); r.removeFromBottom (3);   // R/I (track) or Bnc (master)

        insertArea = r.removeFromTop (38); r.removeFromTop (3);
        if (! isMaster) { inRow = r.removeFromTop (14); r.removeFromTop (2); sendsRow = r.removeFromTop (14); r.removeFromTop (2); }
        else            { inRow = {}; sendsRow = {}; }
        outputRow = r.removeFromTop (14); r.removeFromTop (2);
        groupRow  = r.removeFromTop (14); r.removeFromTop (2);
        autoRow   = r.removeFromTop (14); r.removeFromTop (3);
        if (! isMaster) { pan.setBounds (r.removeFromTop (24).withSizeKeepingCentre (24, 24)); r.removeFromTop (2); }
        valueBox = r.removeFromTop (13); r.removeFromTop (3);

        peakLabel = r.removeFromTop (11); r.removeFromTop (2);   // numeric peak-hold dB readout above the meter
        meterBounds = r.removeFromRight (9);
        r.removeFromRight (2);
        tickArea = r.removeFromRight (18);     // dB scale numbers
        r.removeFromRight (2);
        fader.setBounds (r);
    }

    // peak (0..1 linear) -> meter fill 0..1 over a 60 dB window (so the dB scale lines up)
    static float meterNorm (float pk)
    {
        if (pk <= 0.0f) return 0.0f;
        const float db = 20.0f * std::log10 (pk);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (skin.control.darker (0.25f));
        g.fillRoundedRectangle (r.toFloat().reduced (1.0f), 4.0f);
        g.setColour (isMaster ? skin.accent : skin.audioStrip);      // colour cap
        g.fillRect (r.getX() + 3, r.getY() + 3, r.getWidth() - 6, 3);

        if (! insertArea.isEmpty())                                  // insert slots (EQ + FX / Mastering)
        {
            auto ia = insertArea;
            for (int i = 0; i < 2; ++i)
            {
                auto slot = ia.removeFromTop (17).reduced (2, 1); ia.removeFromTop (1);
                g.setColour (skin.control.brighter (0.04f)); g.fillRoundedRectangle (slot.toFloat(), 3.0f);
                g.setColour (skin.windowBg.darker (0.2f));   g.drawRoundedRectangle (slot.toFloat(), 3.0f, 1.0f);
                const juce::String label = (i < fxNames.size()) ? fxNames[i]
                                         : (i == 0 ? juce::String ("EQ") : (isMaster ? juce::String ("Mastering") : juce::String()));
                g.setColour (label.isEmpty() ? skin.muted.withAlpha (0.5f) : skin.text);
                g.setFont (juce::Font (juce::FontOptions().withHeight (9.5f)));
                g.drawText (label.isEmpty() ? juce::String ("-") : label, slot.reduced (5, 0), juce::Justification::centredLeft, true);
            }
        }

        auto rowSlot = [&] (juce::Rectangle<int> b, const juce::String& t, juce::Colour txt, bool link, bool knob, bool disabled)
        {
            if (b.isEmpty()) return;
            if (disabled)   // read-only routing label (no control behaviour yet) -> draw flat so it doesn't read as a button
            {
                g.setColour (skin.muted.withAlpha (0.5f));
                g.setFont (juce::Font (juce::FontOptions().withHeight (9.5f)));
                g.drawText (t, b.toFloat().withTrimmedLeft (6.0f), juce::Justification::centredLeft, true);
                return;
            }
            auto rf = b.toFloat().reduced (2.0f, 1.0f);
            g.setColour (skin.control.brighter (0.04f)); g.fillRoundedRectangle (rf, 3.0f);
            g.setColour (skin.windowBg.darker (0.2f));   g.drawRoundedRectangle (rf, 3.0f, 1.0f);
            float lpad = 6.0f;
            if (link) { g.setColour (skin.muted); g.drawEllipse (rf.getX() + 3.0f, rf.getCentreY() - 3.0f, 6.0f, 6.0f, 1.1f); g.drawEllipse (rf.getX() + 7.0f, rf.getCentreY() - 3.0f, 6.0f, 6.0f, 1.1f); lpad = 18.0f; }
            if (knob) { g.setColour (skin.windowBg.darker (0.2f)); g.fillEllipse (rf.getRight() - 11.0f, rf.getCentreY() - 4.0f, 8.0f, 8.0f); g.setColour (skin.muted); g.drawEllipse (rf.getRight() - 11.0f, rf.getCentreY() - 4.0f, 8.0f, 8.0f, 1.0f); }
            g.setColour (txt); g.setFont (juce::Font (juce::FontOptions().withHeight (9.5f)));
            g.drawText (t, b.toFloat().withTrimmedLeft (lpad), juce::Justification::centredLeft, true);
        };
        rowSlot (inRow,     "In 1-2",     skin.text,  true,  false, true);   // input routing: read-only label for now
        rowSlot (autoRow,   "Read",       juce::Colour (0xff5fbf6f), false, false, true);

        if (! outputRow.isEmpty() && ! isMaster && ! isBus)   // output-routing slot (Logic-style), click to choose Master/bus
        {
            auto rf = outputRow.toFloat().reduced (2.0f, 1.0f);
            g.setColour (skin.control.brighter (0.06f)); g.fillRoundedRectangle (rf, 3.0f);
            g.setColour (skin.accent.withAlpha (0.55f));  g.drawRoundedRectangle (rf, 3.0f, 1.0f);
            g.setColour (skin.text); g.setFont (juce::Font (juce::FontOptions().withHeight (9.5f)));
            g.drawText (outputLabel, rf.withTrimmedLeft (6.0f), juce::Justification::centredLeft, true);
        }
        else rowSlot (outputRow, "Stereo Out", skin.text, false, false, true);

        if (! groupRow.isEmpty() && ! isMaster)   // mixer link group (click to cycle)
        {
            const juce::Colour grpCols[] = { skin.muted, juce::Colour (0xffe0913d), juce::Colour (0xff5aa0d0), juce::Colour (0xff7bbf5a), juce::Colour (0xffd06fb0) };
            auto rf = groupRow.toFloat().reduced (2.0f, 1.0f);
            g.setColour (skin.control.brighter (0.04f)); g.fillRoundedRectangle (rf, 3.0f);
            g.setColour (skin.windowBg.darker (0.2f));   g.drawRoundedRectangle (rf, 3.0f, 1.0f);
            if (groupId > 0) { g.setColour (grpCols[groupId].withAlpha (0.85f)); g.fillRoundedRectangle (rf.removeFromLeft (5.0f), 2.0f); }
            g.setColour (groupId > 0 ? skin.text : skin.muted.withAlpha (0.6f));
            g.setFont (juce::Font (juce::FontOptions().withHeight (9.5f)));
            g.drawText (groupId > 0 ? ("Group " + juce::String (groupId)) : "Group", groupRow.toFloat().withTrimmedLeft (8.0f), juce::Justification::centredLeft, true);
        }

        if (! sendsRow.isEmpty() && ! isMaster)   // FX-bus send level (drag to set)
        {
            auto rf = sendsRow.toFloat().reduced (2.0f, 1.0f);
            g.setColour (skin.control.brighter (0.04f)); g.fillRoundedRectangle (rf, 3.0f);
            g.setColour (skin.accent.withAlpha (0.55f));  g.fillRoundedRectangle (rf.withWidth (rf.getWidth() * juce::jlimit (0.0f, 1.0f, sendLevel)), 3.0f);
            g.setColour (skin.windowBg.darker (0.2f));    g.drawRoundedRectangle (rf, 3.0f, 1.0f);
            g.setColour (skin.text); g.setFont (juce::Font (juce::FontOptions().withHeight (9.5f)));
            g.drawText ("Send " + juce::String ((int) (sendLevel * 100.0f)), sendsRow.toFloat().withTrimmedLeft (6.0f), juce::Justification::centredLeft, true);
        }

        if (! valueBox.isEmpty())                                    // live fader dB readout
        {
            const double v  = fader.getValue();
            const float  db = 20.0f * (float) std::log10 (juce::jmax (1.0e-4, v));
            g.setColour (juce::Colour (0xff2a2a2a)); g.fillRoundedRectangle (valueBox.toFloat(), 2.0f);
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::plain)));
            g.setColour (db > 0.05f ? juce::Colours::red : juce::Colour (0xff7fcf7f));
            g.drawText (juce::String (db, 1) + " dB", valueBox, juce::Justification::centred, false);
        }

        if (! peakLabel.isEmpty())                                   // numeric peak-hold dB readout
        {
            const juce::String txt = peakHold <= 0.0002f ? juce::String ("-\xe2\x88\x9e")
                                                         : juce::String (20.0f * std::log10 (peakHold), 1);
            g.setColour (peakHold > 0.96f ? juce::Colours::red : skin.text);
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 8.5f, juce::Font::plain)));
            g.drawText (txt + " dB", peakLabel, juce::Justification::centred, false);
        }

        if (! tickArea.isEmpty())                                    // dB level scale (shared by fader + meter)
        {
            g.setColour (skin.muted.withAlpha (0.9f));
            g.setFont (juce::Font (juce::FontOptions().withHeight (7.5f)));
            for (int m : { 0, 6, 12, 24, 48 })
            {
                const int y = tickArea.getY() + (int) (((float) m / 60.0f) * tickArea.getHeight());
                g.drawText (m == 0 ? "0" : "-" + juce::String (m), tickArea.getX(), y, tickArea.getWidth(), 9, juce::Justification::centredRight, false);
            }
        }

        if (! meterBounds.isEmpty())                                 // dB-scaled segmented peak meter
        {
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRect (meterBounds);
            const int segs = juce::jmax (1, meterBounds.getHeight() / 4);
            const int lit  = (int) (meterNorm (meter) * (float) segs);
            for (int s = 0; s < lit; ++s)
            {
                const float f = (float) s / (float) segs;
                g.setColour (f > 0.96f ? juce::Colours::red : f > 0.85f ? juce::Colour (0xffe0b020) : juce::Colour (0xff4ad07a));
                g.fillRect (meterBounds.getX(), meterBounds.getBottom() - (s + 1) * 4 + 1, meterBounds.getWidth(), 3);
            }
            const int phy = meterBounds.getBottom() - (int) (meterNorm (peakHold) * (float) meterBounds.getHeight());   // peak-hold tick
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.fillRect (meterBounds.getX(), juce::jlimit (meterBounds.getY(), meterBounds.getBottom() - 1, phy), meterBounds.getWidth(), 1);
        }

        if (! riRow.isEmpty())                                       // R/I (track) or Bnc (master)
        {
            if (isMaster)
            {
                auto bb = riRow.toFloat().reduced (2.0f, 1.0f);
                g.setColour (skin.control.brighter (0.04f)); g.fillRoundedRectangle (bb, 3.0f);
                g.setColour (skin.windowBg.darker (0.2f));   g.drawRoundedRectangle (bb, 3.0f, 1.0f);
                g.setColour (skin.text); g.setFont (9.5f); g.drawText ("Bnc", riRow, juce::Justification::centred, false);
            }
            else
            {
                auto rl = riRow; auto rR = rl.removeFromLeft (rl.getWidth() / 2 - 1); rl.removeFromLeft (2);
                auto cell = [&] (juce::Rectangle<int> b, const char* tx, juce::Colour col, bool on)
                { g.setColour (on ? col : skin.control.darker (0.1f)); g.fillRoundedRectangle (b.toFloat().reduced (1.0f), 2.5f);
                  g.setColour (on ? juce::Colours::white : col); g.setFont (9.5f); g.drawText (tx, b, juce::Justification::centred, false); };
                cell (rR, "R", juce::Colour (0xffc8281a), recArmed);   // record-arm (lit when armed)
                cell (rl, "I", skin.muted.withAlpha (0.5f), false);    // input routing: read-only for now
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
    std::function<void (int, int, bool)>  onRecordArm;   // (group, track, armed)
    std::function<void ()>                onBounce;       // master Bnc -> export/bounce
    std::function<void (int, int, float)> onSend;         // (group, track, FX-bus send level)
    std::function<void (int, int, int)>   onGroupChange;   // (group, track, link-group id)
    std::function<void (float)>           onMasterVolume;
    std::function<void (bool)>            onMasterMute;
    std::function<void (int, float)>      onBusFader;   // (busIdx, gain)
    std::function<void (int, bool)>       onBusMute;    // (busIdx, muted)
    std::function<void (int)>             onBusFx;      // (busIdx) -> open bus FX menu
    std::function<void (int, int)>        onOutputMenu; // (group, track) -> choose output (Master / bus)

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
        if (engine != nullptr)              // output-bus strips, between tracks and master
            for (int b = 0, bc = engine->busCount(); b < bc; ++b)
                addBusStrip (b);
        addStrip (-1, -1, nullptr, true);   // master
        resized();
        repaint();
    }

    void addBusStrip (int b)
    {
        auto s = std::make_unique<ChannelStrip> (true);   // master-style layout
        s->isBus = true; s->busIdx = b; s->engine = engine; s->skin = skin;
        s->name.setText (engine != nullptr ? engine->busName (b) : ("Backline " + juce::String (b + 1)), juce::dontSendNotification);
        s->fader.setValue (engine != nullptr ? engine->getBusGain (b) : 1.0, juce::dontSendNotification);
        s->mute.setToggleState (engine != nullptr && engine->getBusMute (b), juce::dontSendNotification);
        for (int i = 0, cnt = engine != nullptr ? engine->busPluginCount (b) : 0; i < cnt; ++i) s->fxNames.add (engine->busPluginName (b, i));
        s->onFader      = [this, b] (float v) { if (onBusFader) onBusFader (b, v); };
        s->onMuteToggle = [this, b] (bool m)  { if (onBusMute)  onBusMute  (b, m); };
        s->onFxClick    = [this, b] { if (onBusFx) onBusFx (b); };
        addAndMakeVisible (*s);
        strips.push_back (std::move (s));
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
            s->sendLevel = tr->send;
            s->outputLabel = (tr->output < 0) ? juce::String ("Stereo Out")
                           : (engine != nullptr ? engine->busName (tr->output) : ("Backline " + juce::String (tr->output + 1)));
            s->repaint();
        }
    }

    void updateMeters()
    {
        if (engine == nullptr) return;
        for (auto& s : strips)
        {
            const float pk = s->isBus    ? engine->getBusPeak (s->busIdx)
                           : s->isMaster ? engine->getMasterPeak()
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
            s->recArmed = tr->recordArm;
            s->sendLevel = tr->send;
            s->groupId = tr->mixGroup;
            s->outputLabel = (tr->output < 0) ? juce::String ("Stereo Out")
                           : (engine != nullptr ? engine->busName (tr->output) : ("Backline " + juce::String (tr->output + 1)));
        }
        else
        {
            s->name.setText ("Master", juce::dontSendNotification);
            s->fader.setValue (engine != nullptr ? engine->getMasterGain() : 1.0, juce::dontSendNotification);
            s->mute.setToggleState (engine != nullptr && engine->getMasterMute(), juce::dontSendNotification);
        }

        const int g2 = g, t2 = t;
        s->onFader      = [this, g2, t2, master] (float v) { if (master) { if (onMasterVolume) onMasterVolume (v); } else if (onVolume) onVolume (g2, t2, v); };
        s->onPanChange  = [this, g2, t2] (float p)  { if (onPan)  onPan  (g2, t2, p); };
        s->onMuteToggle = [this, g2, t2, master] (bool b) { if (master) { if (onMasterMute) onMasterMute (b); } else if (onMute) onMute (g2, t2, b); };
        s->onSoloToggle = [this, g2, t2] (bool b)   { if (onSolo) onSolo (g2, t2, b); };
        s->onFxClick    = [this, g2, t2] { if (onFxMenu) onFxMenu (g2, t2); };
        s->onSelectClick= [this, g2, t2] { if (onSelect) onSelect (g2, t2); };
        s->onRecordArmToggle = [this, g2, t2] (bool b) { if (onRecordArm) onRecordArm (g2, t2, b); };
        s->onBounceClick     = [this] { if (onBounce) onBounce(); };
        s->onSendChange      = [this, g2, t2] (float v) { if (onSend) onSend (g2, t2, v); };
        s->onGroupChange     = [this, g2, t2] (int gid) { if (onGroupChange) onGroupChange (g2, t2, gid); };
        s->onOutputClick     = [this, g2, t2] { if (onOutputMenu) onOutputMenu (g2, t2); };

        addAndMakeVisible (*s);
        strips.push_back (std::move (s));
    }

    const std::vector<std::unique_ptr<VideoGroup>>* groups = nullptr;
    int activeGroup = -1;
    AudioEngine* engine = nullptr;
    Skin skin = Skin::forDaw (Skin::Layback);
    std::vector<std::unique_ptr<ChannelStrip>> strips;
};
