#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <juce_gui_extra/juce_gui_extra.h>
#include "VideoView.h"
#include "TimelineComponent.h"
#include "AudioEngine.h"
#include "FfmpegTool.h"
#include "Clip.h"
#include "Track.h"

//==============================================================================
// Multi-video: a project is a stack of video groups (each = one video + its own
// audio tracks). One group is "active": its video previews and its audio plays.
// The audio engine holds every track's audio in RAM; only the active group's
// tracks carry clips (others are silent), so switching groups is instant.
class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    MainComponent()
    {
        addAndMakeVisible (video);

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

        timeLabel.setJustificationType (juce::Justification::centredRight);
        timeLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (timeLabel);

        titleLabel.setText ("Layback Station", juce::dontSendNotification);
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0a6));
        addAndMakeVisible (titleLabel);

        timeline.onSeek          = [this] (double s) { seekAll (s); };
        timeline.onScrubStart    = [this] { isScrubbing = true;  wasPlaying = playing; pauseAll(); };
        timeline.onScrubEnd      = [this] { isScrubbing = false; if (wasPlaying) playAll(); };
        timeline.onClipChanged   = [this] (int g, int t, int c, AudioClip nc) { clipChanged (g, t, c, nc); };
        timeline.onClipMenu      = [this] (int g, int t, int c, double tm) { showClipMenu (g, t, c, tm); };
        timeline.onClipSelected  = [this] (int g, int t, int c) { selGroup = g; selTrack = t; selClip = c; timeline.setSelection (g, t, c); };
        timeline.onToggleExpand  = [this] (int g) { if (validGroup (g)) { groups[(size_t) g]->expanded = ! groups[(size_t) g]->expanded; resized(); timeline.repaint(); } };
        timeline.onImportTrack   = [this] (int g) { importTrack (g); };
        timeline.onTrackMute     = [this] (int g, int t) { if (validTrack (g, t)) { auto& tr = *groups[(size_t) g]->tracks[(size_t) t]; tr.mute = ! tr.mute; applyMixGains(); timeline.repaint(); } };
        timeline.onTrackSolo     = [this] (int g, int t) { if (validTrack (g, t)) { auto& tr = *groups[(size_t) g]->tracks[(size_t) t]; tr.solo = ! tr.solo; applyMixGains(); timeline.repaint(); } };
        timeline.onVideoMute     = [this] (int g) { if (validGroup (g)) { groups[(size_t) g]->videoMute = ! groups[(size_t) g]->videoMute; applyMixGains(); timeline.repaint(); } };
        timeline.onVideoSolo     = [this] (int g) { if (validGroup (g)) { groups[(size_t) g]->videoSolo = ! groups[(size_t) g]->videoSolo; applyMixGains(); timeline.repaint(); } };
        timeline.onActivateGroup = [this] (int g) { activateGroup (g); };
        timeline.onAddVideo      = [this] { openAddVideo(); };
        timeline.onLoopChanged   = [this] (bool en, double s, double e)
        {
            loopEnabled = en; loopStart = s; loopEnd = e;
            loopToggle.setToggleState (en, juce::dontSendNotification);
        };
        timeline.onLoopMenu      = [this] (double t) { showLoopMenu (t); };
        timeline.setGroups (&groups);
        timelineViewport.setViewedComponent (&timeline, false);
        timelineViewport.setScrollBarsShown (true, false);
        addAndMakeVisible (timelineViewport);

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

        startTimerHz (30);
    }

    ~MainComponent() override
    {
        if (alive) *alive = false;   // in-flight ffmpeg worker callbacks bail instead of touching a dead window
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
        applyMixGains();
        timeline.setSelection (selGroup, selTrack, selClip);
        resized();
        timeline.repaint();
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff121317)); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        titleLabel.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);

        timelineViewport.setBounds (r.removeFromBottom (340));
        r.removeFromBottom (8);

        auto controls = r.removeFromBottom (40);
        r.removeFromBottom (8);
        video.setBounds (r);

        openButton.setBounds (controls.removeFromLeft (104));
        controls.removeFromLeft (8);
        playButton.setBounds (controls.removeFromLeft (84));
        controls.removeFromLeft (10);
        loopToggle.setBounds (controls.removeFromLeft (66));
        controls.removeFromLeft (10);
        snapToggle.setBounds (controls.removeFromLeft (62));
        controls.removeFromLeft (10);
        exportButton.setBounds (controls.removeFromLeft (90));
        timeLabel.setBounds  (controls.removeFromRight (150));

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
            audioEngine.setTrackGain (t->engineId, aud ? 1.0f : 0.0f);
        }
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
            });
    }

    void openAddVideo()
    {
        chooser = std::make_unique<juce::FileChooser> ("Add a video", juce::File ("~/Desktop"),
                    "*.mov;*.mp4;*.m4v;*.qt;*.avi;*.mpg;*.mpeg;*.m2v;*.m2ts;*.mts;*.ts;*.3gp;*.3g2;*.mxf;*.dv;*.mkv;*.webm;*.wmv;*.flv");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
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
            [this] (int r) { if (r == 1) exportVideo(); else if (r == 2) exportAudio(); });
    }

    void setLoopRegion (double s, double e)
    {
        if (e <= s) return;
        loopEnabled = true; loopStart = s; loopEnd = e;
        loopToggle.setToggleState (true, juce::dontSendNotification);
        timeline.setLoop (true, s, e);
    }

    void clearLoop()
    {
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
            });
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { timeline.repaint(); }

    void timerCallback() override
    {
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
    VideoView video;
    AudioEngine audioEngine;
    juce::AudioThumbnailCache thumbnailCache { 32 };
    std::vector<std::unique_ptr<VideoGroup>> groups;
    TimelineComponent timeline;
    juce::Viewport timelineViewport;

    juce::TextButton openButton, playButton, exportButton;
    juce::ToggleButton loopToggle, snapToggle;
    juce::Label timeLabel, titleLabel;
    std::unique_ptr<juce::FileChooser> chooser;

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
