#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Clip.h"

//==============================================================================
/** In-memory multitrack mixer + master transport.

    Each track's audio is decoded and resampled to the device rate once at load,
    then arranged as clips on the timeline. A single master position advances
    through the whole timeline (so it is the project's master clock, with the
    video slaved to it). Per-track gain implements mute/solo. Glitch-free because
    everything is in RAM (no disk/resampling on the audio thread).
*/
class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    juce::AudioFormatManager& getFormatManager() { return formatManager; }
    double getDeviceSampleRate() const { return deviceRate; }

    /** Decodes a file into memory at the device rate. Returns a track id, or -1.
        fullLengthSeconds is set to the song's duration. */
    int  addTrack (const juce::File& file, double& fullLengthSeconds);
    void removeTrack (int trackId);
    void clearTracks();

    void setTrackClips (int trackId, const std::vector<AudioClip>& clips);
    void setTrackGain  (int trackId, float gain);

    /** Ensures the transport runs at least this long (so the playhead keeps
        advancing over video-only regions with no audio). */
    void setMinLengthSeconds (double seconds);

    void play();
    void stop();
    bool isPlaying() const;

    void   setPositionSeconds (double s);
    double getPositionSeconds() const;
    double getLengthSeconds() const;

    /** Offline-renders the current mix (active group's clips + mute/solo gains) to a stereo WAV. */
    bool renderMixToFile (const juce::File& outWav, double lengthSeconds);

    /** Detects onset/beat times (seconds, in the source) of a loaded track. */
    std::vector<double> computeTrackOnsets (int trackId);

private:
    struct TrackData
    {
        int id = 0;
        juce::AudioBuffer<float> audio;     // at device rate
        std::vector<AudioClip>   clips;
        std::atomic<float>       gain { 1.0f };
    };

    class Mixer : public juce::PositionableAudioSource
    {
    public:
        void prepareToPlay (int, double sr) override { rate = sr; recomputeLength(); }
        void releaseResources() override {}
        void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

        void setNextReadPosition (juce::int64 p) override { pos.store (p, std::memory_order_relaxed); }
        juce::int64 getNextReadPosition() const override  { return pos.load (std::memory_order_relaxed); }
        juce::int64 getTotalLength() const override        { return totalLen.load (std::memory_order_relaxed); }
        bool isLooping() const override                    { return false; }

        void recomputeLength();

        juce::CriticalSection lock;
        std::vector<std::unique_ptr<TrackData>> tracks;
        std::atomic<juce::int64> pos { 0 };       // atomic: read unlocked by the transport/UI
        std::atomic<juce::int64> totalLen { 0 };
        double rate = 44100.0;
        double minLengthSeconds = 0.0;
    };

    static juce::AudioBuffer<float> resampleBuffer (const juce::AudioBuffer<float>& in, double inRate, double outRate);
    TrackData* findTrack (int id);   // call under mixer.lock

    juce::AudioDeviceManager   deviceManager;
    juce::AudioFormatManager   formatManager;
    juce::AudioSourcePlayer    sourcePlayer;
    juce::AudioTransportSource transport;
    Mixer  mixer;
    double deviceRate = 44100.0;
    int    nextTrackId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
