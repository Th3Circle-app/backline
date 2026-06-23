#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

//==============================================================================
/** A JUCE component that displays video using macOS AVFoundation
    (AVPlayer + AVPlayerLayer). Hardware-accelerated, frame-accurate seeking.

    This is the Phase 0 video surface. Later it becomes the per-clip renderer
    inside the timeline, fed by the transport/playhead rather than its own controls.
*/
class VideoView : public juce::Component
{
public:
    VideoView();
    ~VideoView() override;

    /** Loads a video file. Returns true on success. */
    bool loadFile (const juce::File& file);

    /** Reads a video file's duration (seconds) without loading it into the player. */
    static double probeDurationSeconds (const juce::File& file);

    /** Muxes a video file's picture with an audio file into outFile (.mov), async.
        onDone(success, errorMessage) is called on the message thread. */
    static void exportVideoWithAudio (const juce::File& videoFile, const juce::File& audioFile,
                                      double lengthSeconds, const juce::File& outFile,
                                      std::function<void (bool, juce::String)> onDone);

    void play();
    void pause();
    bool isPlaying() const;

    double getDurationSeconds() const;
    double getPositionSeconds() const;

    /** Frame-accurate seek (zero tolerance). */
    void setPositionSeconds (double seconds);

    /** Mutes/unmutes the video's own (baked-in) audio track. */
    void setMuted (bool shouldBeMuted);

    /** Peak level (0..1) of the video's audio from the last processed block, for metering. */
    float getAudioPeak() const;

    void resized() override;

private:
    void* impl = nullptr;   // opaque pointer to the Objective-C++ implementation
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VideoView)
};
