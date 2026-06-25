#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
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

    //== Recording (capture from the audio input device) ==
    bool  hasAudioInput() const;          // an input device with >=1 channel is open
    void  startRecording();               // begin capturing input
    bool  isRecording() const;
    /** Stop capturing; returns the recorded take (stereo, device rate). Empty if nothing captured. */
    juce::AudioBuffer<float> stopRecording();

    /** Decodes a file into memory at the device rate. Returns a track id, or -1.
        fullLengthSeconds is set to the song's duration. */
    int  addTrack (const juce::File& file, double& fullLengthSeconds);
    void removeTrack (int trackId);
    void clearTracks();

    void setTrackClips (int trackId, const std::vector<AudioClip>& clips);
    /** Push a track's volume automation envelope (time, linear-gain) + whether to read it. */
    void setTrackAutomation (int trackId, const std::vector<std::pair<double, float>>& env, bool on);
    void setTrackGain  (int trackId, float gain);   // post-fader/mute gain (Main folds in track volume)
    void setTrackPan   (int trackId, float pan);    // -1..+1
    void  setMasterGain (float g);
    float getMasterGain() const;
    void  setMasterMute (bool m);
    bool  getMasterMute() const;
    void  setExternalPeak (float v);                // fold in non-engine audio (e.g. the video) for the full-mix meter
    float getTrackPeak  (int trackId);              // last block's post-fader peak (0..1+), for meters
    float getMasterPeak() const;

    //== Per-track effect chain (native built-ins + hosted AU/VST3 plugins) ==
    juce::AudioPluginFormatManager& getPluginFormats() { return pluginFormats; }
    juce::KnownPluginList&          getKnownPlugins()  { return knownPlugins;  }

    /** Insert a built-in effect (0 = EQ, 1 = Compressor). */
    void addNativeEffect (int trackId, int which);
    /** Instantiate a scanned AU/VST3 plugin and insert it. Returns false + sets error on failure. */
    bool addHostedPlugin (int trackId, const juce::PluginDescription& desc, juce::String& error);
    /** Same, but instantiates on a background thread (a slow/hanging plugin won't freeze the UI).
        The callback fires on the message thread. */
    void addHostedPluginAsync (int trackId, const juce::PluginDescription& desc,
                               std::function<void (bool, juce::String)> done);

    int  trackPluginCount (int trackId);
    juce::AudioProcessor* trackPlugin (int trackId, int index);   // raw ptr, valid until removed (message thread only)

    /** Serialize a track's whole FX chain (native + hosted, with state) to a var array. */
    juce::var saveTrackFx (int trackId);
    /** Recreate a track's FX chain from saveTrackFx() output (skips plugins missing on this machine). */
    void      restoreTrackFx (int trackId, const juce::var& fxArray);
    juce::String trackPluginName (int trackId, int index);
    void removeTrackPlugin (int trackId, int index);

    //== FX / aux bus (a shared send destination, e.g. a reverb) ==
    void setTrackSend (int trackId, float level);   // 0..1, post-fader
    void addAuxEffect (int which);                  // native effect (0 EQ,1 Comp,2 Reverb,3 Delay,4 Limiter,5 Gate)
    int  auxPluginCount();
    juce::AudioProcessor* auxPlugin (int index);
    juce::String auxPluginName (int index);
    void removeAuxPlugin (int index);

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
    /** Peak magnitude (0..1) of a clip's source region, for Normalize. */
    float clipPeak (int trackId, double sourceIn, double duration);
    double sampleRate() const;   // current device sample rate (for length<->samples math)
    /** Bake a pitch-preserved time-stretch of a clip's source region (ratio = output/source length).
        Returns a stereo buffer at the device rate to play in place of the source; nullptr on failure. */
    std::shared_ptr<juce::AudioBuffer<float>> makeStretchedClip (int trackId, double sourceIn, double srcSeconds, double ratio);
    /** Bake a tape-style speed fade (varispeed ramp at head/tail) of a clip's source region. */
    std::shared_ptr<juce::AudioBuffer<float>> makeSpeedFaded (int trackId, double sourceIn, double srcSeconds, double speedInSec, double speedOutSec);

    /** Detects onset/beat times (seconds, in the source) of a loaded track. */
    std::vector<double> computeTrackOnsets (int trackId);

private:
    struct TrackData
    {
        int id = 0;
        juce::AudioBuffer<float> audio;     // at device rate
        std::vector<AudioClip>   clips;
        std::atomic<float>       gain { 1.0f };
        std::atomic<float>       pan  { 0.0f };
        std::atomic<float>       send { 0.0f };   // post-fader send level to the FX/aux bus (0..1)
        std::atomic<float>       peak { 0.0f };   // post-fader peak of the last block (for the meter)
        std::vector<std::unique_ptr<juce::AudioProcessor>> chain;   // per-track insert FX (native + hosted)
        std::vector<std::pair<double, float>> autoEnv;   // volume envelope (time, gain), sorted; read under lock
        std::atomic<bool>        autoOn { false };        // use the envelope instead of the static gain
    };

    class Mixer : public juce::PositionableAudioSource
    {
    public:
        void prepareToPlay (int samplesPerBlock, double sr) override;
        void releaseResources() override {}
        void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

        void setNextReadPosition (juce::int64 p) override { pos.store (p, std::memory_order_relaxed); }
        juce::int64 getNextReadPosition() const override  { return pos.load (std::memory_order_relaxed); }
        juce::int64 getTotalLength() const override        { return totalLen.load (std::memory_order_relaxed); }
        bool isLooping() const override                    { return false; }

        void recomputeLength();
        void ensureScratch (int channels, int samples);   // non-realtime only (prepare / offline render)

        juce::CriticalSection lock;
        std::vector<std::unique_ptr<TrackData>> tracks;
        std::atomic<juce::int64> pos { 0 };       // atomic: read unlocked by the transport/UI
        std::atomic<juce::int64> totalLen { 0 };
        std::atomic<double> rate { 44100.0 };     // atomic: written by device-prepare thread, read on message thread
        double minLengthSeconds = 0.0;
        juce::AudioBuffer<float> scratch;         // reused per-track render buffer (no realtime alloc)
        juce::AudioBuffer<float> aux;             // FX/aux bus accumulator (tracks send into it; chain -> master)
        std::vector<std::unique_ptr<juce::AudioProcessor>> auxChain;   // FX on the aux bus (e.g. a shared reverb)
        std::atomic<int> preparedBlock { 512 };   // grow-only; never hand a plugin a larger block than prepared
        std::atomic<float> masterGain { 1.0f };
        std::atomic<float> masterPeak { 0.0f };
        std::atomic<bool>  masterMute { false };
        std::atomic<float> externalPeak { 0.0f };   // video (non-engine) audio peak, folded into the full-mix meter
    };

    static juce::AudioBuffer<float> resampleBuffer (const juce::AudioBuffer<float>& in, double inRate, double outRate);
    TrackData* findTrack (int id);   // call under mixer.lock
    void prepareProcessor (juce::AudioProcessor& p) const;
    void addProcessorToTrack (int trackId, std::unique_ptr<juce::AudioProcessor> p);

    // Captures the input device into a preallocated buffer while recording. Reads inputs only,
    // never writes outputs, so it can run as a second device callback without touching the mix.
    struct RecordTap : public juce::AudioIODeviceCallback
    {
        std::atomic<bool> active { false };
        std::atomic<int>  writePos { 0 };
        juce::AudioBuffer<float> buf;
        double sr = 44100.0;

        void audioDeviceAboutToStart (juce::AudioIODevice* d) override
        {
            sr = d->getCurrentSampleRate() > 0.0 ? d->getCurrentSampleRate() : 44100.0;
            buf.setSize (2, (int) (sr * 60.0 * 8.0));   // up to ~8 minutes, stereo
            buf.clear(); writePos.store (0);
        }
        void audioDeviceStopped() override {}
        void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                               float* const* out, int numOut, int n,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            for (int ch = 0; ch < numOut; ++ch)   // CRITICAL: this is a second callback; JUCE sums our output
                if (out[ch] != nullptr) juce::FloatVectorOperations::clear (out[ch], n);   // contribute silence, not garbage
            if (! active.load()) return;
            int w = writePos.load();
            const int cap = buf.getNumSamples();
            for (int i = 0; i < n && w < cap; ++i, ++w)
            {
                const float l = (numIn > 0 && in[0] != nullptr) ? in[0][i] : 0.0f;
                const float r = (numIn > 1 && in[1] != nullptr) ? in[1][i] : l;
                buf.setSample (0, w, l);
                buf.setSample (1, w, r);
            }
            writePos.store (w);
        }
    };

    juce::AudioDeviceManager   deviceManager;
    juce::AudioFormatManager   formatManager;
    juce::AudioPluginFormatManager pluginFormats;
    juce::KnownPluginList      knownPlugins;
    juce::AudioSourcePlayer    sourcePlayer;
    juce::AudioTransportSource transport;
    RecordTap recordTap;
    Mixer  mixer;
    double deviceRate = 44100.0;
    int    nextTrackId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
