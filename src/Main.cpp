#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <map>
#include <juce_gui_extra/juce_gui_extra.h>
#include "VideoView.h"
#include "TimelineComponent.h"
#include "AudioEngine.h"
#include "FfmpegTool.h"
#include "StemSplitter.h"
#include "Clip.h"
#include "Track.h"
#include "LaybackLookAndFeel.h"
#include "MixerView.h"
#include "LogicControlBar.h"
#include "LogicInspector.h"
#include "ProToolsControlBar.h"

//==============================================================================
// Multi-video: a project is a stack of video groups (each = one video + its own
// audio tracks). One group is "active": its video previews and its audio plays.
// The audio engine holds every track's audio in RAM; only the active group's
// tracks carry clips (others are silent), so switching groups is instant.
static juce::var doublesToVar (const std::vector<double>& v) { juce::var a; for (double d : v) a.append (d); return a; }
static std::vector<double> varToDoubles (const juce::var& v) { std::vector<double> r; if (auto* arr = v.getArray()) for (auto& e : *arr) r.push_back ((double) e); return r; }

struct EditSnapshot
{
    struct TrackS { bool mute = false, solo = false; float volume = 1.0f, pan = 0.0f, send = 0.0f;
                    int mixGroup = 0; bool automationOn = false;
                    std::vector<AudioClip> clips; std::vector<AutoPoint> volumeAuto; };
    struct GroupS { bool vMute = false, vSolo = false; std::vector<TrackS> tracks; };
    std::vector<GroupS> groups;
    bool loopEnabled = false; double loopStart = 0.0, loopEnd = 0.0;
    int selGroup = -1, selTrack = -1, selClip = -1;
};

namespace LSCmd { enum { TogglePlay = 0x6001, Split, DeleteClip, ToggleLoop, ToggleSnap, NudgeLeft, NudgeRight, Undo, Redo,
                          AddMarker, PrevMarker, NextMarker, ZoomFit, GoToTimecode,
                          CopyClip, PasteClip, DuplicateClip, ToggleAutomation }; }

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
/** The Plugins window: browse / scan available AU & VST3 plugins (search, failure
    list, rescan), backed by the engine's KnownPluginList. */
struct PluginListWindow : public juce::DocumentWindow
{
    std::function<void()> onClose;
    PluginListWindow (juce::AudioPluginFormatManager& fmts, juce::KnownPluginList& list,
                      const juce::File& deadMans, juce::Colour bg)
        : juce::DocumentWindow ("Plugins", bg, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new juce::PluginListComponent (fmts, list, deadMans, nullptr, true), true);
        setResizable (true, false);
        setSize (660, 480);
        setTopLeftPosition (180, 120);
        setVisible (true);
    }
    void closeButtonPressed() override { if (onClose) onClose(); }
};

//==============================================================================
/** A simple read-only text window (Help / Shortcuts). */
struct InfoWindow : public juce::DocumentWindow
{
    std::function<void()> onClose;
    InfoWindow (const juce::String& title, const juce::String& body, juce::Colour bg, juce::Colour fg)
        : juce::DocumentWindow (title, bg, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        auto* te = new juce::TextEditor();
        te->setMultiLine (true); te->setReadOnly (true); te->setScrollbarsShown (true); te->setCaretVisible (false);
        te->setColour (juce::TextEditor::backgroundColourId, bg);
        te->setColour (juce::TextEditor::textColourId, fg);
        te->setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        te->setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain)));
        te->setText (body, false);
        setContentOwned (te, false);
        setResizable (true, false);
        setSize (560, 640);
        setTopLeftPosition (200, 110);
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

//==============================================================================
/** Tracks the external child processes we spawn (Demucs, uv installer, ffmpeg) so
    they can be killed on quit instead of lingering as zombie CPU hogs. Held by a
    shared_ptr captured into worker threads, so it safely outlives the window. */
struct ProcRegistry
{
    std::mutex mx;
    std::vector<std::shared_ptr<juce::ChildProcess>> procs;

    void add    (std::shared_ptr<juce::ChildProcess> p) { std::lock_guard<std::mutex> l (mx); procs.push_back (std::move (p)); }
    void remove (const std::shared_ptr<juce::ChildProcess>& p) { std::lock_guard<std::mutex> l (mx); procs.erase (std::remove (procs.begin(), procs.end(), p), procs.end()); }
    void killAll() { std::lock_guard<std::mutex> l (mx); for (auto& p : procs) if (p && p->isRunning()) p->kill(); procs.clear(); }
};

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ChangeListener,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      public juce::FileDragAndDropTarget
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
        logicBar.onRecord = [this] { toggleRecord(); };
        logicBar.onCycle  = [this] { loopToggle.setToggleState (! loopEnabled, juce::dontSendNotification); toggleLoop(); };
        logicBar.onLibrary = [this] { openPluginListWindow(); };
        logicBar.onMixer   = [this] { toggleMixer(); };
        logicBar.isPlaying = [this] { return playing; };
        logicBar.isCycle   = [this] { return loopEnabled; };
        addChildComponent (logicBar);

        logicInspector.setEngine (&audioEngine);
        logicInspector.onVolume = [this] (int g, int t, float v) { changeTrackVolume (g, t, v); };
        logicInspector.onPan    = [this] (int g, int t, float p) { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->pan = p; audioEngine.setTrackPan (groups[(size_t) g]->tracks[(size_t) t]->engineId, p); if (mixerVisible) mixerView.syncFromModel(); } };
        logicInspector.onMute   = [this] (int g, int t, bool b)  { setTrackMute (g, t, b); };
        logicInspector.onSolo   = [this] (int g, int t, bool b)  { setTrackSolo (g, t, b); };
        logicInspector.onFxMenu = [this] (int g, int t) { showTrackMenu (g, t); };
        logicInspector.onMasterVolume = [this] (float v) { audioEngine.setMasterGain (v); };
        logicInspector.onMasterMute   = [this] (bool b)  { audioEngine.setMasterMute (b); if (mixerVisible) mixerView.syncFromModel(); };
        addChildComponent (logicInspector);

        ptBar.onRewind = [this] { seekAll (0.0); };
        ptBar.onStop   = [this] { pauseAll(); seekAll (0.0); };
        ptBar.onPlay   = [this] { togglePlay(); };
        ptBar.onRecord = [this] { toggleRecord(); };
        ptBar.onLoop   = [this] { loopToggle.setToggleState (! loopEnabled, juce::dontSendNotification); toggleLoop(); };
        ptBar.onMode   = [this] (int mode) { applyPtEditMode (mode); };
        ptBar.onTool   = [this] (int t)
        {
            using ET = TimelineComponent::EditTool;
            const ET tools[] = { ET::Zoom, ET::Trim, ET::Grab, ET::Scrub, ET::Smart };   // zoom/trim/grab/scrub/smart
            timeline.setEditTool (tools[juce::jlimit (0, 4, t)]);
        };
        ptBar.isPlaying = [this] { return playing; };
        ptBar.isLoop    = [this] { return loopEnabled; };
        addChildComponent (ptBar);

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
        timeline.onTrackMute     = [this] (int g, int t) { if (validTrack (g, t)) setTrackMute (g, t, ! groups[(size_t) g]->tracks[(size_t) t]->mute); };
        timeline.onTrackSolo     = [this] (int g, int t) { if (validTrack (g, t)) setTrackSolo (g, t, ! groups[(size_t) g]->tracks[(size_t) t]->solo); };
        timeline.onTrackRecord   = [this] (int g, int t) { if (validTrack (g, t)) { auto& tr = *groups[(size_t) g]->tracks[(size_t) t]; tr.recordArm = ! tr.recordArm; timeline.repaint(); } };
        timeline.onTrackVolume   = [this] (int g, int t, float v) { changeTrackVolume (g, t, v); };
        timeline.onTrackPan      = [this] (int g, int t, float p) { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->pan = p; audioEngine.setTrackPan (groups[(size_t) g]->tracks[(size_t) t]->engineId, p); timeline.repaint(); if (mixerVisible) mixerView.syncFromModel(); refreshInspector(); } };
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
        timeline.onZoomChanged   = [this] { updateTimelineSize(); };
        timeline.onMarkerRename  = [this] (int i) { renameMarker (i); };
        timeline.onAutoEdit      = [this] (int g, int t, std::vector<AutoPoint> pts)
        {
            if (! validTrack (g, t)) return;
            groups[(size_t) g]->tracks[(size_t) t]->volumeAuto = std::move (pts);
            pushTrackAutomation (g, t);
        };
        timeline.setGroups (&groups);
        timelineViewport.setViewedComponent (&timeline, false);
        timelineViewport.setScrollBarsShown (true, true);   // vertical + horizontal (fixed-zoom scroll)
        addAndMakeVisible (timelineViewport);

        mixerView.setEngine (&audioEngine);
        mixerView.onVolume = [this] (int g, int t, float v) { changeTrackVolume (g, t, v); };
        mixerView.onPan    = [this] (int g, int t, float p) { if (validTrack (g, t)) { auto& tr = *groups[(size_t) g]->tracks[(size_t) t]; tr.pan = p; audioEngine.setTrackPan (tr.engineId, p); } };
        mixerView.onMute   = [this] (int g, int t, bool b)  { setTrackMute (g, t, b); };
        mixerView.onSolo   = [this] (int g, int t, bool b)  { setTrackSolo (g, t, b); };
        mixerView.onGroupChange = [this] (int g, int t, int gid) { if (validTrack (g, t)) { pushUndo(); groups[(size_t) g]->tracks[(size_t) t]->mixGroup = gid; } };
        mixerView.onFxMenu = [this] (int g, int t) { showTrackMenu (g, t); };
        mixerView.onSelect = [this] (int g, int t) { selGroup = g; selTrack = t; selClip = -1; timeline.setSelection (g, t, -1); };
        mixerView.onMasterVolume = [this] (float v) { audioEngine.setMasterGain (v); };
        mixerView.onMasterMute   = [this] (bool b)  { audioEngine.setMasterMute (b); refreshInspector(); };
        mixerView.onRecordArm    = [this] (int g, int t, bool b) { if (validTrack (g, t)) { groups[(size_t) g]->tracks[(size_t) t]->recordArm = b; timeline.repaint(); } };
        mixerView.onBounce       = [this] { exportAudio(); };
        mixerView.onSend         = [this] (int g, int t, float v) { if (validTrack (g, t)) { auto& tr = *groups[(size_t) g]->tracks[(size_t) t]; tr.send = v; audioEngine.setTrackSend (tr.engineId, v); } };
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
        applyKeyProfile (loadSavedSkin());     // restore the user's last-used skin (Logic if unset)

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

        auto a = alive;                     // offer to recover an unsaved session from a previous run
        juce::MessageManager::callAsync ([this, a] { if (a->load()) maybeOfferRecovery(); });
    }

    ~MainComponent() override
    {
        juce::MenuBarModel::setMacMainMenu (nullptr);
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        setLookAndFeel (nullptr);
        if (alive) *alive = false;   // in-flight ffmpeg/scan worker callbacks bail instead of touching a dead window
        procReg->killAll();          // kill any running Demucs/uv/ffmpeg child processes so they don't linger
        videoWindow.reset();         // release the video pop-out before the VideoView member is destroyed
        pluginListWindow.reset();    // close the plugin browser before the engine list it edits goes away
        helpWindow.reset();
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

    //== drag-and-drop import from Finder ==
    static bool isAudioExt (const juce::String& e)
    { return juce::StringArray ({ ".wav",".mp3",".m4a",".aif",".aiff",".flac",".ogg",".aac",".caf",".wma" }).contains (e); }
    static bool isVideoExt (const juce::String& e)
    { return juce::StringArray ({ ".mov",".mp4",".m4v",".qt",".avi",".mpg",".mpeg",".m2v",".m2ts",".mts",".ts",".3gp",".mxf",".dv",".mkv",".webm",".wmv",".flv" }).contains (e); }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (auto& f : files) { const auto e = juce::File (f).getFileExtension().toLowerCase(); if (isAudioExt (e) || isVideoExt (e)) return true; }
        return false;
    }
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        int added = 0; bool needAudioHome = false;
        for (auto& f : files)
        {
            const juce::File file (f);
            const auto e = file.getFileExtension().toLowerCase();
            if (isVideoExt (e))      { addVideo (file); ++added; }
            else if (isAudioExt (e)) { if (activeGroup >= 0) { addTrackFromFile (activeGroup, file); ++added; } else needAudioHome = true; }
        }
        if (added) { resized(); timeline.repaint(); }
        if (needAudioHome && added == 0)
            titleLabel.setText ("Add a video first, then drag songs onto it", juce::dontSendNotification);
    }

    void addTrackFromFile (int g, const juce::File& f)
    {
        if (! validGroup (g)) return;

        double len = 0.0;
        const int id = audioEngine.addTrack (f, len);
        if (id < 0) { titleLabel.setText ("Couldn't read \"" + f.getFileName() + "\" - unsupported audio format.", juce::dontSendNotification); return; }

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

    //== Stem splitting (built-in source separation via Demucs) ==
    // One-time setup: build the Demucs venv (uv fetches Python 3.11 + installs demucs + numpy<2).
    void installStemSplitter (std::function<void (bool)> onDone)
    {
        if (installing) return;
        const juce::File uv = StemSplitter::findUv();
        if (uv == juce::File())
        {
            titleLabel.setText ("Couldn't find 'uv' to set up the stem splitter - install uv, then retry", juce::dontSendNotification);
            if (onDone) onDone (false);
            return;
        }
        installing = true;
        titleLabel.setText ("Setting up stem splitter (one-time, ~200MB download, a few minutes)...", juce::dontSendNotification);

        const juce::File venvDir = StemSplitter::venvDir();
        const juce::File venvPy  = StemSplitter::venvPython();
        auto a = alive; auto reg = procReg;
        std::thread ([this, a, reg, uv, venvDir, venvPy, onDone]
        {
            auto run = [reg] (const juce::File& exe, const juce::StringArray& args) -> int
            {
                auto p = std::make_shared<juce::ChildProcess>();   // killable on quit
                juce::StringArray cmd; cmd.add (exe.getFullPathName()); cmd.addArray (args);
                reg->add (p);
                int code = -1;
                if (p->start (cmd, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
                { p->readAllProcessOutput(); code = p->getExitCode(); }
                reg->remove (p);
                return code;
            };
            venvDir.getParentDirectory().createDirectory();
            bool ok = run (uv, { "venv", "--python", "3.11", venvDir.getFullPathName() }) == 0;
            if (ok) ok = run (uv, { "pip", "install", "--python", venvPy.getFullPathName(), "demucs", "numpy<2" }) == 0;
            const bool installed = ok && venvPy.existsAsFile();
            juce::MessageManager::callAsync ([this, a, installed, onDone]
            {
                if (! a->load()) return;
                installing = false;
                titleLabel.setText (installed ? "Stem splitter ready" : "Stem splitter setup failed - check 'uv' + network",
                                    juce::dontSendNotification);
                if (onDone) onDone (installed);
            });
        }).detach();
    }

    // Delete split-stem folders older than two weeks so the app-support cache doesn't grow forever.
    void pruneOldStems (const juce::File& root)
    {
        if (! root.isDirectory()) return;
        const auto now = juce::Time::getCurrentTime();
        for (auto& d : root.findChildFiles (juce::File::findDirectories, false))
            if ((now - d.getLastModificationTime()).inDays() > 14.0)
                d.deleteRecursively();
    }

    // Add a stem as a new track, placed to match the source track's clips. Returns the new track index (-1 on failure).
    int importStemTrack (int g, const juce::File& f, const juce::String& name, const std::vector<AudioClip>& clipsToUse)
    {
        if (! validGroup (g) || ! f.existsAsFile()) return -1;
        double len = 0.0;
        const int id = audioEngine.addTrack (f, len);
        if (id < 0) return -1;

        auto tr = std::make_unique<AudioTrack>();
        tr->name = name; tr->file = f; tr->engineId = id; tr->sourceLength = len;
        if (clipsToUse.empty())
            tr->clips.push_back ({ 0.0, 0.0, len });
        else
            for (auto c : clipsToUse)   // stems share the source's timebase -> reuse its clip placement
            {
                c.sourceIn = juce::jlimit (0.0, len, c.sourceIn);
                c.duration = juce::jlimit (0.0, len - c.sourceIn, c.duration);
                if (c.duration > 0.01) tr->clips.push_back (c);
            }
        tr->thumb = std::make_unique<juce::AudioThumbnail> (512, audioEngine.getFormatManager(), thumbnailCache);
        tr->thumb->addChangeListener (this);
        tr->thumb->setSource (new juce::FileInputSource (f));

        groups[(size_t) g]->tracks.push_back (std::move (tr));
        audioEngine.setTrackClips (id, (g == activeGroup) ? groups[(size_t) g]->tracks.back()->clips : std::vector<AudioClip>{});
        return (int) groups[(size_t) g]->tracks.size() - 1;
    }

    void splitTrackIntoStems (int g, int t, bool sixStem)
    {
        if (splitting) { titleLabel.setText ("A stem split is already running...", juce::dontSendNotification); return; }
        if (! validTrack (g, t)) { titleLabel.setText ("Select an audio track to split into stems", juce::dontSendNotification); return; }
        if (! StemSplitter::isInstalled())   // first run: set up Demucs once, then split automatically
        {
            if (installing) { titleLabel.setText ("Stem splitter is installing - it'll run when ready...", juce::dontSendNotification); return; }
            installStemSplitter ([this, g, t, sixStem] (bool ok) { if (ok) splitTrackIntoStems (g, t, sixStem); });
            return;
        }

        auto* tr = groups[(size_t) g]->tracks[(size_t) t].get();
        const juce::File src = tr->file;
        if (! src.existsAsFile()) { titleLabel.setText ("This track has no source file to split", juce::dontSendNotification); return; }

        const juce::String baseName = tr->name;
        const std::vector<AudioClip> clips = tr->clips;   // place each stem exactly where the source sits
        // Persistent (not OS-temp, which can be purged out from under a reopened project); saving a project
        // copies these into the .lbproj bundle anyway. Prune stale folders so they don't pile up.
        const juce::File stemsRoot = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                        .getChildFile ("Layback").getChildFile ("stems");
        pruneOldStems (stemsRoot);
        const juce::File outDir = stemsRoot.getChildFile (juce::String ((juce::int64) juce::Time::getMillisecondCounter()));

        splitting = true;
        titleLabel.setText (juce::String ("Splitting into ") + (sixStem ? "6" : "4")
                            + " stems (offline render, give it a minute)...", juce::dontSendNotification);

        auto a = alive; auto reg = procReg;
        auto proc = std::make_shared<juce::ChildProcess>();
        reg->add (proc);                                       // killable on quit
        stemProc = proc; splitCancelled = false; splitElapsedTicks = 0;
        std::thread ([this, a, reg, proc, src, outDir, sixStem, g, baseName, clips]
        {
            juce::String err; juce::Array<juce::File> stems;
            const bool ok = StemSplitter::separate (src, outDir, sixStem, *proc, err, stems);
            reg->remove (proc);
            juce::MessageManager::callAsync ([this, a, ok, err, stems, g, baseName, clips]
            {
                if (! a->load()) return;
                splitting = false;
                if (! ok) { titleLabel.setText (splitCancelled ? "Stem split cancelled" : ("Stem split failed: " + err), juce::dontSendNotification); return; }
                int first = -1;
                for (auto& f : stems)
                {
                    const int idx = importStemTrack (g, f, baseName + " - " + f.getFileNameWithoutExtension(), clips);
                    if (first < 0) first = idx;
                }
                if (first >= 0) { selGroup = g; selTrack = first; selClip = 0; timeline.setSelection (selGroup, selTrack, selClip); }
                clearHistory(); applyMixGains(); refreshMixer(); updateTimelineSize(); resized(); timeline.repaint();
                titleLabel.setText (juce::String ((int) stems.size()) + " stems added from \"" + baseName + "\"", juce::dontSendNotification);
            });
        }).detach();
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
            g.setColour (s.text);
            g.setFont (juce::Font (juce::FontOptions().withHeight (11.5f)));
            g.drawText ("Edit",      logicMEdit, juce::Justification::centred, false);
            g.drawText ("Functions", logicMFunc, juce::Justification::centred, false);
            g.drawText ("View",      logicMView, juce::Justification::centred, false);
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
            case 3: layoutProTools();        break;   // Pro Tools: green-counter transport bar
            case 2: layoutAbleton();         break;   // Ableton
            default: layoutDefault();        break;   // Layback
        }
    }

    // Show the right top chrome for the active station (Logic bar / Pro Tools bar / generic toolbar).
    void setChrome()
    {
        const int lay = laf.skin.layout;
        const bool logic = (lay == 1), pt = (lay == 3);
        logicBar.setVisible (logic);
        logicInspector.setVisible (logic);
        ptBar.setVisible (pt);
        const bool generic = ! logic && ! pt;
        for (auto* c : std::initializer_list<juce::Component*> { &openButton, &playButton, &exportButton, &keysButton, &projectButton, &videoButton })
            c->setVisible (generic);
        loopToggle.setVisible (generic);
        snapToggle.setVisible (generic);
        timeLabel.setVisible (generic);
    }

    // Logic: full-width LCD control bar on top, status line, tracks fill below.
    void layoutLogic()
    {
        setChrome();
        auto area = getLocalBounds();
        transportBand = area.removeFromTop (58);
        logicBar.setBounds (transportBand);

        auto status = area.removeFromTop (18);
        titleLabel.setBounds (status.reduced (10, 0));
        logicToolbar = area.removeFromTop (26);                 // local Edit / Functions / View row
        { const int y = logicToolbar.getY(), h = logicToolbar.getHeight(); const int x = logicToolbar.getX() + 210;
          logicMEdit = { x, y, 46, h }; logicMFunc = { x + 56, y, 78, h }; logicMView = { x + 144, y, 46, h }; }
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
        setChrome();
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
    // Pro Tools: full-width transport bar (green LED counters) on top, tracks fill below.
    void layoutProTools()
    {
        setChrome();
        auto area = getLocalBounds();
        transportBand = area.removeFromTop (60);
        ptBar.setBounds (transportBand);

        auto status = area.removeFromTop (18);
        titleLabel.setBounds (status.reduced (10, 0));

        area.removeFromTop (4);
        if (mixerVisible) { mixerView.setBounds (area.removeFromBottom (210)); area.removeFromBottom (4); }
        timelineViewport.setBounds (area);
        viewerFrame = {};
        updateTimelineSize();
    }

    void layoutStacked (int barH, int counterW)
    {
        setChrome();
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
        setChrome();
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
        const int sb   = timelineViewport.getScrollBarThickness();
        const int visW = juce::jmax (200, timelineViewport.getWidth() - sb);   // visible content width (minus vertical bar)
        timeline.setViewportWidth (visW);
        const int cw   = timeline.contentWidth();
        const bool needH = cw > visW;
        const int w = juce::jmax (visW, cw);
        const int h = juce::jmax (timeline.contentHeight(), timelineViewport.getHeight() - (needH ? sb : 0));
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
        if (ag == nullptr) { video.setMuted (true); videoAudible = false; return; }

        bool anySolo = ag->videoSolo;
        for (auto& t : ag->tracks) if (t->solo) anySolo = true;

        videoAudible = (! ag->videoMute && (! anySolo || ag->videoSolo));
        video.setMuted (! videoAudible);
        for (auto& t : ag->tracks)
        {
            const bool aud = ! t->mute && (! anySolo || t->solo);
            audioEngine.setTrackGain (t->engineId, aud ? t->volume : 0.0f);   // fold in the channel fader
        }
    }

    // Single-track gain update for a fader drag: one engine call instead of re-setting every track.
    static void writeAutoPoint (AudioTrack& tr, double time, float value)
    {
        for (auto& p : tr.volumeAuto) if (std::abs (p.time - time) < 0.08) { p.value = value; return; }   // thin to ~1 point / 80 ms
        tr.volumeAuto.push_back ({ time, value });
        std::sort (tr.volumeAuto.begin(), tr.volumeAuto.end(), [] (const AutoPoint& a, const AutoPoint& b) { return a.time < b.time; });
    }
    // Central volume-change path: sets the fader and, in Latch automation while playing, writes the ride into the envelope.
    void changeTrackVolume (int g, int t, float v)
    {
        if (! validTrack (g, t)) return;
        auto* tr = groups[(size_t) g]->tracks[(size_t) t].get();
        tr->volume = v;
        updateTrackGain (g, t);
        if (autoMode == 1 && automationVisible && playing && g == activeGroup)
        {
            writeAutoPoint (*tr, playhead, v);
            std::vector<std::pair<double, float>> env; for (auto& p : tr->volumeAuto) env.push_back ({ p.time, p.value });
            audioEngine.setTrackAutomation (tr->engineId, env, tr->automationOn);
            timeline.repaint();
        }
        if (tr->mixGroup > 0)   // linked faders: peers in the same mix group follow
            for (int i = 0; i < (int) groups[(size_t) g]->tracks.size(); ++i)
                if (i != t && groups[(size_t) g]->tracks[(size_t) i]->mixGroup == tr->mixGroup)
                { groups[(size_t) g]->tracks[(size_t) i]->volume = v; updateTrackGain (g, i); }
        if (mixerVisible) mixerView.syncFromModel();
        refreshInspector();
    }
    void setTrackMute (int g, int t, bool b)
    {
        if (! validTrack (g, t)) return;
        auto* tr = groups[(size_t) g]->tracks[(size_t) t].get();
        tr->mute = b;
        if (tr->mixGroup > 0) for (auto& pt : groups[(size_t) g]->tracks) if (pt->mixGroup == tr->mixGroup) pt->mute = b;
        applyMixGains(); timeline.repaint();
        if (mixerVisible) mixerView.syncFromModel();
    }
    void setTrackSolo (int g, int t, bool b)
    {
        if (! validTrack (g, t)) return;
        auto* tr = groups[(size_t) g]->tracks[(size_t) t].get();
        tr->solo = b;
        if (tr->mixGroup > 0) for (auto& pt : groups[(size_t) g]->tracks) if (pt->mixGroup == tr->mixGroup) pt->solo = b;
        applyMixGains(); timeline.repaint();
        if (mixerVisible) mixerView.syncFromModel();
    }

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

    //== Recording: capture the input device, drop the take as a new track at the record point ==
    void toggleRecord()
    {
        if (audioEngine.isRecording()) { finishRecording(); return; }
        if (! audioEngine.hasAudioInput()) { titleLabel.setText ("No audio input device available (check mic permission / input).", juce::dontSendNotification); return; }
        if (! validGroup (activeGroup)) { titleLabel.setText ("Add a video first, then record into it.", juce::dontSendNotification); return; }
        recordStartTime = playhead;
        recordTicks = 0;
        audioEngine.startRecording();
        if (! playing) playAll();        // roll so you hear the mix while recording
        titleLabel.setText ("Recording...", juce::dontSendNotification);
    }
    void finishRecording()
    {
        auto take = audioEngine.stopRecording();
        const double sr = audioEngine.getDeviceSampleRate();
        pauseAll();
        if (take.getNumSamples() < (int) (sr * 0.05)) { titleLabel.setText ("Nothing recorded.", juce::dontSendNotification); return; }

        const juce::File dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                   .getChildFile ("Layback").getChildFile ("recordings");
        dir.createDirectory();
        const juce::File wav = dir.getChildFile ("take_" + juce::String ((juce::int64) juce::Time::getMillisecondCounter()) + ".wav");
        {
            juce::WavAudioFormat fmt;
            if (auto os = std::unique_ptr<juce::FileOutputStream> (wav.createOutputStream()))
                if (auto w = std::unique_ptr<juce::AudioFormatWriter> (fmt.createWriterFor (os.get(), sr, 2, 24, {}, 0)))
                { os.release(); w->writeFromAudioSampleBuffer (take, 0, take.getNumSamples()); }
        }
        if (! wav.existsAsFile()) { titleLabel.setText ("Couldn't write the recording.", juce::dontSendNotification); return; }

        const int g = activeGroup;
        addTrackFromFile (g, wav);                          // adds the take as a new track (clip at 0)
        auto& tracks = groups[(size_t) g]->tracks;
        if (! tracks.empty())
        {
            auto* nt = tracks.back().get();
            nt->name = "Take " + juce::String ((int) tracks.size());
            if (! nt->clips.empty()) { nt->clips[0].timelineStart = recordStartTime; audioEngine.setTrackClips (nt->engineId, nt->clips); }
        }
        updateTimelineSize(); resized(); timeline.repaint();
        titleLabel.setText ("Recorded take added at " + formatTimecode (recordStartTime), juce::dontSendNotification);
    }

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
            updateTimelineSize();   // content length may have changed -> update scroll range (zoom stays fixed)
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
                updateTimelineSize();
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
            updateTimelineSize();
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
        m.addSeparator();
        const char* shapes[] = { "Linear", "Exponential", "S-Curve (Bell)", "Logarithmic" };
        juce::PopupMenu fin, fout;
        for (int i = 0; i < 4; ++i) { fin.addItem (10 + i, shapes[i]); fout.addItem (20 + i, shapes[i]); }
        m.addSubMenu ("Fade In", fin);
        m.addSubMenu ("Fade Out", fout);
        m.addItem (5, "Remove Fades");
        m.addItem (7, "Crossfade with Previous");
        m.addSeparator();
        m.addItem (8, "Normalize");
        m.addItem (9, "Time-Stretch...");
        m.addItem (30, "Speed Fade In (spin up)");
        m.addItem (31, "Speed Fade Out (slow down)");
        m.addItem (6, "Reset Clip Gain (0 dB)");
        m.showMenuAsync (juce::PopupMenu::Options(),
            [this, g, t, c, tm] (int r)
            {
                if      (r == 1) splitTrackClip (g, t, tm);
                else if (r == 2) deleteClip (g, t, c);
                else if (r >= 10 && r <= 13) applyFade (0, r - 10);
                else if (r >= 20 && r <= 23) applyFade (1, r - 20);
                else if (r == 5) applyFade (2);
                else if (r == 7) crossfadeWithPrevious();
                else if (r == 8) normalizeSelectedClip();
                else if (r == 9) timeStretchSelectedClip();
                else if (r == 30) applySpeedFade (true, false);
                else if (r == 31) applySpeedFade (false, true);
                else if (r == 6 && validClip (g, t, c))
                {
                    pushUndo();
                    AudioClip nc = groups[(size_t) g]->tracks[(size_t) t]->clips[(size_t) c];
                    nc.gainDb = 0.0f;
                    clipChanged (g, t, c, nc);
                }
                restoreKeyFocus();
            });
    }

    // Fade the selected clip. mode: 0 = fade in, 1 = fade out, 2 = remove both.
    // shape: 0 linear, 1 exponential, 2 s-curve (bell), 3 logarithmic.
    void applyFade (int mode, int shape = 0)
    {
        if (! validClip (selGroup, selTrack, selClip)) return;
        pushUndo();
        AudioClip nc = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->clips[(size_t) selClip];
        const double f = juce::jmax (0.05, juce::jmin (1.0, nc.duration * 0.5));
        if      (mode == 0) { nc.fadeIn  = (nc.fadeIn  > 0.0 ? nc.fadeIn  : f); nc.fadeInShape  = shape; }
        else if (mode == 1) { nc.fadeOut = (nc.fadeOut > 0.0 ? nc.fadeOut : f); nc.fadeOutShape = shape; }
        else                { nc.fadeIn = 0.0; nc.fadeOut = 0.0; }
        clipChanged (selGroup, selTrack, selClip, nc);
        timeline.repaint();
    }

    // Set the selected clip's gain so its loudest peak hits ~ -0.3 dBFS.
    void normalizeSelectedClip()
    {
        if (! validClip (selGroup, selTrack, selClip)) { titleLabel.setText ("Select a clip to normalize", juce::dontSendNotification); return; }
        auto* tr = groups[(size_t) selGroup]->tracks[(size_t) selTrack].get();
        const auto& c = tr->clips[(size_t) selClip];
        const float pk = audioEngine.clipPeak (tr->engineId, c.sourceIn, c.duration);
        if (pk <= 1.0e-4f) { titleLabel.setText ("Clip is silent - nothing to normalize", juce::dontSendNotification); return; }
        pushUndo();
        AudioClip nc = c;
        nc.gainDb = juce::jlimit (-24.0f, 24.0f, juce::Decibels::gainToDecibels (1.0f / pk) - 0.3f);
        clipChanged (selGroup, selTrack, selClip, nc);
        titleLabel.setText ("Normalized (" + juce::String (nc.gainDb, 1) + " dB)", juce::dontSendNotification);
    }

    // Crossfade the selected clip with the previous clip on its track (extends the previous tail to overlap).
    void crossfadeWithPrevious()
    {
        if (! validClip (selGroup, selTrack, selClip)) { titleLabel.setText ("Select a clip to crossfade", juce::dontSendNotification); return; }
        auto& cl = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->clips;
        const double selStart = cl[(size_t) selClip].timelineStart;
        int prev = -1; double bestEnd = -1.0;
        for (int i = 0; i < (int) cl.size(); ++i)
            if (i != selClip && cl[(size_t) i].timelineStart < selStart && cl[(size_t) i].timelineEnd() > bestEnd)
            { bestEnd = cl[(size_t) i].timelineEnd(); prev = i; }
        if (prev < 0) { titleLabel.setText ("No earlier clip on this track to crossfade with", juce::dontSendNotification); return; }
        pushUndo();
        const double xf = 0.5;
        const double srcLen = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->sourceLength;
        AudioClip p = cl[(size_t) prev];
        const double maxDur = juce::jmax (kMinClipSeconds, (srcLen > 0.0 ? srcLen : p.duration + xf) - p.sourceIn);
        p.duration = juce::jlimit (kMinClipSeconds, maxDur, (selStart + xf) - p.timelineStart);   // extend tail to overlap
        const double overlap = juce::jmax (0.05, p.timelineEnd() - selStart);
        p.fadeOut = overlap;
        AudioClip s = cl[(size_t) selClip]; s.fadeIn = overlap;
        cl[(size_t) prev] = p; cl[(size_t) selClip] = s;
        pushActiveClips (selGroup);
        updateTimelineSize();
        timeline.repaint();
        titleLabel.setText ("Crossfaded (" + juce::String (overlap, 2) + " s)", juce::dontSendNotification);
    }

    // Pitch-preserving time-stretch: fit the selected clip to a new length (SoundTouch, baked offline).
    void timeStretchSelectedClip()
    {
        if (! validClip (selGroup, selTrack, selClip)) { titleLabel.setText ("Select a clip to time-stretch", juce::dontSendNotification); return; }
        const auto& c = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->clips[(size_t) selClip];
        const double srcSeconds = c.duration / juce::jmax (0.01, c.stretchRatio);
        auto* w = new juce::AlertWindow ("Time-Stretch",
                       "Stretch this clip to a new length in seconds (pitch preserved). Source length is "
                       + juce::String (srcSeconds, 2) + " s:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("len", juce::String (c.duration, 2));
        w->addButton ("Stretch", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Reset",   2);
        w->addButton ("Cancel",  0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
        {
            std::unique_ptr<juce::AlertWindow> own (w);
            if      (r == 1) applyClipStretch (w->getTextEditorContents ("len").getDoubleValue());
            else if (r == 2) resetClipStretch();
            restoreKeyFocus();
        }), false);
    }
    void applyClipStretch (double targetLen)
    {
        if (! validClip (selGroup, selTrack, selClip) || targetLen < 0.1) return;
        auto* tr = groups[(size_t) selGroup]->tracks[(size_t) selTrack].get();
        AudioClip c = tr->clips[(size_t) selClip];
        const double srcSeconds = c.duration / juce::jmax (0.01, c.stretchRatio);
        const double ratio = juce::jlimit (0.25, 4.0, targetLen / juce::jmax (0.05, srcSeconds));
        titleLabel.setText ("Time-stretching...", juce::dontSendNotification);
        auto baked = audioEngine.makeStretchedClip (tr->engineId, c.sourceIn, srcSeconds, ratio);
        if (baked == nullptr) { titleLabel.setText ("Time-stretch failed", juce::dontSendNotification); return; }
        pushUndo();
        c.stretched = baked; c.stretchRatio = ratio; c.speedFadeIn = 0.0; c.speedFadeOut = 0.0;
        c.bakedSrcSeconds = srcSeconds; c.duration = srcSeconds * ratio;
        clipChanged (selGroup, selTrack, selClip, c);
        updateTimelineSize();
        titleLabel.setText ("Stretched to " + juce::String (c.duration, 2) + " s (" + juce::String ((int) (ratio * 100.0)) + "%)", juce::dontSendNotification);
    }
    void resetClipStretch()
    {
        if (! validClip (selGroup, selTrack, selClip)) return;
        AudioClip c = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->clips[(size_t) selClip];
        if (c.stretchRatio == 1.0 && c.stretched == nullptr) return;
        const double srcSeconds = c.duration / juce::jmax (0.01, c.stretchRatio);
        pushUndo();
        c.stretched.reset(); c.stretchRatio = 1.0; c.speedFadeIn = 0.0; c.speedFadeOut = 0.0; c.bakedSrcSeconds = 0.0; c.duration = srcSeconds;
        clipChanged (selGroup, selTrack, selClip, c);
        updateTimelineSize();
        titleLabel.setText ("Stretch / speed reset", juce::dontSendNotification);
    }

    // Tape-style speed fade: spin-up at the head and/or slow-down at the tail (pitch + tempo ramp).
    void applySpeedFade (bool head, bool tail)
    {
        if (! validClip (selGroup, selTrack, selClip)) { titleLabel.setText ("Select a clip for a speed fade", juce::dontSendNotification); return; }
        auto* tr = groups[(size_t) selGroup]->tracks[(size_t) selTrack].get();
        AudioClip c = tr->clips[(size_t) selClip];
        const double srcSeconds = c.duration / juce::jmax (0.01, c.stretchRatio);   // back to the source length first
        const double useIn  = head ? 0.75 : c.speedFadeIn;
        const double useOut = tail ? 0.75 : c.speedFadeOut;
        auto baked = audioEngine.makeSpeedFaded (tr->engineId, c.sourceIn, srcSeconds, useIn, useOut);
        if (baked == nullptr) { titleLabel.setText ("Speed fade failed", juce::dontSendNotification); return; }
        pushUndo();
        c.stretched = baked; c.stretchRatio = 1.0; c.speedFadeIn = useIn; c.speedFadeOut = useOut;
        c.bakedSrcSeconds = srcSeconds; c.duration = baked->getNumSamples() / audioEngine.sampleRate();
        clipChanged (selGroup, selTrack, selClip, c);
        updateTimelineSize();
        titleLabel.setText (juce::String (head ? "Speed fade-in (spin up) " : "") + (tail ? "speed fade-out (slow down)" : ""), juce::dontSendNotification);
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
                                        LSCmd::ToggleSnap, LSCmd::NudgeLeft, LSCmd::NudgeRight, LSCmd::Undo, LSCmd::Redo,
                                        LSCmd::AddMarker, LSCmd::PrevMarker, LSCmd::NextMarker, LSCmd::ZoomFit, LSCmd::GoToTimecode,
                                        LSCmd::CopyClip, LSCmd::PasteClip, LSCmd::DuplicateClip, LSCmd::ToggleAutomation };
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
            case LSCmd::AddMarker:  info.setInfo ("Add marker", "Add a marker at the playhead", "Markers", 0); break;
            case LSCmd::PrevMarker: info.setInfo ("Previous marker", "Move playhead to the previous marker", "Markers", 0); break;
            case LSCmd::NextMarker: info.setInfo ("Next marker", "Move playhead to the next marker", "Markers", 0); break;
            case LSCmd::ZoomFit:    info.setInfo ("Zoom to fit", "Fit the whole timeline to the window", "View", 0); break;
            case LSCmd::GoToTimecode: info.setInfo ("Go to position...", "Move the playhead to a typed position", "Transport", 0); break;
            case LSCmd::CopyClip:   info.setInfo ("Copy clip", "Copy the selected clip", "Edit", 0); break;
            case LSCmd::PasteClip:  info.setInfo ("Paste clip", "Paste the copied clip at the playhead", "Edit", 0); break;
            case LSCmd::DuplicateClip: info.setInfo ("Duplicate clip", "Duplicate the selected clip", "Edit", 0); break;
            case LSCmd::ToggleAutomation: info.setInfo ("Show automation", "Show/edit volume automation envelopes", "View", 0); info.setTicked (automationVisible); break;
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
            case LSCmd::AddMarker:  addMarkerAtPlayhead(); return true;
            case LSCmd::PrevMarker: gotoAdjacentMarker (-1); return true;
            case LSCmd::NextMarker: gotoAdjacentMarker (1);  return true;
            case LSCmd::ZoomFit:    timeline.zoomToFit(); return true;
            case LSCmd::GoToTimecode: goToTimecode(); return true;
            case LSCmd::CopyClip:      copySelectedClip(); return true;
            case LSCmd::PasteClip:     pasteClipAtPlayhead(); return true;
            case LSCmd::DuplicateClip: duplicateSelectedClip(); return true;
            case LSCmd::ToggleAutomation: toggleAutomation(); return true;
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

    //== Volume automation ==
    void pushTrackAutomation (int g, int t)   // model envelope -> engine
    {
        if (! validTrack (g, t)) return;
        auto* tr = groups[(size_t) g]->tracks[(size_t) t].get();
        std::vector<std::pair<double, float>> env;
        env.reserve (tr->volumeAuto.size());
        for (auto& p : tr->volumeAuto) env.push_back ({ p.time, p.value });
        audioEngine.setTrackAutomation (tr->engineId, env, tr->automationOn);
    }
    void toggleAutomation()
    {
        automationVisible = ! automationVisible;
        timeline.setAutomationMode (automationVisible);
        if (validGroup (activeGroup))
            for (int t = 0; t < (int) groups[(size_t) activeGroup]->tracks.size(); ++t)
            { groups[(size_t) activeGroup]->tracks[(size_t) t]->automationOn = automationVisible; pushTrackAutomation (activeGroup, t); }
        titleLabel.setText (automationVisible ? "Automation on - drag on a track to draw volume; double-click a point to delete"
                                              : "Automation off", juce::dontSendNotification);
        timeline.repaint();
    }

    //== Markers ==
    void addMarkerAtPlayhead()
    {
        if (! validGroup (activeGroup)) { titleLabel.setText ("Open a video to add markers", juce::dontSendNotification); return; }
        auto& mks = groups[(size_t) activeGroup]->markers;
        pushUndo();
        mks.push_back ({ playhead, "Marker " + juce::String ((int) mks.size() + 1) });
        std::sort (mks.begin(), mks.end(), [] (const Marker& a, const Marker& b) { return a.time < b.time; });
        timeline.repaint();
        titleLabel.setText ("Marker added at " + formatTimecode (playhead), juce::dontSendNotification);
    }
    void gotoAdjacentMarker (int dir)
    {
        if (! validGroup (activeGroup)) return;
        double best = -1.0;
        for (auto& m : groups[(size_t) activeGroup]->markers)
        {
            if (dir > 0 && m.time > playhead + 1.0e-3 && (best < 0.0 || m.time < best)) best = m.time;
            if (dir < 0 && m.time < playhead - 1.0e-3 && (best < 0.0 || m.time > best)) best = m.time;
        }
        if (best >= 0.0) { playhead = best; seekAll (best); timeline.setPlayhead (best); timeline.repaint(); }
    }
    void renameMarker (int idx)
    {
        if (! validGroup (activeGroup)) return;
        auto& mks = groups[(size_t) activeGroup]->markers;
        if (idx < 0 || idx >= (int) mks.size()) return;
        auto* w = new juce::AlertWindow ("Marker", "Marker name:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("nm", mks[(size_t) idx].name);
        w->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Delete", 2);
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, idx] (int r)
        {
            std::unique_ptr<juce::AlertWindow> own (w);
            if (validGroup (activeGroup))
            {
                auto& m2 = groups[(size_t) activeGroup]->markers;
                if (idx < (int) m2.size())
                {
                    if      (r == 1) { pushUndo(); m2[(size_t) idx].name = w->getTextEditorContents ("nm"); }
                    else if (r == 2) { pushUndo(); m2.erase (m2.begin() + idx); }
                }
            }
            timeline.repaint();
            restoreKeyFocus();
        }), false);
    }

    //== Go to a typed position; Copy / Paste / Duplicate clips ==
    void goToTimecode()
    {
        auto* w = new juce::AlertWindow ("Go to position", "HH:MM:SS:FF, M:SS, or seconds:", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("pos", formatTimecode (playhead));
        w->addButton ("Go",     1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
        {
            std::unique_ptr<juce::AlertWindow> own (w);
            if (r == 1) { const double s = parsePosition (w->getTextEditorContents ("pos"));
                          if (s >= 0.0) { playhead = s; seekAll (s); timeline.setPlayhead (s); timeline.repaint(); } }
            restoreKeyFocus();
        }), false);
    }
    void copySelectedClip()
    {
        if (! validClip (selGroup, selTrack, selClip)) { titleLabel.setText ("Select a clip to copy", juce::dontSendNotification); return; }
        clipboardClip = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->clips[(size_t) selClip];
        hasClipboard = true;
        titleLabel.setText ("Clip copied", juce::dontSendNotification);
    }
    void pasteClipAtPlayhead()
    {
        if (! hasClipboard) { titleLabel.setText ("Nothing to paste", juce::dontSendNotification); return; }
        if (! validTrack (selGroup, selTrack)) { titleLabel.setText ("Select a track to paste into", juce::dontSendNotification); return; }
        pushUndo();
        AudioClip nc = clipboardClip;
        nc.timelineStart = juce::jmax (0.0, playhead);
        auto& cl = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->clips;
        cl.push_back (nc);
        selClip = (int) cl.size() - 1;
        pushActiveClips (selGroup);
        updateTimelineSize();
        timeline.setSelection (selGroup, selTrack, selClip);
        timeline.repaint();
    }
    void duplicateSelectedClip()
    {
        if (! validClip (selGroup, selTrack, selClip)) { titleLabel.setText ("Select a clip to duplicate", juce::dontSendNotification); return; }
        pushUndo();
        auto& cl = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->clips;
        AudioClip nc = cl[(size_t) selClip];
        nc.timelineStart = nc.timelineEnd();          // place the copy right after the original
        cl.push_back (nc);
        selClip = (int) cl.size() - 1;
        pushActiveClips (selGroup);
        updateTimelineSize();
        timeline.setSelection (selGroup, selTrack, selClip);
        timeline.repaint();
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
        add (LSCmd::AddMarker,     "M");
        add (LSCmd::CopyClip,      "command + C");
        add (LSCmd::PasteClip,     "command + V");
        add (LSCmd::DuplicateClip, "command + D");
        add (LSCmd::ZoomFit,       "Z");
        add (LSCmd::GoToTimecode,  "command + G");
        add (LSCmd::ToggleAutomation, "A");
        switch (p)
        {
            case KeyProfile::Logic:    add (LSCmd::Split, "command + T"); add (LSCmd::ToggleLoop, "C");                   add (LSCmd::ToggleSnap, "command + shift + S"); break;
            case KeyProfile::ProTools: add (LSCmd::Split, "command + E"); add (LSCmd::ToggleLoop, "command + shift + L"); add (LSCmd::ToggleSnap, "command + shift + S"); break;
            case KeyProfile::Ableton:  add (LSCmd::Split, "command + E"); add (LSCmd::ToggleLoop, "command + L");         add (LSCmd::ToggleSnap, "command + shift + S"); break;
            default:                   add (LSCmd::Split, "S");           add (LSCmd::ToggleLoop, "L");                   add (LSCmd::ToggleSnap, "N"); break;
        }
        keysButton.setButtonText ("Keys: " + profileName (p));
        applySkinForProfile (p);   // switching the station also reskins the app to that DAW

        if (p == KeyProfile::ProTools)   // sync the timeline to the PT tool/mode palette
        {
            using ET = TimelineComponent::EditTool;
            const ET tools[] = { ET::Zoom, ET::Trim, ET::Grab, ET::Scrub, ET::Smart };
            timeline.setEditTool (tools[juce::jlimit (0, 4, ptBar.selTool)]);
            ptEditMode = ptBar.selMode;
            const bool grid = (ptBar.selMode == 3);
            snapToggle.setToggleState (grid, juce::dontSendNotification);
            timeline.setSnapEnabled (grid);
            timeline.setShuffle (ptBar.selMode == 0);
        }
        else                              // other skins: normal smart editing, snap per the toggle
        {
            timeline.setEditTool (TimelineComponent::EditTool::Smart);
            timeline.setShuffle (false);
            timeline.setSnapEnabled (snapToggle.getToggleState());
        }

        menuItemsChanged();        // rebuild the menu bar to that DAW's menu set
        grabKeyboardFocus();
        saveSettings();            // remember this skin for next launch
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
        ptBar.setSkin (s);
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
                             m.addItem (9012, "Insert Compressor on Selected Track", hasTrack);
                             m.addItem (9015, "Split Selected Track into Stems (6: voc/drm/bass/gtr/pno/oth)", hasTrack && ! splitting);
                             m.addItem (9016, "Split Selected Track into Stems (4: voc/drm/bass/oth)", hasTrack && ! splitting);
                             m.addItem (9017, StemSplitter::isInstalled() ? "Reinstall Stem Splitter..." : "Set Up Stem Splitter (one-time)...", ! installing); };
        auto pluginTools = [&] { m.addItem (9014, "Plugins Window...");
                                 m.addItem (9013, scanning ? "Scanning Plugins..." : "Rescan Plugins", ! scanning); };
        auto markersMenu = [&] {
            juce::PopupMenu mm;
            mm.addCommandItem (&commandManager, LSCmd::AddMarker);
            mm.addCommandItem (&commandManager, LSCmd::PrevMarker);
            mm.addCommandItem (&commandManager, LSCmd::NextMarker);
            if (validGroup (activeGroup) && ! groups[(size_t) activeGroup]->markers.empty())
            {
                mm.addSeparator();
                juce::PopupMenu go;
                const auto& mks = groups[(size_t) activeGroup]->markers;
                for (int i = 0; i < (int) mks.size() && i < 90; ++i)
                    go.addItem (9100 + i, mks[(size_t) i].name + "  (" + formatTimecode (mks[(size_t) i].time) + ")");
                mm.addSubMenu ("Go to Marker", go);
            }
            m.addSubMenu ("Markers", mm);
        };
        auto clipEdits = [&] { m.addCommandItem (&commandManager, LSCmd::CopyClip);
                               m.addCommandItem (&commandManager, LSCmd::PasteClip);
                               m.addCommandItem (&commandManager, LSCmd::DuplicateClip); };
        auto fxBusMenu = [&] {
            juce::PopupMenu bus, add;
            const char* fx[] = { "EQ", "Compressor", "Reverb", "Delay", "Limiter", "Gate" };
            for (int i = 0; i < 6; ++i) add.addItem (9200 + i, fx[i]);
            bus.addSubMenu ("Add to FX Bus", add);
            const int n = audioEngine.auxPluginCount();
            if (n > 0)
            {
                bus.addSeparator();
                for (int i = 0; i < n && i < 16; ++i)
                {
                    juce::PopupMenu one; one.addItem (9220 + i * 2, "Open editor"); one.addItem (9220 + i * 2 + 1, "Remove");
                    bus.addSubMenu (audioEngine.auxPluginName (i), one);
                }
            }
            m.addSubMenu ("FX Bus (sends)", bus);
        };

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
            m.addSeparator();
            m.addItem (9007, "Relink Missing Media...");
        }
        else if (name == "Edit")
        {
            m.addCommandItem (&commandManager, LSCmd::Undo);
            m.addCommandItem (&commandManager, LSCmd::Redo);
            m.addSeparator();
            m.addCommandItem (&commandManager, LSCmd::Split);
            m.addCommandItem (&commandManager, LSCmd::DeleteClip);
            clipEdits();
            m.addSeparator();
            const bool hasClip = validClip (selGroup, selTrack, selClip);
            const char* shapes[] = { "Linear", "Exponential", "S-Curve (Bell)", "Logarithmic" };
            juce::PopupMenu fin, fout;
            for (int i = 0; i < 4; ++i) { fin.addItem (9070 + i, shapes[i], hasClip); fout.addItem (9074 + i, shapes[i], hasClip); }
            m.addSubMenu ("Fade In", fin, hasClip);
            m.addSubMenu ("Fade Out", fout, hasClip);
            m.addItem (9078, "Remove Fades", hasClip);
        }
        else if (name == "Track")
        {
            m.addItem (9010, "Import Audio Track...", activeGroup >= 0);
            m.addSeparator();
            fxItems();
            m.addSeparator();
            fxBusMenu();
            pluginTools();
        }
        else if (name == "Create")          // Ableton
        {
            m.addItem (9004, "Add Video...");
            m.addItem (9010, "Import Audio Track...", activeGroup >= 0);
            m.addSeparator();
            fxItems();
            m.addSeparator();
            fxBusMenu();
            pluginTools();
        }
        else if (name == "Mix" || name == "AudioSuite")   // Logic / Pro Tools
        {
            fxItems();
            m.addSeparator();
            fxBusMenu();
            pluginTools();
        }
        else if (name == "Transport" || name == "Navigate")
        {
            m.addCommandItem (&commandManager, LSCmd::TogglePlay);
            if (name == "Navigate") m.addItem (9030, "Go to Start");
            m.addCommandItem (&commandManager, LSCmd::GoToTimecode);
            m.addSeparator();
            m.addCommandItem (&commandManager, LSCmd::ToggleLoop);
            m.addCommandItem (&commandManager, LSCmd::NudgeLeft);
            m.addCommandItem (&commandManager, LSCmd::NudgeRight);
            m.addSeparator();
            markersMenu();
        }
        else if (name == "Clip")            // Pro Tools
        {
            m.addCommandItem (&commandManager, LSCmd::Split);
            m.addCommandItem (&commandManager, LSCmd::DeleteClip);
            clipEdits();
        }
        else if (name == "Event")           // Pro Tools
        {
            m.addCommandItem (&commandManager, LSCmd::NudgeLeft);
            m.addCommandItem (&commandManager, LSCmd::NudgeRight);
            m.addSeparator();
            m.addCommandItem (&commandManager, LSCmd::ToggleSnap);
            m.addSeparator();
            markersMenu();
        }
        else if (name == "Record")          // Logic (recording is Phase C)
        {
            m.addItem (9099, "Record  (coming soon)", false);
        }
        else if (name == "View")
        {
            m.addItem (9060, videoWindowOpen ? "Hide Video Window" : "Show Video Window");
            m.addItem (9050, mixerVisible ? "Hide Mixer" : "Show Mixer");
            m.addCommandItem (&commandManager, LSCmd::ZoomFit);
            m.addCommandItem (&commandManager, LSCmd::ToggleAutomation);
            m.addItem (9062, "Automation: Latch (write fader rides)", automationVisible, autoMode == 1);
            m.addSeparator();
            m.addCommandItem (&commandManager, LSCmd::ToggleSnap);
            m.addItem (9061, "Snap to Frames", true, frameSnapOn);
            m.addCommandItem (&commandManager, LSCmd::ToggleLoop);
            markersMenu();
            m.addSeparator();
            addSkin (m);
        }
        else if (name == "Options" || name == "Setup")    // Ableton / Pro Tools
        {
            m.addItem (9060, videoWindowOpen ? "Hide Video Window" : "Show Video Window");
            m.addCommandItem (&commandManager, LSCmd::ToggleSnap);
            m.addSeparator();
            pluginTools();
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
            m.addItem (9098, "Layback Station Help");
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
            case 9007: relinkMissingMedia(); break;
            case 9010: if (activeGroup >= 0) importTrack (activeGroup); break;
            case 9011: insertEffectAndEdit (0); break;
            case 9012: insertEffectAndEdit (1); break;
            case 9014: openPluginListWindow(); break;
            case 9200: case 9201: case 9202: case 9203: case 9204: case 9205:
                audioEngine.addAuxEffect (menuItemID - 9200); openAuxEditor (audioEngine.auxPluginCount() - 1); break;
            case 9015: splitTrackIntoStems (selGroup, selTrack, true);  break;
            case 9016: splitTrackIntoStems (selGroup, selTrack, false); break;
            case 9017: installStemSplitter ([] (bool) {}); break;
            case 9013: startPluginScan(); break;
            case 9020: applyKeyProfile (KeyProfile::Layback);  break;
            case 9021: applyKeyProfile (KeyProfile::Logic);    break;
            case 9022: applyKeyProfile (KeyProfile::ProTools); break;
            case 9023: applyKeyProfile (KeyProfile::Ableton);  break;
            case 9030: seekAll (0.0); break;
            case 9040: closeAllPluginWindows(); break;
            case 9070: applyFade (0, 0); break;  case 9071: applyFade (0, 1); break;
            case 9072: applyFade (0, 2); break;  case 9073: applyFade (0, 3); break;
            case 9074: applyFade (1, 0); break;  case 9075: applyFade (1, 1); break;
            case 9076: applyFade (1, 2); break;  case 9077: applyFade (1, 3); break;
            case 9078: applyFade (2); break;
            case 9098: openHelpWindow(); break;
            case 9060: showVideoWindow (! videoWindowOpen); break;
            case 9062: autoMode = (autoMode == 1 ? 0 : 1);
                       titleLabel.setText (autoMode == 1 ? "Automation: Latch - move a fader while playing to write the ride"
                                                         : "Automation: Read", juce::dontSendNotification); break;
            case 9061: frameSnapOn = ! frameSnapOn; timeline.setFrameSnap (frameSnapOn);
                       titleLabel.setText (frameSnapOn ? "Snap to frames: on" : "Snap to frames: off", juce::dontSendNotification); break;
            case 9050: toggleMixer(); break;
            default:
                if (menuItemID >= 9100 && menuItemID < 9200 && validGroup (activeGroup))   // Go to Marker N
                {
                    const int i = menuItemID - 9100;
                    auto& mks = groups[(size_t) activeGroup]->markers;
                    if (i < (int) mks.size()) { playhead = mks[(size_t) i].time; seekAll (playhead); timeline.setPlayhead (playhead); timeline.repaint(); }
                }
                else if (menuItemID >= 9220 && menuItemID < 9260)   // FX Bus: open editor / remove
                {
                    const int idx = (menuItemID - 9220) / 2;
                    if (((menuItemID - 9220) % 2) == 0) openAuxEditor (idx);
                    else { closePluginWindowForProc (audioEngine.auxPlugin (idx)); audioEngine.removeAuxPlugin (idx); }
                }
                break;
        }
    }

    //== global app settings (remember the chosen skin across launches) ==
    juce::File settingsFile() const
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Layback").getChildFile ("settings.json");
    }
    void saveSettings()
    {
        const auto f = settingsFile();
        f.getParentDirectory().createDirectory();
        auto* o = new juce::DynamicObject();
        o->setProperty ("skin", (int) keyProfile);
        f.replaceWithText (juce::JSON::toString (juce::var (o)));
    }
    KeyProfile loadSavedSkin()   // defaults to Logic if no setting yet
    {
        const auto v = juce::JSON::parse (settingsFile().loadFileAsString());
        const int s = v.isObject() ? (int) v.getProperty ("skin", (int) KeyProfile::Logic) : (int) KeyProfile::Logic;
        return (KeyProfile) juce::jlimit (0, 3, s);
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

    void openAuxEditor (int index)   // editor for an FX-bus effect
    {
        auto* proc = audioEngine.auxPlugin (index);
        if (proc == nullptr) return;
        for (auto& w : pluginWindows) if (w->proc == proc) { w->toFront (true); return; }
        auto win = std::make_unique<PluginWindow> (proc, laf.skin.panel);
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

    // The Plugins window: scan/browse available AU & VST3 plugins.
    void openPluginListWindow()
    {
        if (pluginListWindow != nullptr) { pluginListWindow->toFront (true); return; }
        const juce::File dead = pluginListFile().getSiblingFile ("scan_crashlog.txt");
        pluginListWindow = std::make_unique<PluginListWindow> (audioEngine.getPluginFormats(),
                                                               audioEngine.getKnownPlugins(), dead, laf.skin.panel);
        auto a = alive;
        pluginListWindow->onClose = [this, a]
        {
            if (! a->load()) return;
            persistPluginList();
            pluginsScanned = audioEngine.getKnownPlugins().getNumTypes() > 0;
            pluginListWindow.reset();
            restoreKeyFocus();
        };
    }

    void openHelpWindow()
    {
        if (helpWindow != nullptr) { helpWindow->toFront (true); return; }
        helpWindow = std::make_unique<InfoWindow> ("Layback Station - Help & Shortcuts", helpText(), laf.skin.panel, laf.skin.text);
        auto a = alive;
        helpWindow->onClose = [this, a] { if (a->load()) { helpWindow.reset(); restoreKeyFocus(); } };
    }
    static juce::String helpText()
    {
        return juce::String::fromUTF8 (
            "LAYBACK STATION - Help & Shortcuts\n"
            "===================================\n\n"
            "GETTING STARTED\n"
            "  - Drag a video from Finder into the window (or File > Add Video).\n"
            "  - Drag songs onto it (or + Import Track) to audition against picture.\n"
            "  - The floating Video window follows the playhead as you scrub.\n\n"
            "TRANSPORT\n"
            "  Space         Play / Pause\n"
            "  Cmd+G         Go to position (HH:MM:SS:FF / M:SS / seconds)\n"
            "  Z             Zoom to fit       Cmd+scroll  Zoom in / out\n\n"
            "CLIP EDITING  (right-click a clip for the full menu)\n"
            "  Split, Delete, Crossfade with previous, Normalize\n"
            "  Fades: in/out with curve shapes (drag the clip's top corners)\n"
            "  Clip gain: drag the horizontal line across the clip\n"
            "  Time-Stretch... : fit a clip to a length (pitch preserved)\n"
            "  Speed Fade In/Out : tape-style spin-up / slow-down\n"
            "  Cmd+C / Cmd+V / Cmd+D : copy / paste / duplicate\n"
            "  Left/Right arrows : nudge the selected clip\n\n"
            "MARKERS\n"
            "  M  add marker at playhead   (click a flag to jump; double-click to rename)\n\n"
            "MIX & FX\n"
            "  Mixer: fader, pan, mute, solo, meters, inserts, master\n"
            "  Inserts: EQ, Compressor, Reverb, Delay, Limiter, Gate + AU/VST3\n"
            "  A  show / edit volume automation (click to add points, double-click to delete)\n\n"
            "STEMS\n"
            "  Right-click a track > Split into Stems (Demucs)\n\n"
            "SKINS\n"
            "  Keys menu: Layback / Logic / Pro Tools / Ableton (remembered next launch)\n\n"
            "PROJECT\n"
            "  Cmd+Z / Cmd+Shift+Z undo / redo. Autosaves every minute (recovers on crash).\n"
            "  Export: video + audio (muxed), or audio-only WAV.\n");
    }

    // Insert a native effect (0 = EQ, 1 = Compressor) on the selected track and open its editor.
    void insertEffectAndEdit (int which)
    {
        if (! validTrack (selGroup, selTrack)) { titleLabel.setText ("Select a track first", juce::dontSendNotification); return; }
        const int engineId = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->engineId;
        audioEngine.addNativeEffect (engineId, which);
        openPluginEditor (engineId, audioEngine.trackPluginCount (engineId) - 1);
        refreshMixer();
    }

    // Transport Record: arm the selected track (audio capture itself is a later phase).
    void toggleSelectedRecordArm()
    {
        if (! validTrack (selGroup, selTrack))
        { titleLabel.setText ("Select an audio track to record-arm", juce::dontSendNotification); return; }
        auto* tr = groups[(size_t) selGroup]->tracks[(size_t) selTrack].get();
        tr->recordArm = ! tr->recordArm;
        titleLabel.setText (tr->recordArm ? "Track armed for record (capture in a later build)"
                                          : "Track record-disarmed", juce::dontSendNotification);
        timeline.repaint();
        if (mixerVisible) mixerView.syncFromModel();
    }

    //== Pro Tools edit modes: Shuffle(0) / Slip(1) / Spot(2) / Grid(3) ==
    void applyPtEditMode (int mode)
    {
        ptEditMode = mode;
        const bool grid = (mode == 3);
        snapToggle.setToggleState (grid, juce::dontSendNotification);
        timeline.setSnapEnabled (grid);          // Grid snaps; Slip/Shuffle/Spot don't grid-snap
        timeline.setShuffle (mode == 0);         // Shuffle = butt clips against neighbours
        if (mode == 2) spotSelectedClip();       // Spot = type an exact location for the selected clip
    }

    // Move the selected clip to a typed position (HH:MM:SS:FF, M:SS, or plain seconds).
    void spotSelectedClip()
    {
        if (! validClip (selGroup, selTrack, selClip))
        { titleLabel.setText ("Select a clip to spot to a location", juce::dontSendNotification); return; }
        const double cur = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->clips[(size_t) selClip].timelineStart;
        auto* w = new juce::AlertWindow ("Spot to location",
                       "Move the selected clip to (HH:MM:SS:FF, M:SS, or seconds):", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("pos", formatTimecode (cur));
        w->addButton ("Spot",   1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int r)
        {
            std::unique_ptr<juce::AlertWindow> own (w);
            if (r == 1 && validClip (selGroup, selTrack, selClip))
            {
                const double secs = parsePosition (w->getTextEditorContents ("pos"));
                if (secs >= 0.0)
                {
                    pushUndo();
                    AudioClip nc = groups[(size_t) selGroup]->tracks[(size_t) selTrack]->clips[(size_t) selClip];
                    nc.timelineStart = secs;
                    clipChanged (selGroup, selTrack, selClip, nc);
                    titleLabel.setText ("Spotted to " + formatTimecode (secs), juce::dontSendNotification);
                }
            }
            restoreKeyFocus();
        }), false);
    }

    static juce::String formatTimecode (double secs)   // HH:MM:SS:FF @30fps
    {
        if (secs < 0.0) secs = 0.0;
        const int h = (int) (secs / 3600.0), m = ((int) (secs / 60.0)) % 60, s = ((int) secs) % 60, f = ((int) (secs * 30.0)) % 30;
        return juce::String::formatted ("%02d:%02d:%02d:%02d", h, m, s, f);
    }
    static double parsePosition (const juce::String& in)
    {
        const auto str = in.trim();
        if (str.isEmpty()) return -1.0;
        if (str.containsChar (':'))
        {
            juce::StringArray p; p.addTokens (str, ":", "");
            double h = 0, m = 0, s = 0, f = 0; const int n = p.size();
            if      (n == 2) { m = p[0].getDoubleValue(); s = p[1].getDoubleValue(); }
            else if (n == 3) { m = p[0].getDoubleValue(); s = p[1].getDoubleValue(); f = p[2].getDoubleValue(); }
            else if (n >= 4) { h = p[0].getDoubleValue(); m = p[1].getDoubleValue(); s = p[2].getDoubleValue(); f = p[3].getDoubleValue(); }
            return h * 3600.0 + m * 60.0 + s + f / 30.0;
        }
        return str.getDoubleValue();
    }

    //== Logic local-menu row (Edit / Functions / View) — real pop-up menus ==
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (laf.skin.layout == 1 && ! logicToolbar.isEmpty())
        {
            if (logicMEdit.contains (e.getPosition())) { showLocalEditMenu();      return; }
            if (logicMFunc.contains (e.getPosition())) { showLocalFunctionsMenu(); return; }
            if (logicMView.contains (e.getPosition())) { showLocalViewMenu();      return; }
        }
    }

    void showLocalEditMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "Undo"); m.addItem (2, "Redo"); m.addSeparator();
        m.addItem (3, "Split at Playhead"); m.addItem (4, "Delete Clip");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (logicMEdit)), [this] (int r)
        {
            if      (r == 1) commandManager.invokeDirectly (LSCmd::Undo, false);
            else if (r == 2) commandManager.invokeDirectly (LSCmd::Redo, false);
            else if (r == 3) commandManager.invokeDirectly (LSCmd::Split, false);
            else if (r == 4) commandManager.invokeDirectly (LSCmd::DeleteClip, false);
            restoreKeyFocus();
        });
    }

    void showLocalFunctionsMenu()
    {
        const bool ht = validTrack (selGroup, selTrack);
        juce::PopupMenu m;
        m.addItem (1, "Insert EQ on Selected Track", ht);
        m.addItem (2, "Insert Compressor on Selected Track", ht);
        m.addSeparator();
        m.addItem (3, "Plugins Window...");
        m.addItem (4, scanning ? "Scanning Plugins..." : "Rescan Plugins", ! scanning);
        m.addSeparator();
        m.addItem (5, "Split Track into Stems (6)", ht && ! splitting);
        m.addItem (6, "Split Track into Stems (4)", ht && ! splitting);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (logicMFunc)), [this] (int r)
        {
            if      (r == 1) insertEffectAndEdit (0);
            else if (r == 2) insertEffectAndEdit (1);
            else if (r == 3) openPluginListWindow();
            else if (r == 4) startPluginScan();
            else if (r == 5) splitTrackIntoStems (selGroup, selTrack, true);
            else if (r == 6) splitTrackIntoStems (selGroup, selTrack, false);
            restoreKeyFocus();
        });
    }

    void showLocalViewMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, mixerVisible ? "Hide Mixer" : "Show Mixer");
        m.addItem (2, videoWindowOpen ? "Hide Video Window" : "Show Video Window");
        m.addSeparator();
        m.addItem (3, "Snap", true, snapToggle.getToggleState());
        m.addItem (4, "Loop / Cycle", true, loopEnabled);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (logicMView)), [this] (int r)
        {
            if      (r == 1) toggleMixer();
            else if (r == 2) showVideoWindow (! videoWindowOpen);
            else if (r == 3) { snapToggle.setToggleState (! snapToggle.getToggleState(), juce::dontSendNotification); timeline.setSnapEnabled (snapToggle.getToggleState()); }
            else if (r == 4) toggleLoop();
            restoreKeyFocus();
        });
    }

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
        insert.addItem (2003, "Reverb (Layback)");
        insert.addItem (2004, "Delay (Layback)");
        insert.addItem (2005, "Limiter (Layback)");
        insert.addItem (2006, "Gate (Layback)");
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
        m.addItem (7, "Split into Stems - 6 (vocals/drums/bass/guitar/piano/other)", ! splitting);
        m.addItem (8, "Split into Stems - 4 (vocals/drums/bass/other)", ! splitting);
        if (splitting) m.addItem (12, "Cancel Stem Split");
        m.addSeparator();
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
        m.addItem (6, "Plugins window...");
        m.addItem (5, scanning ? "Scanning plugins..." : "Rescan plugins", ! scanning);
        m.addItem (1, "Delete track");

        m.showMenuAsync (juce::PopupMenu::Options(), [this, g, t, engineId] (int r)
        {
            if      (r == 1)    deleteTrack (g, t);
            else if (r == 7)    splitTrackIntoStems (g, t, true);
            else if (r == 8)    splitTrackIntoStems (g, t, false);
            else if (r == 12)   cancelStemSplit();
            else if (r == 5)    startPluginScan();
            else if (r == 6)    openPluginListWindow();
            else if (r >= 2001 && r <= 2006) { audioEngine.addNativeEffect (engineId, r - 2001); openPluginEditor (engineId, audioEngine.trackPluginCount (engineId) - 1); }
            else if (r >= 3000 && r < 4000)
            {
                const auto it = pluginMenuMap.find (r);
                if (it != pluginMenuMap.end())
                {
                    titleLabel.setText ("Loading plugin...", juce::dontSendNotification);
                    auto a = alive;
                    audioEngine.addHostedPluginAsync (engineId, it->second, [this, a, engineId] (bool ok, juce::String err)
                    {
                        if (! a->load()) return;                                 // instantiated off-thread: a slow plugin can't freeze the UI
                        if (ok) { openPluginEditor (engineId, audioEngine.trackPluginCount (engineId) - 1); refreshMixer(); }
                        else    { titleLabel.setText ("Plugin failed to load: " + err, juce::dontSendNotification); }
                    });
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
                ts.volume = t->volume; ts.pan = t->pan; ts.send = t->send; ts.mixGroup = t->mixGroup;
                ts.automationOn = t->automationOn; ts.volumeAuto = t->volumeAuto;
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
                auto& tr = *g->tracks[ti]; const auto& ts = gs.tracks[ti];
                tr.mute = ts.mute; tr.solo = ts.solo; tr.clips = ts.clips;
                tr.volume = ts.volume; tr.pan = ts.pan; tr.send = ts.send;
                tr.mixGroup = ts.mixGroup; tr.automationOn = ts.automationOn; tr.volumeAuto = ts.volumeAuto;
                if (tr.engineId >= 0)   // push the restored mix state to the engine
                {
                    audioEngine.setTrackPan (tr.engineId, tr.pan);
                    audioEngine.setTrackSend (tr.engineId, tr.send);
                    std::vector<std::pair<double, float>> env; for (auto& p : tr.volumeAuto) env.push_back ({ p.time, p.value });
                    audioEngine.setTrackAutomation (tr.engineId, env, tr.automationOn);
                }
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
        if (mixerVisible) mixerView.syncFromModel();
        refreshInspector();
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
    using CopyJob = std::pair<juce::File, juce::File>;   // (source, destination)

    juce::String copyMedia (const juce::File& src, const juce::File& mediaDir,
                            std::map<juce::String, juce::String>& seen, bool& allOk, bool doCopy = true,
                            std::vector<CopyJob>* queue = nullptr)
    {
        if (! src.existsAsFile()) { allOk = false; return src.getFullPathName(); }
        if (! doCopy) return src.getFullPathName();   // autosave/recovery: reference originals (no copy)
        const juce::String key = src.getFullPathName();
        if (auto it = seen.find (key); it != seen.end()) return it->second;   // same source -> reuse the same media file

        // unique destination per distinct source (hash of the full path) so same-named files never collide
        const juce::String destName = src.getFileNameWithoutExtension() + "_"
            + juce::String::toHexString ((juce::int64) (key.hashCode64() & 0xffffffLL)) + src.getFileExtension();
        const juce::File dest = mediaDir.getChildFile (destName);
        const juce::String result = "media/" + destName;

        if (queue != nullptr)            // defer the actual file copy to a worker thread (non-freezing save)
        {
            queue->push_back ({ src, dest });
        }
        else                             // synchronous copy
        {
            bool copied = (dest.existsAsFile() && dest.getSize() == src.getSize()) || src.copyFileTo (dest);
            if (! copied) { allOk = false; seen[key] = key; return key; }
        }
        seen[key] = result;
        return result;
    }

    // Serialize the whole project to a var. doCopy=true copies media into mediaDir (for .lbproj);
    // doCopy=false references the original file paths (for autosave/recovery). If queue is given,
    // media copies are deferred into it (the caller copies on a worker thread).
    juce::var buildProjectVar (const juce::File& mediaDir, bool doCopy, bool& allOk, std::vector<CopyJob>* queue = nullptr)
    {
        std::map<juce::String, juce::String> seen;
        auto* root = new juce::DynamicObject();
        root->setProperty ("version", 1);
        root->setProperty ("activeGroup", activeGroup);
        root->setProperty ("loopEnabled", loopEnabled);
        root->setProperty ("loopStart", loopStart);
        root->setProperty ("loopEnd", loopEnd);
        root->setProperty ("masterGain", audioEngine.getMasterGain());
        root->setProperty ("masterMute", audioEngine.getMasterMute());

        juce::var garr;
        for (auto& g : groups)
        {
            auto* go = new juce::DynamicObject();
            go->setProperty ("name", g->name);
            go->setProperty ("video", copyMedia (g->file, mediaDir, seen, allOk, doCopy, queue));
            go->setProperty ("duration", g->duration);
            go->setProperty ("expanded", g->expanded);
            go->setProperty ("videoMute", g->videoMute);
            go->setProperty ("videoSolo", g->videoSolo);
            go->setProperty ("cuts", doublesToVar (g->cutMarkers));
            juce::var marr;
            for (auto& mk : g->markers) { auto* mo = new juce::DynamicObject(); mo->setProperty ("t", mk.time); mo->setProperty ("name", mk.name); marr.append (juce::var (mo)); }
            go->setProperty ("markers", marr);

            juce::var tarr;
            for (auto& t : g->tracks)
            {
                auto* to = new juce::DynamicObject();
                to->setProperty ("name", t->name);
                to->setProperty ("file", copyMedia (t->file, mediaDir, seen, allOk, doCopy, queue));
                to->setProperty ("sourceLength", t->sourceLength);
                to->setProperty ("mute", t->mute);
                to->setProperty ("solo", t->solo);
                to->setProperty ("volume", t->volume);
                to->setProperty ("pan", t->pan);
                to->setProperty ("send", t->send);
                to->setProperty ("mixGroup", t->mixGroup);
                to->setProperty ("recordArm", t->recordArm);
                if (t->engineId >= 0) to->setProperty ("fx", audioEngine.saveTrackFx (t->engineId));
                juce::var aarr;
                for (auto& p : t->volumeAuto) { auto* po = new juce::DynamicObject(); po->setProperty ("t", p.time); po->setProperty ("v", p.value); aarr.append (juce::var (po)); }
                to->setProperty ("auto", aarr);
                to->setProperty ("autoOn", t->automationOn);
                to->setProperty ("beats", doublesToVar (t->beatMarkers));

                juce::var carr;
                for (auto& c : t->clips)
                {
                    auto* co = new juce::DynamicObject();
                    co->setProperty ("start", c.timelineStart);
                    co->setProperty ("in", c.sourceIn);
                    co->setProperty ("dur", c.duration);
                    co->setProperty ("fadeIn", c.fadeIn);
                    co->setProperty ("fadeOut", c.fadeOut);
                    co->setProperty ("fadeInShape", c.fadeInShape);
                    co->setProperty ("fadeOutShape", c.fadeOutShape);
                    co->setProperty ("gainDb", c.gainDb);
                    co->setProperty ("stretch", c.stretchRatio);
                    co->setProperty ("spdIn", c.speedFadeIn);
                    co->setProperty ("spdOut", c.speedFadeOut);
                    co->setProperty ("bakeSrc", c.bakedSrcSeconds);
                    carr.append (juce::var (co));
                }
                to->setProperty ("clips", carr);
                tarr.append (juce::var (to));
            }
            go->setProperty ("tracks", tarr);
            garr.append (juce::var (go));
        }
        root->setProperty ("groups", garr);
        return juce::var (root);
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

                bool present = true;                                   // build the JSON on the message thread (fast),
                std::vector<CopyJob> queue;                            // but defer the heavy media copy to a worker
                const juce::var rv = buildProjectVar (media, true, present, &queue);
                const juce::String json = juce::JSON::toString (rv);
                const juce::File projFile = dir.getChildFile ("project.json");

                titleLabel.setText ("Saving...", juce::dontSendNotification);
                auto a = alive;
                std::thread ([this, a, queue, json, projFile, dir, present]
                {
                    bool allOk = present;
                    for (const auto& job : queue)
                        if (! (job.second.existsAsFile() && job.second.getSize() == job.first.getSize()))
                            if (! job.first.copyFileTo (job.second)) allOk = false;
                    const bool wrote = projFile.replaceWithText (json);
                    juce::MessageManager::callAsync ([this, a, wrote, allOk, dir]
                    {
                        if (! a->load()) return;
                        clearRecovery();   // a real save supersedes the autosave
                        if (! wrote)      titleLabel.setText ("Save failed: couldn't write project.json.", juce::dontSendNotification);
                        else if (! allOk) titleLabel.setText ("Saved (some media missing/kept by reference): " + dir.getFileName(), juce::dontSendNotification);
                        else              titleLabel.setText ("Saved: " + dir.getFileName(), juce::dontSendNotification);
                    });
                }).detach();
            });
    }

    //== Autosave / crash recovery (references original media; no copy) ==
    juce::File recoveryFile() const
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Layback").getChildFile ("recovery.lbproj.json");
    }
    void autosaveTick()
    {
        if (groups.empty()) return;
        bool ok = true;
        const auto rv = buildProjectVar ({}, false, ok);
        const auto f = recoveryFile();
        f.getParentDirectory().createDirectory();
        f.replaceWithText (juce::JSON::toString (rv));
    }
    void clearRecovery() { recoveryFile().deleteFile(); }
    void maybeOfferRecovery()
    {
        const auto f = recoveryFile();
        if (! f.existsAsFile()) return;
        auto* w = new juce::AlertWindow ("Recover unsaved session?",
                       "Layback Station found an unsaved session from a previous run. Restore it?",
                       juce::MessageBoxIconType::QuestionIcon);
        w->addButton ("Restore", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Discard", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, f] (int r)
        {
            std::unique_ptr<juce::AlertWindow> own (w);
            if (r == 1) { const auto v = juce::JSON::parse (f.loadFileAsString()); if (v.isObject()) loadProjectFromVar (v, f.getParentDirectory()); }
            else        clearRecovery();
            restoreKeyFocus();
        }), false);
    }

    //== Relink media that moved since the project was saved ==
    bool relinkTrack (AudioTrack& t, const juce::File& nf)
    {
        double len = 0.0;
        const int id = audioEngine.addTrack (nf, len);
        if (id < 0) return false;
        if (t.engineId >= 0) audioEngine.removeTrack (t.engineId);
        t.engineId = id; t.file = nf; if (t.sourceLength <= 0.0) t.sourceLength = len;
        if (t.thumb != nullptr) t.thumb->removeChangeListener (this);
        t.thumb = std::make_unique<juce::AudioThumbnail> (512, audioEngine.getFormatManager(), thumbnailCache);
        t.thumb->addChangeListener (this);
        t.thumb->setSource (new juce::FileInputSource (nf));
        audioEngine.setTrackPan (id, t.pan);
        audioEngine.setTrackSend (id, t.send);
        std::vector<std::pair<double, float>> env; for (auto& p : t.volumeAuto) env.push_back ({ p.time, p.value });
        audioEngine.setTrackAutomation (id, env, t.automationOn);
        for (auto& cc : t.clips)
        {
            const double srcSec = cc.bakedSrcSeconds > 0.0 ? cc.bakedSrcSeconds : cc.duration / juce::jmax (0.01, cc.stretchRatio);
            if      (cc.speedFadeIn > 0.0 || cc.speedFadeOut > 0.0) cc.stretched = audioEngine.makeSpeedFaded   (id, cc.sourceIn, srcSec, cc.speedFadeIn, cc.speedFadeOut);
            else if (cc.stretchRatio != 1.0)                        cc.stretched = audioEngine.makeStretchedClip (id, cc.sourceIn, srcSec, cc.stretchRatio);
        }
        return true;
    }
    void relinkMissingMedia()
    {
        bool any = false;
        for (auto& g : groups) { if (! g->file.existsAsFile()) any = true; for (auto& t : g->tracks) if (! t->file.existsAsFile()) any = true; }
        if (! any) { titleLabel.setText ("No missing media to relink", juce::dontSendNotification); return; }
        chooser = std::make_unique<juce::FileChooser> ("Choose a folder that contains the missing media", juce::File ("~/Desktop"), juce::String());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this] (const juce::FileChooser& fc)
            {
                restoreKeyFocus();
                const auto dir = fc.getResult();
                if (dir == juce::File() || ! dir.isDirectory()) return;
                const auto found = dir.findChildFiles (juce::File::findFiles, true);
                auto byName = [&] (const juce::String& nm) -> juce::File
                { for (auto& f : found) if (f.getFileName().equalsIgnoreCase (nm)) return f; return {}; };
                int n = 0;
                for (auto& g : groups)
                {
                    if (! g->file.existsAsFile()) { const auto nf = byName (g->file.getFileName()); if (nf != juce::File()) { g->file = nf; ++n; } }
                    for (auto& t : g->tracks)
                        if (! t->file.existsAsFile()) { const auto nf = byName (t->file.getFileName()); if (nf != juce::File() && relinkTrack (*t, nf)) ++n; }
                }
                if (validGroup (activeGroup)) { video.loadFile (groups[(size_t) activeGroup]->file); activateGroup (activeGroup); }
                resized(); timeline.repaint();
                titleLabel.setText (n > 0 ? ("Relinked " + juce::String (n) + " file(s)") : "No matching files found in that folder", juce::dontSendNotification);
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
        clearRecovery();           // a fresh/opened session supersedes any autosave (var already in memory if restoring)
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
                if (auto* ma = gv["markers"].getArray())
                    for (auto& mv : *ma) grp->markers.push_back ({ (double) mv.getProperty ("t", 0.0), mv["name"].toString() });
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
                        tr->mute         = (bool)  tv.getProperty ("mute", false);
                        tr->solo         = (bool)  tv.getProperty ("solo", false);
                        tr->volume       = (float) tv.getProperty ("volume", 1.0);
                        tr->pan          = (float) tv.getProperty ("pan", 0.0);
                        tr->send         = (float) tv.getProperty ("send", 0.0);
                        tr->mixGroup     = (int)   tv.getProperty ("mixGroup", 0);
                        tr->recordArm    = (bool)  tv.getProperty ("recordArm", false);
                        tr->automationOn = (bool)  tv.getProperty ("autoOn", false);
                        if (auto* aa = tv["auto"].getArray())
                            for (auto& av : *aa) tr->volumeAuto.push_back ({ (double) av.getProperty ("t", 0.0), (float) av.getProperty ("v", 1.0) });
                        tr->beatMarkers  = varToDoubles (tv["beats"]);
                        if (auto* carr = tv["clips"].getArray())
                            for (auto& cv : *carr)
                                tr->clips.push_back ({ (double) cv["start"], (double) cv["in"], (double) cv["dur"],
                                                       (double) cv.getProperty ("fadeIn", 0.0), (double) cv.getProperty ("fadeOut", 0.0),
                                                       (int) cv.getProperty ("fadeInShape", 0), (int) cv.getProperty ("fadeOutShape", 0),
                                                       (float) cv.getProperty ("gainDb", 0.0),
                                                       (double) cv.getProperty ("stretch", 1.0),
                                                       (double) cv.getProperty ("spdIn", 0.0),
                                                       (double) cv.getProperty ("spdOut", 0.0),
                                                       (double) cv.getProperty ("bakeSrc", 0.0) });
                        if (tr->clips.empty() && tr->sourceLength > 0.0)
                            tr->clips.push_back ({ 0.0, 0.0, tr->sourceLength });   // never silently empty

                        if (id < 0)
                        {
                            ++missingMedia;
                        }
                        else
                        {
                            audioEngine.setTrackPan (id, tr->pan);   // restore pan (volume is applied via applyMixGains)
                            audioEngine.setTrackSend (id, tr->send);
                            audioEngine.restoreTrackFx (id, tv["fx"]);   // recreate EQ/Comp + hosted plugins with their state
                            { std::vector<std::pair<double, float>> env; for (auto& p : tr->volumeAuto) env.push_back ({ p.time, p.value });
                              audioEngine.setTrackAutomation (id, env, tr->automationOn); }
                            for (auto& cc : tr->clips)   // re-bake stretched / speed-faded clips (baked audio isn't stored)
                            {
                                const double srcSec = cc.bakedSrcSeconds > 0.0 ? cc.bakedSrcSeconds
                                                                               : cc.duration / juce::jmax (0.01, cc.stretchRatio);
                                if (cc.speedFadeIn > 0.0 || cc.speedFadeOut > 0.0)
                                    cc.stretched = audioEngine.makeSpeedFaded (id, cc.sourceIn, srcSec, cc.speedFadeIn, cc.speedFadeOut);
                                else if (cc.stretchRatio != 1.0)
                                    cc.stretched = audioEngine.makeStretchedClip (id, cc.sourceIn, srcSec, cc.stretchRatio);
                            }
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
        audioEngine.setMasterMute ((bool) root.getProperty ("masterMute", false));
        if (groups.empty()) { loopEnabled = false; loopStart = loopEnd = 0.0; }
        loopToggle.setToggleState (loopEnabled, juce::dontSendNotification);

        if (! groups.empty())
        {
            const int ag = (int) root["activeGroup"];
            activeGroup = -1;
            activateGroup (juce::jlimit (0, (int) groups.size() - 1, ag < 0 ? 0 : ag));
        }
        timeline.setLoop (loopEnabled, loopStart, loopEnd);

        automationVisible = false;   // show the automation overlay if any active-group track has it on
        if (validGroup (activeGroup))
            for (auto& tr : groups[(size_t) activeGroup]->tracks)
                if (tr->automationOn) { automationVisible = true; break; }
        timeline.setAutomationMode (automationVisible);

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

    void cancelStemSplit()
    {
        splitCancelled = true;
        if (auto p = stemProc.lock()) p->kill();
        titleLabel.setText ("Cancelling stem split...", juce::dontSendNotification);
    }

    void timerCallback() override
    {
        if (++autosaveCounter >= 1800) { autosaveCounter = 0; autosaveTick(); }   // autosave every ~60 s

        if (splitting && (++splitElapsedTicks % 30 == 0))   // heartbeat so a multi-minute split doesn't look frozen
            titleLabel.setText ("Splitting stems... " + juce::String (splitElapsedTicks / 30) + " s  (right-click the track to cancel)", juce::dontSendNotification);
        if (audioEngine.isRecording() && (++recordTicks % 30 == 0))
            titleLabel.setText ("Recording... " + juce::String (recordTicks / 30) + " s  (press Record to stop)", juce::dontSendNotification);

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
                if (audioEngine.isRecording()) finishRecording();   // capture the take if we were recording
                else                           pauseAll();
            }

            playhead = ph;

            if (playing && video.isPlaying() && playhead < vdur - 0.05)
            {
                const double drift = video.getPositionSeconds() - playhead;          // +ve => video ahead of audio
                if (std::abs (drift) > 0.35) video.setPositionSeconds (playhead);     // way off (e.g. after a scrub): hard re-lock
                else                         video.setRate (juce::jlimit (0.90, 1.10, 1.0 - drift * 0.6));   // gentle rate chase -> frame-tight lock, no jumps
            }

            if (! isScrubbing) timeline.setPlayhead (playhead);
        }

        timeLabel.setText (formatTime (playhead) + "  /  " + formatTime (timelineLength()), juce::dontSendNotification);
        const int bbBar   = (int) (playhead / 2.0) + 1;                  // 120 BPM 4/4 -> bars/beats LCD
        const double inBar = playhead - 2.0 * (double) (bbBar - 1);
        const int bbBeat  = juce::jlimit (1, 4, (int) (inBar / 0.5) + 1);
        logicBar.setPosition (juce::String (bbBar) + "  " + juce::String (bbBeat), formatTime (playhead));
        const double fps  = video.getFrameRate() > 1.0 ? video.getFrameRate() : 30.0;   // frame-accurate to the loaded clip
        const int    fpsI = (int) juce::jmax (1.0, std::round (fps));
        const int tcH = (int) (playhead / 3600.0), tcM = ((int) (playhead / 60.0)) % 60, tcS = ((int) playhead) % 60, tcF = ((int) (playhead * fps)) % fpsI;
        ptBar.setPosition (juce::String::formatted ("%02d:%02d:%02d:%02d", tcH, tcM, tcS, tcF), juce::String (bbBar) + "|" + juce::String (bbBeat));
        audioEngine.setExternalPeak (videoAudible ? video.getAudioPeak() : 0.0f);   // full-mix Master-strip meter incl. video
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
    juce::Rectangle<int> logicMEdit, logicMFunc, logicMView;   // Logic local-menu hit areas

    std::unique_ptr<VideoWindow> videoWindow;   // destroyed in ~MainComponent before `video`
    bool videoWindowOpen = false;
    bool videoAudible = false;                  // is the video's audio currently in the mix (for the full-mix meter)
    std::vector<std::unique_ptr<PluginWindow>> pluginWindows;
    std::unique_ptr<PluginListWindow> pluginListWindow;
    std::unique_ptr<InfoWindow>       helpWindow;
    std::map<int, juce::PluginDescription>     pluginMenuMap;   // menu id -> plugin to instantiate
    bool scanning = false;
    bool pluginsScanned = false;
    bool splitting = false;   // a stem split is in progress (offline render on a worker thread)
    std::weak_ptr<juce::ChildProcess> stemProc;   // the running Demucs process (for cancel)
    bool splitCancelled = false;
    int  splitElapsedTicks = 0;
    bool installing = false;  // the one-time Demucs setup is running
    int  ptEditMode = 1;      // Pro Tools edit mode: 0 Shuffle, 1 Slip, 2 Spot, 3 Grid
    AudioClip clipboardClip;  // copy/paste clipboard
    bool      hasClipboard = false;
    bool      automationVisible = false;   // volume-automation overlay/read is on
    bool      frameSnapOn = false;         // snap edits to video frames
    int       autosaveCounter = 0;         // timer ticks since the last autosave
    double    recordStartTime = 0.0;       // playhead position where recording began
    int       recordTicks = 0;             // timer ticks since recording started (for the heartbeat)
    int       autoMode = 0;                // 0 = Read, 1 = Latch (write fader rides into the envelope)

    MixerView mixerView;
    bool      mixerVisible = false;
    LogicControlBar logicBar;     // shown only for the Logic station
    LogicInspector  logicInspector;
    ProToolsControlBar ptBar;     // shown only for the Pro Tools station

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
    std::shared_ptr<ProcRegistry>      procReg { std::make_shared<ProcRegistry>() };

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
            centreWithSize (1120, 820);     // restored-down size
            setVisible (true);
            setFullScreen (true);           // open maximized to the screen, like a real DAW
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
START_JUCE_APPLICATION (LaybackApplication)
