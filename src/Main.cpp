#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <map>
#include <juce_gui_extra/juce_gui_extra.h>
#include "VideoView.h"
#include "TimelineComponent.h"
#include "AudioEngine.h"
#include "FfmpegTool.h"
#include "Clip.h"
#include "Track.h"
#include "LaybackLookAndFeel.h"
#include "MixerView.h"
#include "LogicControlBar.h"
#include "LogicInspector.h"

//==============================================================================
// Multi-video: a project is a stack of video groups (each = one video + its own
// audio tracks). One group is "active": its video previews and its audio plays.
// The audio engine holds every track's audio in RAM; only the active group's
// tracks carry clips (others are silent), so switching groups is instant.
static juce::var doublesToVar (const std::vector<double>& v) { juce::var a; for (double d : v) a.append (d); return a; }
static std::vector<double> varToDoubles (const juce::var& v) { std::vector<double> r; if (auto* arr = v.getArray()) for (auto& e : *arr) r.push_back ((double) e); return r; }

struct EditSnapshot
{
    struct TrackS { bool mute = false, solo = false; std::vector<AudioClip> clips; };
    struct GroupS { bool vMute = false, vSolo = false; std::vector<TrackS> tracks; };
    std::vector<GroupS> groups;
    bool loopEnabled = false; double loopStart = 0.0, loopEnd = 0.0;
    int selGroup = -1, selTrack = -1, selClip = -1;
};

namespace LSCmd { enum { TogglePlay = 0x6001, Split, DeleteClip, ToggleLoop, ToggleSnap, NudgeLeft, NudgeRight, Undo, Redo }; }

//==============================================================================
/** A floating window that hosts one effect's editor (native generic UI or the
    plugin's own UI). The processor owns the editor; this just frames it. */
struct PluginWindow : public juce::DocumentWindow
{
    juce::AudioProcessor* proc = nullptr;
    std::function<void()> onClose;

    PluginWindow (juce::AudioProcessor* p, juce::Colour bg)
        : juce::DocumentWindow (p->getName(), bg, juce::DocumentWindow::closeButton), proc (p)
    {
        setUsingNativeTitleBar (true);
        if (auto* ed = p->createEditorIfNeeded())
        {
            setContentOwned (ed, true);          // editor dtor notifies the processor (editorBeingDeleted)
            setResizable (ed->isResizable(), false);
        }
        else
        {
            setContentOwned (new juce::GenericAudioProcessorEditor (*p), true);
            setResizable (true, false);
        }
        setTopLeftPosition (160, 140);
        setVisible (true);
    }

    void closeButtonPressed() override { if (onClose) onClose(); }
};

//==============================================================================
/** Floating pop-out that shows the synced video while you scrub the timeline,
    so the main window stays a pure DAW surface. */
class VideoWindow : public juce::DocumentWindow
{
public:
    struct Content : public juce::Component
    {
        VideoView* vid = nullptr;
        void attach (VideoView* v) { vid = v; if (v != nullptr) addAndMakeVisible (v); resized(); }
        void resized() override { if (vid != nullptr) vid->setBounds (getLocalBounds()); }
        void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }
    };

    VideoWindow (VideoView& v, std::function<void()> onCloseClicked)
        : juce::DocumentWindow ("Video", juce::Colours::black, juce::DocumentWindow::closeButton),
          onCloseCb (std::move (onCloseClicked))
    {
        setUsingNativeTitleBar (true);
        content.attach (&v);
        setContentNonOwned (&content, false);   // the VideoView is owned by MainComponent, not this window
        setResizable (true, false);
        setSize (640, 380);
    }

    void closeButtonPressed() override { if (onCloseCb) onCloseCb(); }

    Content content;
    std::function<void()> onCloseCb;
};

enum class KeyProfile { Layback, Logic, ProTools, Ableton };

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ChangeListener,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel
{
public:
    MainComponent()
    {
        setLookAndFeel (&laf);
        juce::LookAndFeel::setDefaultLookAndFeel (&laf);   // theme popup menus too

        videoButton.setButtonText ("Video");
        videoButton.setClickingTogglesState (true);
        videoButton.onClick = [this] { showVideoWindow (videoButton.getToggleState()); };
        addAndMakeVisible (videoButton);
        videoWindow = std::make_unique<VideoWindow> (video, [this] { showVideoWindow (false); });

        logicBar.onRewind = [this] { seekAll (0.0); };
        logicBar.onStop   = [this] { pauseAll(); seekAll (0.0); };
        logicBar.onPlay   = [this] { togglePlay(); };
        logicBar.onRecord = [this] { /* recording is Phase C */ };
        logicBar.onCycle  = [this] { loopToggle.setToggleState (! loopEnabled, juce::dontSendNotification); toggleLoop(); };
        logicBar.isPlaying = [this] { return playing; };
        logicBar.isCycle   = [this] { return loopEnabled; };
        addChildComponent (logicBar);

        logicInspector.setEngine (&audioEngine);
        logicInspector.onVolume = [this] (int g, int t, float v) { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->volume = v; updateTrackGain (g, t); if (mixerVisible) mixerView.syncFromModel(); } };
        logicInspector.onPan    = [this] (int g, int t, float p) { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->pan = p; audioEngine.setTrackPan (groups[(size_t) g]->tracks[(size_t) t]->engineId, p); if (mixerVisible) mixerView.syncFromModel(); } };
        logicInspector.onMute   = [this] (int g, int t, bool b)  { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->mute = b; applyMixGains(); timeline.repaint(); if (mixerVisible) mixerView.syncFromModel(); } };
        logicInspector.onSolo   = [this] (int g, int t, bool b)  { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->solo = b; applyMixGains(); timeline.repaint(); if (mixerVisible) mixerView.syncFromModel(); } };
        logicInspector.onFxMenu = [this] (int g, int t) { showTrackMenu (g, t); };
        logicInspector.onMasterVolume = [this] (float v) { audioEngine.setMasterGain (v); };
        addChildComponent (logicInspector);

        openButton.setButtonText ("Add Video...");
        openButton.onClick = [this] { openAddVideo(); };
        addAndMakeVisible (openButton);

        playButton.setButtonText ("Play");
        playButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible (playButton);

        loopToggle.setButtonText ("Loop");
        loopToggle.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
        loopToggle.onClick = [this] { toggleLoop(); };
        addAndMakeVisible (loopToggle);

        snapToggle.setButtonText ("Snap");
        snapToggle.setToggleState (true, juce::dontSendNotification);
        snapToggle.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
        snapToggle.onClick = [this] { timeline.setSnapEnabled (snapToggle.getToggleState()); };
        addAndMakeVisible (snapToggle);

        exportButton.setButtonText ("Export");
        exportButton.onClick = [this] { showExportMenu(); };
        addAndMakeVisible (exportButton);

        timeLabel.setJustificationType (juce::Justification::centred);
        timeLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::plain)));
        timeLabel.setColour (juce::Label::textColourId,       juce::Colour (0xff8fd6ff));
        timeLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff0b0d11));
        timeLabel.setColour (juce::Label::outlineColourId,    juce::Colour (0xff2b303b));
        addAndMakeVisible (timeLabel);

        titleLabel.setText ("Layback Station", juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions().withHeight (14.0f)));
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0a6));
        addAndMakeVisible (titleLabel);

        timeline.onSeek          = [this] (double s) { seekAll (s); };
        timeline.onScrubStart    = [this] { isScrubbing = true;  wasPlaying = playing; pauseAll(); };
        timeline.onScrubEnd      = [this] { isScrubbing = false; if (wasPlaying) playAll(); };
        timeline.onEditBegin     = [this] { pushUndo(); };
        timeline.onClipChanged   = [this] (int g, int t, int c, AudioClip nc) { clipChanged (g, t, c, nc); };
        timeline.onClipMenu      = [this] (int g, int t, int c, double tm) { showClipMenu (g, t, c, tm); };
        timeline.onClipSelected  = [this] (int g, int t, int c) { selGroup = g; selTrack = t; selClip = c; timeline.setSelection (g, t, c); refreshInspector(); };
        timeline.onToggleExpand  = [this] (int g) { if (validGroup (g)) { groups[(size_t) g]->expanded = ! groups[(size_t) g]->expanded; resized(); timeline.repaint(); } };
        timeline.onImportTrack   = [this] (int g) { importTrack (g); };
        timeline.onTrackMute     = [this] (int g, int t) { if (validTrack (g, t)) { auto& tr = *groups[(size_t) g]->tracks[(size_t) t]; tr.mute = ! tr.mute; applyMixGains(); timeline.repaint(); mixerView.syncFromModel(); } };
        timeline.onTrackSolo     = [this] (int g, int t) { if (validTrack (g, t)) { auto& tr = *groups[(size_t) g]->tracks[(size_t) t]; tr.solo = ! tr.solo; applyMixGains(); timeline.repaint(); mixerView.syncFromModel(); } };
        timeline.onTrackRecord   = [this] (int g, int t) { if (validTrack (g, t)) { auto& tr = *groups[(size_t) g]->tracks[(size_t) t]; tr.recordArm = ! tr.recordArm; timeline.repaint(); } };
        timeline.onTrackVolume   = [this] (int g, int t, float v) { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->volume = v; updateTrackGain (g, t); if (mixerVisible) mixerView.syncFromModel(); refreshInspector(); } };
        timeline.onVideoMute     = [this] (int g) { if (validGroup (g)) { groups[(size_t) g]->videoMute = ! groups[(size_t) g]->videoMute; applyMixGains(); timeline.repaint(); } };
        timeline.onVideoSolo     = [this] (int g) { if (validGroup (g)) { groups[(size_t) g]->videoSolo = ! groups[(size_t) g]->videoSolo; applyMixGains(); timeline.repaint(); } };
        timeline.onActivateGroup = [this] (int g) { activateGroup (g); };
        timeline.onAddVideo      = [this] { openAddVideo(); };
        timeline.onLoopChanged   = [this] (bool en, double s, double e)
        {
            if (en && e <= s) { en = false; s = e = 0.0; }   // never store a degenerate loop as enabled
            loopEnabled = en; loopStart = s; loopEnd = e;
            loopToggle.setToggleState (en, juce::dontSendNotification);
        };
        timeline.onLoopMenu      = [this] (double t) { showLoopMenu (t); };
        timeline.onTrackMenu     = [this] (int g, int t) { showTrackMenu (g, t); };
        timeline.onGroupMenu     = [this] (int g) { showGroupMenu (g); };
        timeline.setGroups (&groups);
        timelineViewport.setViewedComponent (&timeline, false);
        timelineViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (timelineViewport);

        mixerView.setEngine (&audioEngine);
        mixerView.onVolume = [this] (int g, int t, float v) { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->volume = v; updateTrackGain (g, t); } };
        mixerView.onPan    = [this] (int g, int t, float p) { if (validTrack (g, t)) { auto& tr = *groups[(size_t) g]->tracks[(size_t) t]; tr.pan = p; audioEngine.setTrackPan (tr.engineId, p); } };
        mixerView.onMute   = [this] (int g, int t, bool b)  { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->mute = b; applyMixGains(); timeline.repaint(); } };
        mixerView.onSolo   = [this] (int g, int t, bool b)  { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->solo = b; applyMixGains(); timeline.repaint(); } };
        mixerView.onFxMenu = [this] (int g, int t) { showTrackMenu (g, t); };
        mixerView.onSelect = [this] (int g, int t) { selGroup = g; selTrack = t; selClip = -1; timeline.setSelection (g, t, -1); };
        mixerView.onMasterVolume = [this] (float v) { audioEngine.setMasterGain (v); };
        addChildComponent (mixerView);   // shown when toggled

        keysButton.onClick = [this] { showKeysMenu(); };
        addAndMakeVisible (keysButton);

        projectButton.setButtonText ("Project");
        projectButton.onClick = [this] { showProjectMenu(); };
        addAndMakeVisible (projectButton);

        for (auto* c : std::initializer_list<juce::Component*> { &openButton, &playButton, &exportButton, &keysButton, &projectButton, &loopToggle, &snapToggle, &videoButton })
            c->setWantsKeyboardFocus (false);

        commandManager.registerAllCommandsForTarget (this);
        addKeyListener (commandManager.getKeyMappings());
        setWantsKeyboardFocus (true);
        applyKeyProfile (KeyProfile::Logic);   // boot into the Logic station

        setApplicationCommandManagerToWatch (&commandManager);   // refreshes command-item states
        juce::MenuBarModel::setMacMainMenu (this);               // File / Edit / Track / Transport / Station at the top

        setSize (1120, 820);

        const juce::StringArray candidates {
            "/Users/harrisonsongolo/Desktop/CREDIT WRECKERS 2 MISS PAYMENT_01 - Alfie Rudman - Jerk It.mov",
            "/Users/harrisonsongolo/Desktop/CREDIT WRECKERS 2 MISS PAYMENT/Film_Versions/MISS PAYMENT_01 - Alfie Rudman - Jerk It.mov"
        };
        for (const auto& p : candidates)
        {
            const juce::File f (p);
            if (f.existsAsFile()) { addVideo (f); break; }
        }

        if (activeGroup >= 0) showVideoWindow (true);   // pop the video out once a clip is loaded

        startTimerHz (30);

        loadPersistedPluginList();          // instant; the user triggers a (re)scan from the track menu
    }

    ~MainComponent() override
    {
        juce::MenuBarModel::setMacMainMenu (nullptr);
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        setLookAndFeel (nullptr);
        if (alive) *alive = false;   // in-flight ffmpeg/scan worker callbacks bail instead of touching a dead window
        videoWindow.reset();         // release the video pop-out before the VideoView member is destroyed
        closeAllPluginWindows();     // delete editors while their processors (in the engine) are still alive
        removeKeyListener (commandManager.getKeyMappings());
        stopTimer();
        timeline.setGroups (nullptr);
        for (auto& g : groups)
            for (auto& t : g->tracks)
                if (t->thumb != nullptr) { t->thumb->removeChangeListener (this); t->thumb->setSource (nullptr); }
    }

    //==========================================================================
    void addVideo (const juce::File& f)
    {
        const double dur = VideoView::probeDurationSeconds (f);
        if (dur <= 0.0)   // AVFoundation couldn't parse it (unsupported container/codec, or not a video)
        {
            titleLabel.setText ("Couldn't read \"" + f.getFileName() + "\" - unsupported video format.", juce::dontSendNotification);
            return;
        }

        auto grp = std::make_unique<VideoGroup>();
        grp->name     = f.getFileName();
        grp->file     = f;
        grp->duration = dur;
        groups.push_back (std::move (grp));
        timeline.setGroups (&groups);
        clearHistory();
        const int newIdx = (int) groups.size() - 1;

        if (activeGroup < 0) activateGroup (newIdx);
        else                 { resized(); timeline.repaint(); }

        detectCutsFor (newIdx);
    }

    void detectCutsFor (int g)
    {
        if (! validGroup (g)) return;
        const juce::File ff = FfmpegTool::find();
        if (! ff.existsAsFile()) return;
        const juce::File vf = groups[(size_t) g]->file;
        auto a = alive;
        std::thread ([this, a, g, ff, vf]
        {
            auto cuts = FfmpegTool::detectSceneCuts (ff, vf, 0.3);
            juce::MessageManager::callAsync ([this, a, g, cuts]
            {
                if (! a->load()) return;   // window gone -> don't touch it
                if (validGroup (g)) { groups[(size_t) g]->cutMarkers = cuts; timeline.repaint(); }
            });
        }).detach();
    }

    void activateGroup (int g)
    {
        if (! validGroup (g)) return;
        clearHistory();   // a group switch is an undo-history boundary (snapshots are per active-group structure)
        pauseAll();
        activeGroup = g;
        playhead = 0.0;

        auto* ag = groups[(size_t) g].get();
        video.loadFile (ag->file);

        // active group's tracks carry clips; all others go silent (empty clips)
        for (int gi = 0; gi < numGroups(); ++gi)
            for (auto& t : groups[(size_t) gi]->tracks)
                audioEngine.setTrackClips (t->engineId, (gi == g) ? t->clips : std::vector<AudioClip>{});

        audioEngine.setMinLengthSeconds (ag->duration);
        audioEngine.setPositionSeconds (0.0);
        lastMinLen = -1.0;

        selGroup = g; selTrack = -1; selClip = -1;
        titleLabel.setText ("Layback Station  -  " + ag->name, juce::dontSendNotification);

        applyMixGains();

        {   // re-clamp the loop to the new group's timeline (it may be shorter)
            const double tl = timelineLength();
            if (loopEnd > tl) loopEnd = tl;
            if (loopStart >= loopEnd) { loopEnabled = false; loopStart = 0.0; loopEnd = 0.0; loopToggle.setToggleState (false, juce::dontSendNotification); }
            timeline.setLoop (loopEnabled, loopStart, loopEnd);
        }

        timeline.setActiveGroup (g);
        timeline.setSelection (g, -1, -1);
        timeline.setPlayhead (0.0);
        refreshMixer();
        resized();
        timeline.repaint();
    }

    void importTrack (int g)
    {
        if (! validGroup (g)) return;
        chooser = std::make_unique<juce::FileChooser> ("Import a track", juce::File ("~/Desktop"),
                    "*.wav;*.aif;*.aiff;*.aifc;*.mp3;*.m4a;*.aac;*.flac;*.ogg;*.oga;*.opus;*.caf;*.wma;*.au;*.amr");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, g] (const juce::FileChooser& fc)
            {
                restoreKeyFocus();
                const auto res = fc.getResult();
                if (res.existsAsFile()) addTrackFromFile (g, res);
            });
    }

    void addTrackFromFile (int g, const juce::File& f)
    {
        if (! validGroup (g)) return;

        double len = 0.0;
        const int id = audioEngine.addTrack (f, len);
        if (id < 0) return;

        auto tr = std::make_unique<AudioTrack>();
        tr->name         = f.getFileNameWithoutExtension();
        tr->file         = f;
        tr->engineId     = id;
        tr->sourceLength = len;
        tr->clips.push_back ({ 0.0, 0.0, len });
        tr->thumb = std::make_unique<juce::AudioThumbnail> (512, audioEngine.getFormatManager(), thumbnailCache);
        tr->thumb->addChangeListener (this);
        tr->thumb->setSource (new juce::FileInputSource (f));

        const bool firstInGroup = groups[(size_t) g]->tracks.empty();
        groups[(size_t) g]->tracks.push_back (std::move (tr));

        audioEngine.setTrackClips (id, (g == activeGroup) ? groups[(size_t) g]->tracks.back()->clips
                                                          : std::vector<AudioClip>{});

        groups[(size_t) g]->tracks.back()->beatMarkers = audioEngine.computeTrackOnsets (id);   // detect beats

        if (firstInGroup && g == activeGroup)
            groups[(size_t) g]->videoMute = true;   // hear the candidate, not the baked-in audio

        selGroup = g; selTrack = (int) groups[(size_t) g]->tracks.size() - 1; selClip = 0;
        clearHistory();
        applyMixGains();
        refreshMixer();
        timeline.setSelection (selGroup, selTrack, selClip);
        resized();
        timeline.repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& s = laf.skin;
        g.fillAll (s.windowBg);

        // transport toolbar strip
        if (! transportBand.isEmpty())
        {
            g.setColour (s.panel);
            g.fillRect (transportBand);
            g.setColour (s.windowBg.darker (0.5f));
            g.fillRect (transportBand.getX(), transportBand.getY(), transportBand.getWidth(), 1);
            g.fillRect (transportBand.getX(), transportBand.getBottom() - 1, transportBand.getWidth(), 1);
        }

        // framed viewer: black matte + thin border behind the AV surface
        if (! viewerFrame.isEmpty())
        {
            g.setColour (juce::Colours::black);
            g.fillRect (viewerFrame.expanded (2));
            g.setColour (s.control);
            g.drawRect (viewerFrame.expanded (2), 1);
        }

        if (laf.skin.layout == 1 && ! logicToolbar.isEmpty())   // Logic local menu row above the tracks
        {
            g.setColour (s.panel.brighter (0.04f));
            g.fillRect (logicToolbar);
            g.setColour (s.windowBg.darker (0.4f));
            g.fillRect (logicToolbar.getX(), logicToolbar.getBottom() - 1, logicToolbar.getWidth(), 1);
            g.setColour (s.muted);
            g.setFont (juce::Font (juce::FontOptions().withHeight (11.5f)));
            g.drawText ("Edit        Functions        View", logicToolbar.getX() + 210, logicToolbar.getY(),
                        360, logicToolbar.getHeight(), juce::Justification::centredLeft, false);
        }

        // Layback signature (default layout only): LAYBACK• wordmark + the "lay-back line" accent hairline
        if (laf.skin.layout == 0)
        {
            if (! laybackWordmark.isEmpty())
            {
                auto w = laybackWordmark;
                g.setColour (s.text);
                g.setFont (juce::Font (juce::FontOptions().withHeight (14.0f)));
                g.drawText ("LAYBACK", w, juce::Justification::centredLeft, false);
                g.setColour (s.accent);
                g.fillEllipse ((float) (w.getX() + 82), (float) w.getCentreY() - 2.5f, 5.0f, 5.0f);
            }
            if (! transportBand.isEmpty())   // the "lay-back line" accent hairline under the transport bar
            {
                const int y = transportBand.getBottom();
                g.setGradientFill (juce::ColourGradient (s.accent, (float) transportBand.getX(), 0.0f,
                                                         juce::Colour (0xff3fe0ff), (float) transportBand.getRight(), 0.0f, false));
                g.fillRect (transportBand.getX(), y, transportBand.getWidth(), 2);
                g.setColour (s.accent.withAlpha (0.10f));
                g.fillRect (transportBand.getX(), y + 2, transportBand.getWidth(), 3);
            }
        }
    }

    void showVideoWindow (bool show)
    {
        videoWindowOpen = show;
        if (videoWindow != nullptr)
        {
            videoWindow->setVisible (show);
            if (show) videoWindow->toFront (true);
        }
        if (videoButton.getToggleState() != show)
            videoButton.setToggleState (show, juce::dontSendNotification);
    }

    void resized() override
    {
        switch (laf.skin.layout)
        {
            case 1: layoutLogic();           break;   // Logic: dedicated LCD control bar
            case 3: layoutStacked (58, 280); break;   // Pro Tools: taller bar, bigger counter
            case 2: layoutAbleton();         break;   // Ableton
            default: layoutDefault();        break;   // Layback
        }
    }

    // Toggle the generic toolbar vs the Logic control bar.
    void setLogicChrome (bool logic)
    {
        logicBar.setVisible (logic);
        logicInspector.setVisible (logic);
        for (auto* c : std::initializer_list<juce::Component*> { &openButton, &playButton, &exportButton, &keysButton, &projectButton, &videoButton })
            c->setVisible (! logic);
        loopToggle.setVisible (! logic);
        snapToggle.setVisible (! logic);
        timeLabel.setVisible (! logic);
    }

    // Logic: full-width LCD control bar on top, status line, tracks fill below.
    void layoutLogic()
    {
        setLogicChrome (true);
        auto area = getLocalBounds();
        transportBand = area.removeFromTop (58);
        logicBar.setBounds (transportBand);

        auto status = area.removeFromTop (18);
        titleLabel.setBounds (status.reduced (10, 0));
        logicToolbar = area.removeFromTop (26);                 // local Edit / Functions / View row
        area.removeFromTop (2);

        if (mixerVisible) { mixerView.setBounds (area.removeFromBottom (210)); area.removeFromBottom (4); }

        logicInspector.setBounds (area.removeFromLeft (200));   // Logic left Inspector (channel strip)
        logicInspector.setSelection (&groups, activeGroup, selTrack);
        area.removeFromLeft (2);

        timelineViewport.setBounds (area);
        viewerFrame = {};
        updateTimelineSize();
    }

    void refreshInspector()
    {
        if (laf.skin.layout == 1) logicInspector.setSelection (&groups, activeGroup, selTrack);
    }

    // Default (Layback) orientation: big viewer on top, transport mid, timeline bottom.
    void layoutDefault()
    {
        setLogicChrome (false);
        auto r = getLocalBounds().reduced (12);
        auto top = r.removeFromTop (24);
        laybackWordmark = top.removeFromLeft (150);   // brand wordmark, drawn in paint()
        top.removeFromLeft (10);
        titleLabel.setBounds (top);
        r.removeFromTop (6);

        auto controls = r.removeFromTop (40);                 // transport bar at the top (DAW-like)
        transportBand = { 0, controls.getY() - 6, getWidth(), controls.getHeight() + 12 };
        r.removeFromTop (10);

        if (mixerVisible) { mixerView.setBounds (r.removeFromBottom (210)); r.removeFromBottom (8); }
        timelineViewport.setBounds (r);                       // tracks fill the rest of the window
        viewerFrame = {};

        openButton.setBounds (controls.removeFromLeft (104));
        controls.removeFromLeft (8);
        projectButton.setBounds (controls.removeFromLeft (80));
        controls.removeFromLeft (8);
        playButton.setBounds (controls.removeFromLeft (84));
        controls.removeFromLeft (10);
        loopToggle.setBounds (controls.removeFromLeft (66));
        controls.removeFromLeft (8);
        snapToggle.setBounds (controls.removeFromLeft (62));
        controls.removeFromLeft (8);
        videoButton.setBounds (controls.removeFromLeft (72));
        controls.removeFromLeft (8);
        keysButton.setBounds (controls.removeFromLeft (118));
        controls.removeFromLeft (8);
        exportButton.setBounds (controls.removeFromLeft (90));
        timeLabel.setBounds  (controls.removeFromRight (150));

        updateTimelineSize();
    }

    // Stacked DAW layout (Logic / Pro Tools): top control bar (transport left,
    // centered counter, file/export right), status line, docked movie strip, timeline below.
    void layoutStacked (int barH, int counterW)
    {
        setLogicChrome (false);
        auto area = getLocalBounds();
        transportBand = area.removeFromTop (barH);          // paint() fills this as the control bar

        auto b = transportBand.reduced (10, juce::jmax (6, (barH - 34) / 2));
        playButton.setBounds (b.removeFromLeft (64)); b.removeFromLeft (6);
        loopToggle.setBounds (b.removeFromLeft (56)); b.removeFromLeft (6);
        snapToggle.setBounds (b.removeFromLeft (52)); b.removeFromLeft (6);
        videoButton.setBounds (b.removeFromLeft (62));

        exportButton.setBounds  (b.removeFromRight (86));  b.removeFromRight (6);
        keysButton.setBounds    (b.removeFromRight (122)); b.removeFromRight (6);
        projectButton.setBounds (b.removeFromRight (80));  b.removeFromRight (6);
        openButton.setBounds    (b.removeFromRight (104)); b.removeFromRight (12);

        timeLabel.setBounds (b.withSizeKeepingCentre (juce::jmax (120, juce::jmin (counterW, b.getWidth())), b.getHeight()));

        auto status = area.removeFromTop (18);
        titleLabel.setBounds (status.reduced (10, 0));

        area.removeFromTop (4);
        if (mixerVisible) { mixerView.setBounds (area.removeFromBottom (210)); area.removeFromBottom (4); }
        timelineViewport.setBounds (area);                  // tracks fill the window (video is a pop-out)
        viewerFrame = {};
        updateTimelineSize();
    }

    // Ableton layout: thin flat top bar (transport + clock on the left), the
    // arrangement/timeline fills the window, video docked as a right-hand panel.
    void layoutAbleton()
    {
        setLogicChrome (false);
        auto area = getLocalBounds();
        transportBand = area.removeFromTop (46);

        auto b = transportBand.reduced (10, 7);
        playButton.setBounds (b.removeFromLeft (58)); b.removeFromLeft (8);
        timeLabel.setBounds  (b.removeFromLeft (168)); b.removeFromLeft (10);   // clock beside the transport
        loopToggle.setBounds (b.removeFromLeft (54)); b.removeFromLeft (6);
        snapToggle.setBounds (b.removeFromLeft (50)); b.removeFromLeft (6);
        videoButton.setBounds (b.removeFromLeft (60));

        exportButton.setBounds  (b.removeFromRight (84));  b.removeFromRight (6);
        keysButton.setBounds    (b.removeFromRight (120)); b.removeFromRight (6);
        projectButton.setBounds (b.removeFromRight (78));  b.removeFromRight (6);
        openButton.setBounds    (b.removeFromRight (100));

        auto status = area.removeFromTop (16);
        titleLabel.setBounds (status.reduced (10, 0));

        if (mixerVisible) { mixerView.setBounds (area.removeFromBottom (200)); area.removeFromBottom (4); }
        timelineViewport.setBounds (area);                  // arrangement fills the window (video is a pop-out)
        viewerFrame = {};
        updateTimelineSize();
    }

    void updateTimelineSize()
    {
        const int sb = timelineViewport.getScrollBarThickness();
        const int w  = juce::jmax (200, timelineViewport.getWidth() - sb);
        const int h  = juce::jmax (timeline.contentHeight(), timelineViewport.getHeight());
        timeline.setSize (w, h);
    }

private:
    //==========================================================================
    int  numGroups() const { return (int) groups.size(); }
    bool validGroup (int g) const { return g >= 0 && g < (int) groups.size(); }
    bool validTrack (int g, int t) const { return validGroup (g) && t >= 0 && t < (int) groups[(size_t) g]->tracks.size(); }
    bool validClip  (int g, int t, int c) const { return validTrack (g, t) && c >= 0 && c < (int) groups[(size_t) g]->tracks[(size_t) t]->clips.size(); }
    VideoGroup* activeGroupPtr() { return validGroup (activeGroup) ? groups[(size_t) activeGroup].get() : nullptr; }

    double videoDur() const { return video.getDurationSeconds(); }

    double timelineLength()
    {
        auto* ag = activeGroupPtr();
        if (ag == nullptr) return 0.0;
        double len = videoDur() > 0.0 ? videoDur() : ag->duration;
        for (const auto& t : ag->tracks)
            for (const auto& c : t->clips)
                len = juce::jmax (len, c.timelineEnd());
        return len;
    }

    void applyMixGains()
    {
        for (int gi = 0; gi < numGroups(); ++gi)
            if (gi != activeGroup)
                for (auto& t : groups[(size_t) gi]->tracks)
                    audioEngine.setTrackGain (t->engineId, 0.0f);

        auto* ag = activeGroupPtr();
        if (ag == nullptr) { video.setMuted (true); return; }

        bool anySolo = ag->videoSolo;
        for (auto& t : ag->tracks) if (t->solo) anySolo = true;

        video.setMuted (! (! ag->videoMute && (! anySolo || ag->videoSolo)));
        for (auto& t : ag->tracks)
        {
            const bool aud = ! t->mute && (! anySolo || t->solo);
            audioEngine.setTrackGain (t->engineId, aud ? t->volume : 0.0f);   // fold in the channel fader
        }
    }

    // Single-track gain update for a fader drag: one engine call instead of re-setting every track.
    void updateTrackGain (int g, int t)
    {
        if (g != activeGroup || ! validTrack (g, t)) return;
        auto* ag = activeGroupPtr();
        if (ag == nullptr) return;
        bool anySolo = ag->videoSolo;
        for (auto& tt : ag->tracks) if (tt->solo) anySolo = true;
        auto& tr = *groups[(size_t) g]->tracks[(size_t) t];
        const bool aud = ! tr.mute && (! anySolo || tr.solo);
        audioEngine.setTrackGain (tr.engineId, aud ? tr.volume : 0.0f);
    }

    void pushActiveClips (int g)   // sync the engine to a track edit in the active group
    {
        if (g == activeGroup)
            for (auto& t : groups[(size_t) g]->tracks)
                audioEngine.setTrackClips (t->engineId, t->clips);
    }

    //==========================================================================
    void seekAll (double p)
    {
        reachedEnd = false;
        playhead = juce::jlimit (0.0, juce::jmax (0.0, timelineLength()), p);
        audioEngine.setPositionSeconds (playhead);
        if (playing && ! audioEngine.isPlaying()) audioEngine.play();
        video.setPositionSeconds (playhead);
        const double vd = videoDur();
        if (playing && (vd <= 0.0 || playhead < vd - 0.001)) video.play();
        timeline.setPlayhead (playhead);
    }

    void playAll()
    {
        const double tl = timelineLength();
        if (loopEnabled && loopEnd > loopStart)
        {
            if (playhead < loopStart - 1.0e-6 || playhead >= loopEnd) playhead = loopStart;
        }
        else if (tl > 0.0 && playhead >= tl - 0.05)
        {
            playhead = 0.0;
        }

        reachedEnd = false;
        playing = true;
        audioEngine.setPositionSeconds (playhead);
        audioEngine.play();
        video.setPositionSeconds (playhead);
        const double vd = videoDur();
        if (vd <= 0.0 || playhead < vd - 0.001) video.play();   // vd==0 => duration not loaded yet, start anyway
        timeline.setPlayhead (playhead);
    }

    void pauseAll() { playing = false; audioEngine.stop(); video.pause(); }
    void togglePlay() { if (playing) pauseAll(); else playAll(); }

    void toggleLoop()
    {
        pushUndo();
        loopEnabled = loopToggle.getToggleState();
        if (loopEnabled && loopEnd <= loopStart) { loopStart = 0.0; loopEnd = timelineLength(); }
        timeline.setLoop (loopEnabled, loopStart, loopEnd);
    }

    //==========================================================================
    void clipChanged (int g, int t, int c, AudioClip nc)
    {
        if (validClip (g, t, c))
        {
            groups[(size_t) g]->tracks[(size_t) t]->clips[(size_t) c] = nc;
            pushActiveClips (g);
            timeline.repaint();
        }
    }

    void splitTrackClip (int g, int t, double tm)
    {
        if (! validTrack (g, t)) return;
        auto& cl = groups[(size_t) g]->tracks[(size_t) t]->clips;
        for (int j = 0; j < (int) cl.size(); ++j)
        {
            auto& c = cl[(size_t) j];
            if (tm > c.timelineStart + kMinClipSeconds && tm < c.timelineEnd() - kMinClipSeconds)
            {
                pushUndo();
                AudioClip a = c; a.duration = tm - c.timelineStart;
                AudioClip b; b.timelineStart = tm; b.sourceIn = c.sourceIn + (tm - c.timelineStart); b.duration = c.timelineEnd() - tm;
                cl[(size_t) j] = a;
                cl.insert (cl.begin() + j + 1, b);
                selGroup = g; selTrack = t; selClip = j + 1;
                pushActiveClips (g);
                timeline.setSelection (g, t, j + 1);
                timeline.repaint();
                return;
            }
        }
    }

    void deleteClip (int g, int t, int c)
    {
        if (validClip (g, t, c))
        {
            pushUndo();
            auto& cl = groups[(size_t) g]->tracks[(size_t) t]->clips;
            cl.erase (cl.begin() + c);
            pushActiveClips (g);
            if (selGroup == g && selTrack == t)
            {
                if (selClip == c)      selClip = -1;
                else if (selClip > c)  --selClip;
            }
            timeline.setSelection (selGroup, selTrack, selClip);
            timeline.repaint();
        }
    }

    void showClipMenu (int g, int t, int c, double tm)
    {
        if (! validTrack (g, t)) return;
        selGroup = g; selTrack = t; selClip = c;
        timeline.setSelection (g, t, c);

        juce::PopupMenu m;
        m.addItem (1, "Split here");
        m.addItem (2, "Delete clip");
        m.showMenuAsync (juce::PopupMenu::Options(),
            [this, g, t, c, tm] (int r)
            {
                if      (r == 1) splitTrackClip (g, t, tm);
                else if (r == 2) deleteClip (g, t, c);
                restoreKeyFocus();
            });
    }

    void openAddVideo()
    {
        chooser = std::make_unique<juce::FileChooser> ("Add a video", juce::File ("~/Desktop"),
                    "*.mov;*.mp4;*.m4v;*.qt;*.avi;*.mpg;*.mpeg;*.m2v;*.m2ts;*.mts;*.ts;*.3gp;*.3g2;*.mxf;*.dv;*.mkv;*.webm;*.wmv;*.flv");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                restoreKeyFocus();
                const auto res = fc.getResult();
                if (res.existsAsFile()) addVideo (res);
            });
    }

    void exportVideo()
    {
        auto* ag = activeGroupPtr();
        if (ag == nullptr) return;
        const double len = videoDur() > 0.0 ? videoDur() : ag->duration;
        if (len <= 0.0) { titleLabel.setText ("Nothing to export.", juce::dontSendNotification); return; }

        const juce::File videoFile = ag->file;
        const juce::String base = videoFile.getFileNameWithoutExtension();

        chooser = std::make_unique<juce::FileChooser> ("Export video (choose format by extension)",
                    juce::File ("~/Desktop").getChildFile (base + " - Layback.mp4"),
                    "*.mp4;*.mov;*.m4v;*.mkv;*.webm;*.avi;*.mxf;*.wmv");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
            [this, videoFile, len] (const juce::FileChooser& fc)
            {
                restoreKeyFocus();
                const auto out = fc.getResult();
                if (out == juce::File()) return;

                pauseAll();
                auto tmpWav = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("layback_export_mix.wav");
                if (! audioEngine.renderMixToFile (tmpWav, len))
                {
                    titleLabel.setText ("Export failed (audio render).", juce::dontSendNotification);
                    return;
                }

                exportButton.setEnabled (false);
                exportButton.setButtonText ("Exporting...");

                const auto ext = out.getFileExtension().toLowerCase();
                if (ext == ".mp4" || ext == ".mov" || ext == ".m4v")    // clean AVFoundation path (OS encoder)
                {
                    VideoView::exportVideoWithAudio (videoFile, tmpWav, len, out,
                        [this, tmpWav, out] (bool ok, juce::String msg)
                        {
                            tmpWav.deleteFile();
                            exportButton.setEnabled (true);
                            exportButton.setButtonText ("Export");
                            titleLabel.setText (ok ? ("Exported: " + out.getFileName())
                                                   : ("Export failed: " + msg), juce::dontSendNotification);
                        });
                }
                else                                                     // any other container -> ffmpeg
                {
                    exportViaFfmpeg (videoFile, tmpWav, len, out);
                }
            });
    }

    void exportViaFfmpeg (juce::File videoFile, juce::File wav, double len, juce::File out)
    {
        const juce::File ff = FfmpegTool::find();
        if (! ff.existsAsFile())
        {
            wav.deleteFile();
            exportButton.setEnabled (true);
            exportButton.setButtonText ("Export");
            titleLabel.setText ("FFmpeg not found - use .mp4/.mov, or install ffmpeg.", juce::dontSendNotification);
            return;
        }

        const juce::StringArray args {
            "-y", "-i", videoFile.getFullPathName(), "-i", wav.getFullPathName(),
            "-map", "0:v:0", "-map", "1:a:0", "-t", juce::String (len), "-shortest",
            out.getFullPathName()
        };

        auto a = alive;
        std::thread ([this, a, ff, args, wav, out]
        {
            juce::String log;
            const bool ok = FfmpegTool::runSync (ff, args, log);
            juce::MessageManager::callAsync ([this, a, ok, out, wav]
            {
                wav.deleteFile();
                if (! a->load()) return;   // window gone -> don't touch it
                exportButton.setEnabled (true);
                exportButton.setButtonText ("Export");
                titleLabel.setText (ok ? ("Exported: " + out.getFileName())
                                       : "Export failed (ffmpeg).", juce::dontSendNotification);
            });
        }).detach();
    }

    void exportAudio()
    {
        auto* ag = activeGroupPtr();
        if (ag == nullptr) return;
        const double len = videoDur() > 0.0 ? videoDur() : ag->duration;
        if (len <= 0.0) { titleLabel.setText ("Nothing to export.", juce::dontSendNotification); return; }

        const juce::String base = ag->file.getFileNameWithoutExtension();
        chooser = std::make_unique<juce::FileChooser> ("Export audio",
                    juce::File ("~/Desktop").getChildFile (base + " - Layback.wav"), "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
            [this, len] (const juce::FileChooser& fc)
            {
                restoreKeyFocus();
                const auto out = fc.getResult();
                if (out == juce::File()) return;
                pauseAll();
                if (audioEngine.renderMixToFile (out, len))
                    titleLabel.setText ("Exported audio: " + out.getFileName(), juce::dontSendNotification);
                else
                    titleLabel.setText ("Audio export failed.", juce::dontSendNotification);
            });
    }

    void showExportMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "Export video + audio (.mov)");
        m.addItem (2, "Export audio only (.wav)");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton),
            [this] (int r) { if (r == 1) exportVideo(); else if (r == 2) exportAudio(); restoreKeyFocus(); });
    }

    void setLoopRegion (double s, double e)
    {
        if (e <= s) return;
        pushUndo();
        loopEnabled = true; loopStart = s; loopEnd = e;
        loopToggle.setToggleState (true, juce::dontSendNotification);
        timeline.setLoop (true, s, e);
    }

    void clearLoop()
    {
        pushUndo();
        loopEnabled = false; loopStart = 0.0; loopEnd = 0.0;
        loopToggle.setToggleState (false, juce::dontSendNotification);
        timeline.setLoop (false, 0.0, 0.0);
    }

    void showLoopMenu (double t)
    {
        auto* ag = activeGroupPtr();
        const double vd = videoDur() > 0.0 ? videoDur() : (ag != nullptr ? ag->duration : 0.0);

        juce::PopupMenu m;
        m.addItem (1, "Loop whole video", vd > 0.0);
        m.addItem (2, "Loop from here to end", vd > 0.0 && t < vd - 0.05);
        m.addSeparator();
        m.addItem (3, "Clear loop", loopEnabled);
        m.showMenuAsync (juce::PopupMenu::Options(),
            [this, t, vd] (int r)
            {
                if      (r == 1) setLoopRegion (0.0, vd);
                else if (r == 2) setLoopRegion (t, vd);
                else if (r == 3) clearLoop();
                restoreKeyFocus();
            });
    }

    //==========================================================================
    // Command system + per-DAW keymap profiles.
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }

    void getAllCommands (juce::Array<juce::CommandID>& c) override
    {
        const juce::CommandID ids[] = { LSCmd::TogglePlay, LSCmd::Split, LSCmd::DeleteClip, LSCmd::ToggleLoop,
                                        LSCmd::ToggleSnap, LSCmd::NudgeLeft, LSCmd::NudgeRight, LSCmd::Undo, LSCmd::Redo };
        c.addArray (ids, juce::numElementsInArray (ids));
    }

    void getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info) override
    {
        switch (id)
        {
            case LSCmd::TogglePlay: info.setInfo ("Play/Pause", "Toggle playback", "Transport", 0); break;
            case LSCmd::Split:      info.setInfo ("Split at playhead", "Split the selected clip", "Edit", 0); break;
            case LSCmd::DeleteClip: info.setInfo ("Delete clip", "Delete the selected clip", "Edit", 0); break;
            case LSCmd::ToggleLoop: info.setInfo ("Toggle loop", "Enable/disable cycle", "Transport", 0); info.setTicked (loopEnabled); break;
            case LSCmd::ToggleSnap: info.setInfo ("Toggle snap", "Enable/disable snapping", "Edit", 0); info.setTicked (snapToggle.getToggleState()); break;
            case LSCmd::NudgeLeft:  info.setInfo ("Nudge left", "Nudge selected clip earlier", "Edit", 0); break;
            case LSCmd::NudgeRight: info.setInfo ("Nudge right", "Nudge selected clip later", "Edit", 0); break;
            case LSCmd::Undo:       info.setInfo ("Undo", "Undo the last edit", "Edit", 0); break;
            case LSCmd::Redo:       info.setInfo ("Redo", "Redo the last undone edit", "Edit", 0); break;
            default: break;
        }
        info.setActive (true);
    }

    bool perform (const InvocationInfo& info) override
    {
        switch (info.commandID)
        {
            case LSCmd::TogglePlay: togglePlay(); return true;
            case LSCmd::Split:      if (validTrack (selGroup, selTrack)) splitTrackClip (selGroup, selTrack, playhead); return true;
            case LSCmd::DeleteClip:
                if      (validClip (selGroup, selTrack, selClip)) deleteClip  (selGroup, selTrack, selClip);
                else if (validTrack (selGroup, selTrack))         deleteTrack (selGroup, selTrack);
                else if (validGroup (selGroup))                   deleteGroup (selGroup);
                return true;
            case LSCmd::ToggleLoop: loopToggle.setToggleState (! loopToggle.getToggleState(), juce::dontSendNotification); toggleLoop(); return true;
            case LSCmd::ToggleSnap: snapToggle.setToggleState (! snapToggle.getToggleState(), juce::dontSendNotification); timeline.setSnapEnabled (snapToggle.getToggleState()); return true;
            case LSCmd::NudgeLeft:  nudgeSelected (-0.05); return true;
            case LSCmd::NudgeRight: nudgeSelected ( 0.05); return true;
            case LSCmd::Undo: undo(); return true;
            case LSCmd::Redo: redo(); return true;
            default: return false;
        }
    }

    void nudgeSelected (double delta)
    {
        if (! validClip (selGroup, selTrack, selClip)) return;
        pushUndo();
        AudioClip c = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->clips[(size_t) selClip];
        c.timelineStart = juce::jmax (0.0, c.timelineStart + delta);
        clipChanged (selGroup, selTrack, selClip, c);
    }

    static juce::String profileName (KeyProfile p)
    {
        switch (p) { case KeyProfile::Logic: return "Logic"; case KeyProfile::ProTools: return "Pro Tools";
                     case KeyProfile::Ableton: return "Ableton"; default: return "Layback"; }
    }

    void applyKeyProfile (KeyProfile p)
    {
        keyProfile = p;
        auto* km = commandManager.getKeyMappings();
        km->clearAllKeyPresses();

        km->addKeyPress (LSCmd::TogglePlay, juce::KeyPress (juce::KeyPress::spaceKey));
        km->addKeyPress (LSCmd::DeleteClip, juce::KeyPress (juce::KeyPress::deleteKey));
        km->addKeyPress (LSCmd::DeleteClip, juce::KeyPress (juce::KeyPress::backspaceKey));
        km->addKeyPress (LSCmd::NudgeLeft,  juce::KeyPress (juce::KeyPress::leftKey));
        km->addKeyPress (LSCmd::NudgeRight, juce::KeyPress (juce::KeyPress::rightKey));

        auto add = [&] (int cmd, const juce::String& desc) { const auto k = juce::KeyPress::createFromDescription (desc); if (k.isValid()) km->addKeyPress (cmd, k); };
        add (LSCmd::Undo, "command + Z");
        add (LSCmd::Redo, "command + shift + Z");
        switch (p)
        {
            case KeyProfile::Logic:    add (LSCmd::Split, "command + T"); add (LSCmd::ToggleLoop, "C");                   add (LSCmd::ToggleSnap, "command + shift + S"); break;
            case KeyProfile::ProTools: add (LSCmd::Split, "command + E"); add (LSCmd::ToggleLoop, "command + shift + L"); add (LSCmd::ToggleSnap, "command + shift + S"); break;
            case KeyProfile::Ableton:  add (LSCmd::Split, "command + E"); add (LSCmd::ToggleLoop, "command + L");         add (LSCmd::ToggleSnap, "command + shift + S"); break;
            default:                   add (LSCmd::Split, "S");           add (LSCmd::ToggleLoop, "L");                   add (LSCmd::ToggleSnap, "N"); break;
        }
        keysButton.setButtonText ("Keys: " + profileName (p));
        applySkinForProfile (p);   // switching the station also reskins the app to that DAW
        menuItemsChanged();        // rebuild the menu bar to that DAW's menu set
        grabKeyboardFocus();
    }

    void applySkinForProfile (KeyProfile p)
    {
        Skin::Daw d = Skin::Layback;
        switch (p)
        {
            case KeyProfile::Logic:    d = Skin::Logic;    break;
            case KeyProfile::ProTools: d = Skin::ProTools; break;
            case KeyProfile::Ableton:  d = Skin::Ableton;  break;
            default:                   d = Skin::Layback;  break;
        }
        laf.applySkin (Skin::forDaw (d));
        timeline.setSkin (laf.skin);
        applyControlColours();
    }

    void applyControlColours()
    {
        const auto& s = laf.skin;
        timeLabel.setColour (juce::Label::textColourId,       s.timecodeText);
        timeLabel.setColour (juce::Label::backgroundColourId, s.timecodeBg);
        timeLabel.setColour (juce::Label::outlineColourId,    s.control);
        timeLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                       keyProfile == KeyProfile::ProTools ? 19.0f : 15.0f, juce::Font::plain)));
        titleLabel.setColour (juce::Label::textColourId,      s.muted);
        loopToggle.setColour (juce::ToggleButton::textColourId, s.text);
        snapToggle.setColour (juce::ToggleButton::textColourId, s.text);
        if (auto* win = findParentComponentOfClass<juce::ResizableWindow>())   // restyle the window chrome too
        { win->setBackgroundColour (s.windowBg); win->repaint(); }
        mixerView.setSkin (s);
        logicBar.setSkin (s);
        logicInspector.setSkin (s);
        resized();                 // a skin can change the whole layout (e.g. Logic's top control bar)
        sendLookAndFeelChange();   // children re-read the themed colour IDs
        repaint();
        timeline.repaint();
    }

    void refreshMixer() { if (mixerVisible) mixerView.setModel (&groups, activeGroup); }

    void toggleMixer()
    {
        mixerVisible = ! mixerVisible;
        mixerView.setVisible (mixerVisible);
        if (mixerVisible) { mixerView.setSkin (laf.skin); mixerView.setModel (&groups, activeGroup); }
        resized();
    }

    //==========================================================================
    // Top menu bar — the active station mirrors that DAW's real menu bar:
    //   Logic     : File Edit Track Navigate Record Mix View Window Help
    //   Pro Tools : File Edit View Track Clip Event AudioSuite Options Setup Window Help
    //   Ableton   : File Edit Create View Options Help
    //   Layback   : File Edit Track Transport View
    juce::StringArray getMenuBarNames() override
    {
        switch (keyProfile)
        {
            case KeyProfile::Logic:    return { "File", "Edit", "Track", "Navigate", "Record", "Mix", "View", "Window", "Help" };
            case KeyProfile::ProTools: return { "File", "Edit", "View", "Track", "Clip", "Event", "AudioSuite", "Options", "Setup", "Window", "Help" };
            case KeyProfile::Ableton:  return { "File", "Edit", "Create", "View", "Options", "Help" };
            default:                   return { "File", "Edit", "Track", "Transport", "View" };
        }
    }

    juce::PopupMenu getMenuForIndex (int, const juce::String& name) override
    {
        juce::PopupMenu m;
        const bool hasTrack = validTrack (selGroup, selTrack);

        auto addSkin = [&] (juce::PopupMenu& pm)
        {
            juce::PopupMenu sk;
            sk.addItem (9020, "Layback",      true, keyProfile == KeyProfile::Layback);
            sk.addItem (9021, "Logic",        true, keyProfile == KeyProfile::Logic);
            sk.addItem (9022, "Pro Tools",    true, keyProfile == KeyProfile::ProTools);
            sk.addItem (9023, "Ableton Live", true, keyProfile == KeyProfile::Ableton);
            pm.addSubMenu ("Skin", sk);
        };
        auto fxItems = [&] { m.addItem (9011, "Insert EQ on Selected Track", hasTrack);
                             m.addItem (9012, "Insert Compressor on Selected Track", hasTrack); };
        auto rescan  = [&] { m.addItem (9013, scanning ? "Scanning Plugins..." : "Rescan Plugins", ! scanning); };

        if (name == "File")
        {
            m.addItem (9001, "New Project");
            m.addItem (9002, "Open Project...");
            m.addItem (9003, "Save Project...");
            m.addSeparator();
            m.addItem (9004, "Add Video...");
            m.addSeparator();
            m.addItem (9005, "Export Video + Audio...");
            m.addItem (9006, "Export Audio (WAV)...");
        }
        else if (name == "Edit")
        {
            m.addCommandItem (&commandManager, LSCmd::Undo);
            m.addCommandItem (&commandManager, LSCmd::Redo);
            m.addSeparator();
            m.addCommandItem (&commandManager, LSCmd::Split);
            m.addCommandItem (&commandManager, LSCmd::DeleteClip);
        }
        else if (name == "Track")
        {
            m.addItem (9010, "Import Audio Track...", activeGroup >= 0);
            m.addSeparator();
            fxItems();
        }
        else if (name == "Create")          // Ableton
        {
            m.addItem (9004, "Add Video...");
            m.addItem (9010, "Import Audio Track...", activeGroup >= 0);
            m.addSeparator();
            fxItems();
        }
        else if (name == "Mix" || name == "AudioSuite")   // Logic / Pro Tools
        {
            fxItems();
            m.addSeparator();
            rescan();
        }
        else if (name == "Transport" || name == "Navigate")
        {
            m.addCommandItem (&commandManager, LSCmd::TogglePlay);
            if (name == "Navigate") m.addItem (9030, "Go to Start");
            m.addSeparator();
            m.addCommandItem (&commandManager, LSCmd::ToggleLoop);
            m.addCommandItem (&commandManager, LSCmd::NudgeLeft);
            m.addCommandItem (&commandManager, LSCmd::NudgeRight);
        }
        else if (name == "Clip")            // Pro Tools
        {
            m.addCommandItem (&commandManager, LSCmd::Split);
            m.addCommandItem (&commandManager, LSCmd::DeleteClip);
        }
        else if (name == "Event")           // Pro Tools
        {
            m.addCommandItem (&commandManager, LSCmd::NudgeLeft);
            m.addCommandItem (&commandManager, LSCmd::NudgeRight);
            m.addSeparator();
            m.addCommandItem (&commandManager, LSCmd::ToggleSnap);
        }
        else if (name == "Record")          // Logic (recording is Phase C)
        {
            m.addItem (9099, "Record  (coming soon)", false);
        }
        else if (name == "View")
        {
            m.addItem (9060, videoWindowOpen ? "Hide Video Window" : "Show Video Window");
            m.addItem (9050, mixerVisible ? "Hide Mixer" : "Show Mixer");
            m.addSeparator();
            m.addCommandItem (&commandManager, LSCmd::ToggleSnap);
            m.addCommandItem (&commandManager, LSCmd::ToggleLoop);
            m.addSeparator();
            addSkin (m);
        }
        else if (name == "Options" || name == "Setup")    // Ableton / Pro Tools
        {
            m.addItem (9060, videoWindowOpen ? "Hide Video Window" : "Show Video Window");
            m.addCommandItem (&commandManager, LSCmd::ToggleSnap);
            m.addSeparator();
            rescan();
            m.addSeparator();
            addSkin (m);
        }
        else if (name == "Window")
        {
            m.addItem (9060, videoWindowOpen ? "Hide Video Window" : "Show Video Window");
            m.addItem (9050, mixerVisible ? "Hide Mixer" : "Show Mixer");
            m.addItem (9040, "Close All Plugin Windows", ! pluginWindows.empty());
        }
        else if (name == "Help")
        {
            m.addItem (9098, "Layback Station Help", false);
        }
        return m;
    }

    void menuItemSelected (int menuItemID, int) override
    {
        switch (menuItemID)
        {
            case 9001: newProject();    break;
            case 9002: openProject();   break;
            case 9003: saveProject();   break;
            case 9004: openAddVideo();  break;
            case 9005: exportVideo();   break;
            case 9006: exportAudio();   break;
            case 9010: if (activeGroup >= 0) importTrack (activeGroup); break;
            case 9011: if (validTrack (selGroup, selTrack)) { audioEngine.addNativeEffect (groups[(size_t) selGroup]->tracks[(size_t) selTrack]->engineId, 0); refreshMixer(); } break;
            case 9012: if (validTrack (selGroup, selTrack)) { audioEngine.addNativeEffect (groups[(size_t) selGroup]->tracks[(size_t) selTrack]->engineId, 1); refreshMixer(); } break;
            case 9013: startPluginScan(); break;
            case 9020: applyKeyProfile (KeyProfile::Layback);  break;
            case 9021: applyKeyProfile (KeyProfile::Logic);    break;
            case 9022: applyKeyProfile (KeyProfile::ProTools); break;
            case 9023: applyKeyProfile (KeyProfile::Ableton);  break;
            case 9030: seekAll (0.0); break;
            case 9040: closeAllPluginWindows(); break;
            case 9060: showVideoWindow (! videoWindowOpen); break;
            case 9050: toggleMixer(); break;
            default: break;
        }
    }

    void showKeysMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "Layback Station", true, keyProfile == KeyProfile::Layback);
        m.addItem (2, "Logic",          true, keyProfile == KeyProfile::Logic);
        m.addItem (3, "Pro Tools",      true, keyProfile == KeyProfile::ProTools);
        m.addItem (4, "Ableton Live",   true, keyProfile == KeyProfile::Ableton);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&keysButton),
            [this] (int r)
            {
                if      (r == 1) applyKeyProfile (KeyProfile::Layback);
                else if (r == 2) applyKeyProfile (KeyProfile::Logic);
                else if (r == 3) applyKeyProfile (KeyProfile::ProTools);
                else if (r == 4) applyKeyProfile (KeyProfile::Ableton);
                restoreKeyFocus();
            });
    }

    //==========================================================================
    // Delete track / video.
    void deleteTrack (int g, int t)
    {
        if (! validTrack (g, t)) return;
        clearHistory();
        auto& tr = groups[(size_t) g]->tracks[(size_t) t];
        if (tr->thumb != nullptr) { tr->thumb->removeChangeListener (this); tr->thumb->setSource (nullptr); }
        closePluginWindowsForTrack (tr->engineId);
        audioEngine.removeTrack (tr->engineId);
        groups[(size_t) g]->tracks.erase (groups[(size_t) g]->tracks.begin() + t);
        refreshMixer();
        if (selGroup == g) { if (selTrack == t) { selTrack = -1; selClip = -1; } else if (selTrack > t) --selTrack; }
        applyMixGains();
        timeline.setSelection (selGroup, selTrack, selClip);
        resized();
        timeline.repaint();
    }

    void deleteGroup (int g)
    {
        if (! validGroup (g)) return;
        clearHistory();
        for (auto& tr : groups[(size_t) g]->tracks)
        {
            if (tr->thumb != nullptr) { tr->thumb->removeChangeListener (this); tr->thumb->setSource (nullptr); }
            closePluginWindowsForTrack (tr->engineId);
            audioEngine.removeTrack (tr->engineId);
        }
        groups.erase (groups.begin() + g);
        timeline.setGroups (&groups);

        if (groups.empty())
        {
            pauseAll();
            activeGroup = -1; selGroup = selTrack = selClip = -1;
            playhead = 0.0; reachedEnd = false; lastMinLen = -1.0;
            audioEngine.setPositionSeconds (0.0);
            audioEngine.setMinLengthSeconds (0.0);
            titleLabel.setText ("Layback Station", juce::dontSendNotification);
            timeline.setActiveGroup (-1);
            timeline.setSelection (-1, -1, -1);
            timeline.setPlayhead (0.0);
        }
        else if (g == activeGroup)            // deleted the active video -> reload another
        {
            const int na = juce::jmin (g, (int) groups.size() - 1);
            activeGroup = -1;
            activateGroup (na);
        }
        else                                   // deleted an inactive video -> just shift indices, don't disturb playback
        {
            if (activeGroup > g) --activeGroup;
            if (selGroup > g) --selGroup;
            else if (selGroup == g) { selGroup = selTrack = selClip = -1; }
            timeline.setActiveGroup (activeGroup);
            timeline.setSelection (selGroup, selTrack, selClip);
        }
        resized();
        timeline.repaint();
    }

    //== effect-editor windows ==
    void openPluginEditor (int engineId, int index)
    {
        auto* proc = audioEngine.trackPlugin (engineId, index);
        if (proc == nullptr) return;
        for (auto& w : pluginWindows) if (w->proc == proc) { w->toFront (true); return; }

        auto win  = std::make_unique<PluginWindow> (proc, laf.skin.panel);
        auto* raw = win.get();
        auto a = alive;
        win->onClose = [this, raw, a] { juce::MessageManager::callAsync ([this, raw, a] { if (a->load()) closePluginWindow (raw); }); };
        pluginWindows.push_back (std::move (win));
    }

    void closePluginWindow (PluginWindow* w)
    {
        for (size_t i = 0; i < pluginWindows.size(); ++i)
            if (pluginWindows[i].get() == w) { pluginWindows.erase (pluginWindows.begin() + (long) i); break; }
    }

    void closePluginWindowForProc (juce::AudioProcessor* proc)
    {
        for (size_t k = 0; k < pluginWindows.size(); )
            if (pluginWindows[k]->proc == proc) pluginWindows.erase (pluginWindows.begin() + (long) k);
            else ++k;
    }

    void closePluginWindowsForTrack (int engineId)
    {
        const int n = audioEngine.trackPluginCount (engineId);
        for (int i = 0; i < n; ++i) closePluginWindowForProc (audioEngine.trackPlugin (engineId, i));
    }

    void closeAllPluginWindows() { pluginWindows.clear(); }

    //== plugin scanning + persistence ==
    juce::File pluginListFile() const
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Layback").getChildFile ("plugins.xml");
    }

    void persistPluginList()
    {
        const auto f = pluginListFile();
        f.getParentDirectory().createDirectory();
        if (auto xml = audioEngine.getKnownPlugins().createXml()) f.replaceWithText (xml->toString());
    }

    void loadPersistedPluginList()
    {
        if (auto xml = juce::parseXML (pluginListFile()))
            audioEngine.getKnownPlugins().recreateFromXml (*xml);
        pluginsScanned = audioEngine.getKnownPlugins().getNumTypes() > 0;
    }

    void startPluginScan()
    {
        if (scanning) return;
        scanning = true;
        titleLabel.setText ("Scanning audio plugins...", juce::dontSendNotification);
        auto a = alive;
        const juce::File dead = pluginListFile().getSiblingFile ("scan_crashlog.txt");   // skip a plugin that crashed the last scan
        std::thread ([this, a, dead]
        {
            juce::AudioPluginFormatManager fmts;   // LOCAL formats: the worker never touches the engine during the scan
            juce::addDefaultFormatsToManager (fmts);
            juce::KnownPluginList local;
            for (auto* fmt : fmts.getFormats())
            {
                if (! a->load()) return;
                juce::PluginDirectoryScanner scanner (local, *fmt, fmt->getDefaultLocationsToSearch(), true, dead);
                juce::String nm;
                while (scanner.scanNextFile (true, nm)) { if (! a->load()) return; }
            }
            auto xml = local.createXml();
            const juce::String xmlStr = xml ? xml->toString() : juce::String();
            juce::MessageManager::callAsync ([this, a, xmlStr]
            {
                if (! a->load()) return;
                if (auto x = juce::parseXML (xmlStr)) audioEngine.getKnownPlugins().recreateFromXml (*x);
                persistPluginList();
                scanning = false;
                pluginsScanned = true;
                titleLabel.setText ("Plugins ready (" + juce::String (audioEngine.getKnownPlugins().getNumTypes()) + " found)", juce::dontSendNotification);
            });
        }).detach();
    }

    void showTrackMenu (int g, int t)
    {
        if (! validTrack (g, t)) return;
        const int engineId = groups[(size_t) g]->tracks[(size_t) t]->engineId;

        juce::PopupMenu insert;
        insert.addItem (2001, "EQ (Layback)");
        insert.addItem (2002, "Compressor (Layback)");
        insert.addSeparator();

        juce::PopupMenu hosted;
        pluginMenuMap.clear();
        int id = 3000;
        for (const auto& d : audioEngine.getKnownPlugins().getTypes())
        {
            hosted.addItem (id, d.name + "  (" + d.pluginFormatName + ")");
            pluginMenuMap[id] = d;
            ++id;
        }
        if (pluginMenuMap.empty()) hosted.addItem (9, scanning ? "Scanning..." : "No plugins found", false, false);
        insert.addSubMenu ("Audio Units / VST3", hosted);

        juce::PopupMenu m;
        m.addSubMenu ("Insert effect", insert);

        const int pc = audioEngine.trackPluginCount (engineId);
        if (pc > 0)
        {
            juce::PopupMenu fx;
            for (int i = 0; i < pc; ++i)
            {
                juce::PopupMenu one;
                one.addItem (4000 + i * 2,     "Open editor");
                one.addItem (4000 + i * 2 + 1, "Remove");
                fx.addSubMenu (audioEngine.trackPluginName (engineId, i), one);
            }
            m.addSubMenu ("Effects (" + juce::String (pc) + ")", fx);
        }

        m.addSeparator();
        m.addItem (5, scanning ? "Scanning plugins..." : "Rescan plugins", ! scanning);
        m.addItem (1, "Delete track");

        m.showMenuAsync (juce::PopupMenu::Options(), [this, g, t, engineId] (int r)
        {
            if      (r == 1)    deleteTrack (g, t);
            else if (r == 5)    startPluginScan();
            else if (r == 2001) audioEngine.addNativeEffect (engineId, 0);
            else if (r == 2002) audioEngine.addNativeEffect (engineId, 1);
            else if (r >= 3000 && r < 4000)
            {
                const auto it = pluginMenuMap.find (r);
                if (it != pluginMenuMap.end())
                {
                    juce::String err;
                    if (! audioEngine.addHostedPlugin (engineId, it->second, err))
                        titleLabel.setText ("Plugin failed to load: " + err, juce::dontSendNotification);
                }
            }
            else if (r >= 4000)
            {
                const int idx  = (r - 4000) / 2;
                const bool open = ((r - 4000) % 2) == 0;
                if (open) openPluginEditor (engineId, idx);
                else { closePluginWindowForProc (audioEngine.trackPlugin (engineId, idx)); audioEngine.removeTrackPlugin (engineId, idx); }
            }
            refreshMixer();
            restoreKeyFocus();
        });
    }

    void showGroupMenu (int g)
    {
        if (! validGroup (g)) return;
        juce::PopupMenu m;
        m.addItem (1, "Delete video");
        m.showMenuAsync (juce::PopupMenu::Options(), [this, g] (int r) { if (r == 1) deleteGroup (g); restoreKeyFocus(); });
    }

    //==========================================================================
    // Undo / redo (snapshot of clip arrangement + mute/solo + loop within the current structure).
    EditSnapshot captureSnapshot()
    {
        EditSnapshot s;
        s.groups.reserve (groups.size());
        for (auto& g : groups)
        {
            EditSnapshot::GroupS gs; gs.vMute = g->videoMute; gs.vSolo = g->videoSolo;
            for (auto& t : g->tracks)
            {
                EditSnapshot::TrackS ts; ts.mute = t->mute; ts.solo = t->solo; ts.clips = t->clips;
                gs.tracks.push_back (std::move (ts));
            }
            s.groups.push_back (std::move (gs));
        }
        s.loopEnabled = loopEnabled; s.loopStart = loopStart; s.loopEnd = loopEnd;
        s.selGroup = selGroup; s.selTrack = selTrack; s.selClip = selClip;
        return s;
    }

    void restoreSnapshot (const EditSnapshot& s)
    {
        if (s.groups.size() != groups.size()) return;   // structure changed -> can't apply (history is cleared on such changes)
        for (size_t gi = 0; gi < groups.size(); ++gi)
            if (s.groups[gi].tracks.size() != groups[gi]->tracks.size()) return;

        for (size_t gi = 0; gi < groups.size(); ++gi)
        {
            auto& g = groups[gi]; const auto& gs = s.groups[gi];
            g->videoMute = gs.vMute; g->videoSolo = gs.vSolo;
            for (size_t ti = 0; ti < g->tracks.size(); ++ti)
            {
                g->tracks[ti]->mute  = gs.tracks[ti].mute;
                g->tracks[ti]->solo  = gs.tracks[ti].solo;
                g->tracks[ti]->clips = gs.tracks[ti].clips;
            }
        }
        loopEnabled = s.loopEnabled; loopStart = s.loopStart; loopEnd = s.loopEnd;
        selGroup = s.selGroup; selTrack = s.selTrack; selClip = s.selClip;

        if (activeGroup >= 0 && activeGroup < (int) groups.size())
            for (auto& t : groups[(size_t) activeGroup]->tracks)
                audioEngine.setTrackClips (t->engineId, t->clips);

        const double tl = timelineLength();                       // clamp restored loop/playhead to the current arrangement
        if (loopEnd > tl) loopEnd = tl;
        if (loopStart >= loopEnd) { loopEnabled = false; loopStart = 0.0; loopEnd = 0.0; }
        playhead = juce::jlimit (0.0, juce::jmax (0.0, tl), playhead);
        audioEngine.setPositionSeconds (playhead);

        applyMixGains();
        loopToggle.setToggleState (loopEnabled, juce::dontSendNotification);
        timeline.setLoop (loopEnabled, loopStart, loopEnd);
        timeline.setSelection (selGroup, selTrack, selClip);
        timeline.setPlayhead (playhead);
        timeline.repaint();
    }

    void pushUndo()
    {
        undoStack.push_back (captureSnapshot());
        if (undoStack.size() > 200) undoStack.erase (undoStack.begin());
        redoStack.clear();
    }

    void clearHistory() { undoStack.clear(); redoStack.clear(); }
    void undo() { if (undoStack.empty()) return; redoStack.push_back (captureSnapshot()); auto s = undoStack.back(); undoStack.pop_back(); restoreSnapshot (s); }
    void redo() { if (redoStack.empty()) return; undoStack.push_back (captureSnapshot()); auto s = redoStack.back(); redoStack.pop_back(); restoreSnapshot (s); }

    void parentHierarchyChanged() override { if (isShowing()) grabKeyboardFocus(); }
    void restoreKeyFocus() { if (isShowing()) grabKeyboardFocus(); }   // reclaim focus after a menu/dialog so shortcuts keep working

    //==========================================================================
    // Project save / open. A ".lbproj" bundle = project.json + a media/ folder of copied files.
    juce::String copyMedia (const juce::File& src, const juce::File& mediaDir,
                            std::map<juce::String, juce::String>& seen, bool& allOk)
    {
        if (! src.existsAsFile()) { allOk = false; return src.getFullPathName(); }
        const juce::String key = src.getFullPathName();
        if (auto it = seen.find (key); it != seen.end()) return it->second;   // same source -> reuse the same media file

        // unique destination per distinct source (hash of the full path) so same-named files never collide
        const juce::String destName = src.getFileNameWithoutExtension() + "_"
            + juce::String::toHexString ((juce::int64) (key.hashCode64() & 0xffffffLL)) + src.getFileExtension();
        const juce::File dest = mediaDir.getChildFile (destName);

        bool copied = true;
        if (! (dest.existsAsFile() && dest.getSize() == src.getSize()))
            copied = src.copyFileTo (dest);

        const juce::String result = copied ? ("media/" + destName) : key;   // on failure, keep referencing the original
        if (! copied) allOk = false;
        seen[key] = result;
        return result;
    }

    void saveProject()
    {
        chooser = std::make_unique<juce::FileChooser> ("Save project",
                    juce::File ("~/Desktop").getChildFile ("Layback Project.lbproj"), "*.lbproj");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& fc)
            {
                restoreKeyFocus();
                auto dir = fc.getResult();
                if (dir == juce::File()) return;
                if (dir.getFileExtension().toLowerCase() != ".lbproj")
                    dir = dir.getSiblingFile (dir.getFileNameWithoutExtension() + ".lbproj");
                if (! dir.createDirectory().wasOk()) { titleLabel.setText ("Save failed: can't create project folder.", juce::dontSendNotification); return; }
                const juce::File media = dir.getChildFile ("media");
                if (! media.createDirectory().wasOk()) { titleLabel.setText ("Save failed: can't create media folder.", juce::dontSendNotification); return; }

                std::map<juce::String, juce::String> seen;
                bool allOk = true;

                auto* root = new juce::DynamicObject();
                root->setProperty ("version", 1);
                root->setProperty ("activeGroup", activeGroup);
                root->setProperty ("loopEnabled", loopEnabled);
                root->setProperty ("loopStart", loopStart);
                root->setProperty ("loopEnd", loopEnd);
                root->setProperty ("masterGain", audioEngine.getMasterGain());

                juce::var garr;
                for (auto& g : groups)
                {
                    auto* go = new juce::DynamicObject();
                    go->setProperty ("name", g->name);
                    go->setProperty ("video", copyMedia (g->file, media, seen, allOk));
                    go->setProperty ("duration", g->duration);
                    go->setProperty ("expanded", g->expanded);
                    go->setProperty ("videoMute", g->videoMute);
                    go->setProperty ("videoSolo", g->videoSolo);
                    go->setProperty ("cuts", doublesToVar (g->cutMarkers));

                    juce::var tarr;
                    for (auto& t : g->tracks)
                    {
                        auto* to = new juce::DynamicObject();
                        to->setProperty ("name", t->name);
                        to->setProperty ("file", copyMedia (t->file, media, seen, allOk));
                        to->setProperty ("sourceLength", t->sourceLength);
                        to->setProperty ("mute", t->mute);
                        to->setProperty ("solo", t->solo);
                        to->setProperty ("beats", doublesToVar (t->beatMarkers));

                        juce::var carr;
                        for (auto& c : t->clips)
                        {
                            auto* co = new juce::DynamicObject();
                            co->setProperty ("start", c.timelineStart);
                            co->setProperty ("in", c.sourceIn);
                            co->setProperty ("dur", c.duration);
                            carr.append (juce::var (co));
                        }
                        to->setProperty ("clips", carr);
                        tarr.append (juce::var (to));
                    }
                    go->setProperty ("tracks", tarr);
                    garr.append (juce::var (go));
                }
                root->setProperty ("groups", garr);

                const bool wrote = dir.getChildFile ("project.json").replaceWithText (juce::JSON::toString (juce::var (root)));
                if (! wrote)      titleLabel.setText ("Save failed: couldn't write project.json.", juce::dontSendNotification);
                else if (! allOk) titleLabel.setText ("Saved (some media kept by reference): " + dir.getFileName(), juce::dontSendNotification);
                else              titleLabel.setText ("Saved: " + dir.getFileName(), juce::dontSendNotification);
            });
    }

    void openProject()
    {
        chooser = std::make_unique<juce::FileChooser> ("Open project", juce::File ("~/Desktop"), "*.lbproj;*.json");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                restoreKeyFocus();
                const auto sel = fc.getResult();
                if (sel == juce::File()) return;
                const juce::File jf  = sel.isDirectory() ? sel.getChildFile ("project.json") : sel;
                const juce::File dir = jf.getParentDirectory();
                if (! jf.existsAsFile()) { titleLabel.setText ("No project.json found.", juce::dontSendNotification); return; }
                const auto root = juce::JSON::parse (jf.loadFileAsString());
                if (! root.isObject()) { titleLabel.setText ("Invalid project file.", juce::dontSendNotification); return; }
                loadProjectFromVar (root, dir);
            });
    }

    void newProject()
    {
        pauseAll();
        closeAllPluginWindows();   // their processors live in the engine tracks we're about to remove
        for (auto& g : groups)
            for (auto& t : g->tracks)
            { if (t->thumb) { t->thumb->removeChangeListener (this); t->thumb->setSource (nullptr); } audioEngine.removeTrack (t->engineId); }
        groups.clear();
        timeline.setGroups (&groups);
        activeGroup = -1; selGroup = selTrack = selClip = -1;
        playhead = 0.0; reachedEnd = false; loopEnabled = false; loopStart = loopEnd = 0.0; lastMinLen = -1.0;
        audioEngine.setMasterGain (1.0f);
        clearHistory();
        loopToggle.setToggleState (false, juce::dontSendNotification);
        timeline.setActiveGroup (-1); timeline.setSelection (-1, -1, -1); timeline.setLoop (false, 0.0, 0.0); timeline.setPlayhead (0.0);
        titleLabel.setText ("Layback Station", juce::dontSendNotification);
        refreshMixer();
        resized(); timeline.repaint();
    }

    void loadProjectFromVar (const juce::var& root, const juce::File& baseDir)
    {
        newProject();

        if ((int) root.getProperty ("version", 1) > 1)
        { titleLabel.setText ("This project was saved by a newer version of Layback.", juce::dontSendNotification); return; }

        int missingMedia = 0;

        auto resolve = [&] (const juce::String& rel) -> juce::File
        {
            if (juce::File::isAbsolutePath (rel)) { juce::File f (rel); if (f.existsAsFile()) return f; }
            return baseDir.getChildFile (rel);
        };

        if (auto* garr = root["groups"].getArray())
            for (auto& gv : *garr)
            {
                auto grp = std::make_unique<VideoGroup>();
                grp->name      = gv["name"].toString();
                grp->file      = resolve (gv["video"].toString());
                grp->duration  = (double) gv.getProperty ("duration", 0.0);
                grp->expanded  = (bool)   gv.getProperty ("expanded", true);
                grp->videoMute = (bool)   gv.getProperty ("videoMute", false);
                grp->videoSolo = (bool)   gv.getProperty ("videoSolo", false);
                grp->cutMarkers = varToDoubles (gv["cuts"]);
                if (! grp->file.existsAsFile()) ++missingMedia;

                if (auto* tarr = gv["tracks"].getArray())
                    for (auto& tv : *tarr)
                    {
                        const juce::File file = resolve (tv["file"].toString());
                        double len = 0.0;
                        const int id = audioEngine.addTrack (file, len);   // -1 if the media is missing/unreadable

                        auto tr = std::make_unique<AudioTrack>();
                        tr->name         = tv["name"].toString();
                        tr->file         = file;
                        tr->engineId     = id;          // keep the track even on failure so its edits survive a round-trip
                        tr->sourceLength = (double) tv.getProperty ("sourceLength", 0.0); if (tr->sourceLength <= 0.0) tr->sourceLength = len;
                        tr->mute         = (bool) tv.getProperty ("mute", false);
                        tr->solo         = (bool) tv.getProperty ("solo", false);
                        tr->beatMarkers  = varToDoubles (tv["beats"]);
                        if (auto* carr = tv["clips"].getArray())
                            for (auto& cv : *carr)
                                tr->clips.push_back ({ (double) cv["start"], (double) cv["in"], (double) cv["dur"] });
                        if (tr->clips.empty() && tr->sourceLength > 0.0)
                            tr->clips.push_back ({ 0.0, 0.0, tr->sourceLength });   // never silently empty

                        if (id < 0)
                        {
                            ++missingMedia;
                        }
                        else
                        {
                            tr->thumb = std::make_unique<juce::AudioThumbnail> (512, audioEngine.getFormatManager(), thumbnailCache);
                            tr->thumb->addChangeListener (this);
                            tr->thumb->setSource (new juce::FileInputSource (file));
                        }
                        grp->tracks.push_back (std::move (tr));
                    }
                groups.push_back (std::move (grp));
            }

        timeline.setGroups (&groups);
        loopEnabled = (bool)   root.getProperty ("loopEnabled", false);
        loopStart   = (double) root.getProperty ("loopStart", 0.0);
        loopEnd     = (double) root.getProperty ("loopEnd", 0.0);
        audioEngine.setMasterGain ((float) root.getProperty ("masterGain", 1.0));
        if (groups.empty()) { loopEnabled = false; loopStart = loopEnd = 0.0; }
        loopToggle.setToggleState (loopEnabled, juce::dontSendNotification);

        if (! groups.empty())
        {
            const int ag = (int) root["activeGroup"];
            activeGroup = -1;
            activateGroup (juce::jlimit (0, (int) groups.size() - 1, ag < 0 ? 0 : ag));
        }
        timeline.setLoop (loopEnabled, loopStart, loopEnd);
        resized(); timeline.repaint();
        if (missingMedia > 0)
            titleLabel.setText ("Opened with " + juce::String (missingMedia) + " missing media file(s): " + baseDir.getFileName(), juce::dontSendNotification);
        else
            titleLabel.setText ("Opened: " + baseDir.getFileName(), juce::dontSendNotification);
    }

    void showProjectMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "New project");
        m.addItem (2, "Open project...");
        m.addItem (3, "Save project...");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&projectButton),
            [this] (int r)
            {
                if      (r == 1) newProject();
                else if (r == 2) openProject();
                else if (r == 3) saveProject();
                restoreKeyFocus();
            });
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { timeline.repaint(); }

    void timerCallback() override
    {
        if (mixerVisible) mixerView.updateMeters();

        const double vdur = videoDur();
        auto* ag = activeGroupPtr();
        if (vdur > 0.0 && ag != nullptr && ! juce::approximatelyEqual (ag->duration, vdur))
            ag->duration = vdur;   // store the loaded video's precise duration back into the group
        const double minLen = (vdur > 0.0) ? vdur : (ag != nullptr ? ag->duration : 0.0);
        if (! juce::approximatelyEqual (minLen, lastMinLen))
        {
            lastMinLen = minLen;
            audioEngine.setMinLengthSeconds (minLen);
        }

        if (playing)
        {
            double ph = audioEngine.getPositionSeconds();
            const double tl = timelineLength();

            if (loopEnabled && loopEnd > loopStart)
            {
                const double le = juce::jmin (loopEnd, tl);
                if (ph >= le - 0.01)
                {
                    ph = loopStart;
                    audioEngine.setPositionSeconds (loopStart);
                    if (! audioEngine.isPlaying()) audioEngine.play();
                    video.setPositionSeconds (loopStart);
                    if (loopStart < vdur - 0.001) video.play();
                }
            }
            else if ((tl > 0.0 && ph >= tl - 0.05) || ! audioEngine.isPlaying())
            {
                reachedEnd = true;
                pauseAll();
            }

            playhead = ph;

            if (playing && video.isPlaying() && playhead < vdur - 0.05)
            {
                double d = video.getPositionSeconds() - playhead;
                if (d < 0) d = -d;
                if (d > 0.25) video.setPositionSeconds (playhead);
            }

            if (! isScrubbing) timeline.setPlayhead (playhead);
        }

        timeLabel.setText (formatTime (playhead) + "  /  " + formatTime (timelineLength()), juce::dontSendNotification);
        logicBar.setPosition (formatTime (playhead), formatTime (timelineLength()));
        logicBar.setMasterLevel (audioEngine.getMasterPeak());
        logicInspector.updateMeters();
        playButton.setButtonText (playing ? "Pause" : "Play");
    }

    static juce::String formatTime (double s)
    {
        if (s < 0.0) s = 0.0;
        const int total = (int) s;
        const int hund  = (int) ((s - total) * 100.0);
        return juce::String::formatted ("%d:%02d.%02d", total / 60, total % 60, hund);
    }

    //==========================================================================
    // Declaration order for teardown: track thumbnails reference audioEngine's
    // format manager + thumbnailCache (declared first); timeline reads &groups
    // (declared before timeline).
    LaybackLookAndFeel laf;   // declared first -> destroyed last (after every component that uses it)
    VideoView video;
    AudioEngine audioEngine;
    juce::AudioThumbnailCache thumbnailCache { 32 };
    std::vector<std::unique_ptr<VideoGroup>> groups;
    TimelineComponent timeline;
    juce::Viewport timelineViewport;

    juce::TextButton openButton, playButton, exportButton, keysButton, projectButton, videoButton;
    juce::ToggleButton loopToggle, snapToggle;
    juce::ApplicationCommandManager commandManager;
    KeyProfile keyProfile = KeyProfile::Layback;
    juce::Label timeLabel, titleLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> transportBand, viewerFrame, laybackWordmark, logicToolbar;

    std::unique_ptr<VideoWindow> videoWindow;   // destroyed in ~MainComponent before `video`
    bool videoWindowOpen = false;
    std::vector<std::unique_ptr<PluginWindow>> pluginWindows;
    std::map<int, juce::PluginDescription>     pluginMenuMap;   // menu id -> plugin to instantiate
    bool scanning = false;
    bool pluginsScanned = false;

    MixerView mixerView;
    bool      mixerVisible = false;
    LogicControlBar logicBar;     // shown only for the Logic station
    LogicInspector  logicInspector;

    int    activeGroup = -1;
    int    selGroup = -1, selTrack = -1, selClip = -1;

    double playhead    = 0.0;
    bool   playing     = false;
    bool   isScrubbing = false;
    bool   wasPlaying  = false;
    bool   reachedEnd  = false;
    double lastMinLen  = -1.0;

    bool   loopEnabled = false;
    double loopStart   = 0.0, loopEnd = 0.0;

    std::vector<EditSnapshot> undoStack, redoStack;
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

//==============================================================================
class LaybackApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Layback Station"; }
    const juce::String getApplicationVersion() override { return "0.0.1"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String&) override { mainWindow.reset (new MainWindow (getApplicationName())); }
    void shutdown() override { mainWindow = nullptr; }
    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            centreWithSize (1120, 820);
            setVisible (true);
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
START_JUCE_APPLICATION (LaybackApplication)
