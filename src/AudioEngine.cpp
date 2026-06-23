#include "AudioEngine.h"
#include "FfmpegTool.h"
#include "NativeEffects.h"
#include <cmath>

//==============================================================================
void AudioEngine::Mixer::recomputeLength()
{
    juce::int64 len = (juce::int64) (minLengthSeconds * rate);
    for (auto& t : tracks)
        for (auto& c : t->clips)
            len = juce::jmax (len, (juce::int64) (c.timelineEnd() * rate));
    totalLen.store (len, std::memory_order_relaxed);
}

void AudioEngine::Mixer::prepareToPlay (int samplesPerBlock, double sr)
{
    rate = sr;
    preparedBlock = juce::jmax (1, samplesPerBlock);
    ensureScratch (2, preparedBlock);

    const juce::ScopedLock sl (lock);
    for (auto& t : tracks)
        for (auto& p : t->chain)
            if (p != nullptr)
            {
                p->setRateAndBufferSizeDetails (sr, preparedBlock);
                p->prepareToPlay (sr, preparedBlock);
            }
    recomputeLength();
}

void AudioEngine::Mixer::ensureScratch (int channels, int samples)   // non-realtime only
{
    if (scratch.getNumChannels() < channels || scratch.getNumSamples() < samples)
        scratch.setSize (juce::jmax (channels, scratch.getNumChannels()),
                         juce::jmax (samples,  scratch.getNumSamples()), false, true, true);
}

void AudioEngine::Mixer::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    const juce::ScopedLock sl (lock);
    const int numCh = juce::jmin (info.buffer->getNumChannels(), scratch.getNumChannels());
    const int n     = info.numSamples;
    const juce::int64 blockStart = pos.load (std::memory_order_relaxed);
    if (numCh <= 0 || n <= 0 || n > scratch.getNumSamples()) { pos.store (blockStart + n, std::memory_order_relaxed); return; }

    for (auto& tptr : tracks)
    {
        auto* t = tptr.get();
        const float g = t->gain.load();
        const bool  hasFx = ! t->chain.empty();
        if (g <= 0.0001f && ! hasFx) continue;

        // 1) render this track's clips into the per-track scratch buffer (pre-fader, pre-FX)
        for (int ch = 0; ch < numCh; ++ch) scratch.clear (ch, 0, n);

        const int tCh  = t->audio.getNumChannels();
        const int tLen = t->audio.getNumSamples();
        if (tCh > 0 && tLen > 0)
        {
            for (const auto& c : t->clips)
            {
                const juce::int64 clipStart = (juce::int64) (c.timelineStart * rate);
                const juce::int64 clipEnd   = (juce::int64) (c.timelineEnd()  * rate);
                const juce::int64 ovStart = juce::jmax (clipStart, blockStart);
                const juce::int64 ovEnd   = juce::jmin (clipEnd, blockStart + n);
                if (ovEnd <= ovStart) continue;

                const int dest = (int) (ovStart - blockStart);
                const juce::int64 srcStart = (juce::int64) (c.sourceIn * rate) + (ovStart - clipStart);
                if (srcStart < 0) continue;

                int count = (int) (ovEnd - ovStart);
                if (srcStart + count > tLen) count = (int) (tLen - srcStart);
                if (count <= 0) continue;

                const juce::int64 clipLenS = clipEnd - clipStart;
                const int fade = (int) juce::jmin ((juce::int64) (0.005 * rate), clipLenS / 2);   // 5 ms declick

                for (int ch = 0; ch < numCh; ++ch)
                {
                    const int sch = juce::jmin (ch, tCh - 1);
                    const float* sp = t->audio.getReadPointer (sch, (int) srcStart);
                    float* dp = scratch.getWritePointer (ch, dest);
                    for (int i = 0; i < count; ++i)
                    {
                        float fdf = 1.0f;
                        if (fade > 0)
                        {
                            const juce::int64 cl = (ovStart - clipStart) + i;
                            if (cl < fade)                 fdf = (float) cl / (float) fade;
                            else if (cl > clipLenS - fade) fdf = (float) (clipLenS - cl) / (float) fade;
                            fdf = juce::jlimit (0.0f, 1.0f, fdf);
                        }
                        dp[i] += sp[i] * fdf;
                    }
                }
            }
        }

        // 2) per-track insert FX chain (native built-ins + hosted plugins)
        if (hasFx)
        {
            float* chans[32];
            const int pc = juce::jmin (numCh, 32);
            for (int ch = 0; ch < pc; ++ch) chans[ch] = scratch.getWritePointer (ch, 0);
            juce::AudioBuffer<float> proxy (chans, pc, n);
            juce::MidiBuffer midi;
            for (auto& p : t->chain)
                if (p != nullptr) p->processBlock (proxy, midi);
        }

        // 3) sum into the output at the post-FX channel gain (mute/solo)
        if (g > 0.0001f)
            for (int ch = 0; ch < numCh; ++ch)
                info.buffer->addFrom (ch, info.startSample, scratch, ch, 0, n, g);
    }

    pos.store (blockStart + n, std::memory_order_relaxed);
}

//==============================================================================
AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();
   #if JUCE_MAC
    formatManager.registerFormat (new juce::CoreAudioFormat(), false);
   #endif
    juce::addDefaultFormatsToManager (pluginFormats);   // AU + VST3 hosting (new JUCE helper)
    mixer.ensureScratch (2, mixer.preparedBlock);

    deviceManager.initialiseWithDefaultDevices (0, 2);
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        if (dev->getCurrentSampleRate() > 0.0)
            deviceRate = dev->getCurrentSampleRate();

    mixer.rate = deviceRate;
    transport.setSource (&mixer);          // in-memory mix: no read-ahead thread needed
    sourcePlayer.setSource (&transport);
    deviceManager.addAudioCallback (&sourcePlayer);
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeAudioCallback (&sourcePlayer);
    transport.setSource (nullptr);
    sourcePlayer.setSource (nullptr);
    const juce::ScopedLock sl (mixer.lock);
    mixer.tracks.clear();
}

//==============================================================================
juce::AudioBuffer<float> AudioEngine::resampleBuffer (const juce::AudioBuffer<float>& in, double inRate, double outRate)
{
    if (juce::approximatelyEqual (inRate, outRate) || in.getNumSamples() == 0)
        return in;

    const double ratio  = inRate / outRate;
    const int    outLen = (int) std::ceil ((double) in.getNumSamples() / ratio);

    juce::AudioBuffer<float> out (in.getNumChannels(), outLen);
    out.clear();
    for (int ch = 0; ch < in.getNumChannels(); ++ch)
    {
        juce::LagrangeInterpolator interp;
        interp.process (ratio, in.getReadPointer (ch), out.getWritePointer (ch), outLen);
    }
    return out;
}

AudioEngine::TrackData* AudioEngine::findTrack (int id)
{
    for (auto& t : mixer.tracks)
        if (t->id == id) return t.get();
    return nullptr;
}

int AudioEngine::addTrack (const juce::File& file, double& fullLengthSeconds)
{
    juce::File decoded;   // temp WAV if we have to transcode via ffmpeg
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)   // JUCE/CoreAudio can't read it -> ffmpeg-decode to WAV
    {
        const juce::File ff = FfmpegTool::find();
        if (ff.existsAsFile())
        {
            decoded = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("layback_import_" + juce::String (nextTrackId) + ".wav");
            decoded.deleteFile();
            juce::String log;
            if (FfmpegTool::runSync (ff, { "-y", "-i", file.getFullPathName(),
                                           "-ac", "2", "-ar", juce::String ((int) deviceRate),
                                           decoded.getFullPathName() }, log))
                reader.reset (formatManager.createReaderFor (decoded));
        }
        if (reader == nullptr) { decoded.deleteFile(); return -1; }
    }

    const int    numCh   = (int) reader->numChannels;
    const int    len     = (int) reader->lengthInSamples;
    const double srcRate = reader->sampleRate > 0.0 ? reader->sampleRate : deviceRate;
    fullLengthSeconds = (srcRate > 0.0) ? ((double) len / srcRate) : 0.0;
    if (numCh <= 0 || len <= 0) { reader.reset(); decoded.deleteFile(); return -1; }

    juce::AudioBuffer<float> src (numCh, len);
    src.clear();
    reader->read (&src, 0, len, 0, true, true);
    reader.reset();
    decoded.deleteFile();   // no-op if ffmpeg wasn't used

    auto td   = std::make_unique<TrackData>();
    td->id    = nextTrackId++;
    td->audio = resampleBuffer (src, srcRate, deviceRate);
    td->clips.push_back ({ 0.0, 0.0, fullLengthSeconds });

    const int id = td->id;
    {
        const juce::ScopedLock sl (mixer.lock);
        mixer.tracks.push_back (std::move (td));
        mixer.recomputeLength();
    }
    return id;
}

void AudioEngine::removeTrack (int trackId)
{
    std::unique_ptr<TrackData> doomed;   // freed outside the lock
    {
        const juce::ScopedLock sl (mixer.lock);
        for (size_t i = 0; i < mixer.tracks.size(); ++i)
            if (mixer.tracks[i]->id == trackId)
            {
                doomed = std::move (mixer.tracks[i]);
                mixer.tracks.erase (mixer.tracks.begin() + (long) i);
                break;
            }
        mixer.recomputeLength();
    }
}

void AudioEngine::clearTracks()
{
    std::vector<std::unique_ptr<TrackData>> doomed;   // freed outside the lock
    {
        const juce::ScopedLock sl (mixer.lock);
        doomed = std::move (mixer.tracks);
        mixer.tracks.clear();
        mixer.recomputeLength();
    }
}

void AudioEngine::setTrackClips (int trackId, const std::vector<AudioClip>& clips)
{
    std::vector<AudioClip> staged (clips);   // allocate/copy outside the audio lock
    {
        const juce::ScopedLock sl (mixer.lock);
        if (auto* t = findTrack (trackId)) std::swap (t->clips, staged);
        mixer.recomputeLength();
    }
    // 'staged' (now the old vector) frees here, outside the lock
}

void AudioEngine::setTrackGain (int trackId, float gain)
{
    const juce::ScopedLock sl (mixer.lock);
    if (auto* t = findTrack (trackId)) t->gain.store (gain);
}

void AudioEngine::setMinLengthSeconds (double seconds)
{
    const juce::ScopedLock sl (mixer.lock);
    mixer.minLengthSeconds = seconds;
    mixer.recomputeLength();
}

//==============================================================================
void AudioEngine::play()            { transport.start(); }
void AudioEngine::stop()            { transport.stop(); }
bool AudioEngine::isPlaying() const { return transport.isPlaying(); }

// In-place seek; AudioTransportSource::setPosition locks the callback internally,
// so it is safe during playback and avoids the stop/start click (review finding).
std::vector<double> AudioEngine::computeTrackOnsets (int trackId)
{
    const juce::AudioBuffer<float>* buf = nullptr;
    {
        const juce::ScopedLock sl (mixer.lock);
        if (auto* t = findTrack (trackId)) buf = &t->audio;   // safe: onset analysis runs only on the message thread; no track-free path is wired
    }
    std::vector<double> onsets;
    if (buf == nullptr) return onsets;

    const int n = buf->getNumSamples();
    const int ch = buf->getNumChannels();
    if (n <= 0 || ch <= 0) return onsets;

    const int win = 1024, hop = 512;
    std::vector<float> env;
    env.reserve ((size_t) (n / hop + 1));
    for (int s = 0; s + win <= n; s += hop)
    {
        double sum = 0.0;
        for (int c = 0; c < ch; ++c)
        {
            const float* d = buf->getReadPointer (c);
            for (int i = 0; i < win; ++i) { const float v = d[s + i]; sum += (double) v * v; }
        }
        env.push_back ((float) std::sqrt (sum / (double) (win * ch)));
    }

    auto fluxAt = [&env] (int i) { return (i >= 1 && i < (int) env.size()) ? juce::jmax (0.0f, env[(size_t) i] - env[(size_t) (i - 1)]) : 0.0f; };

    const int w = 8;
    for (int i = 1; i + 1 < (int) env.size(); ++i)
    {
        const float fl = fluxAt (i);
        float mean = 0.0f; int cnt = 0;
        for (int k = -w; k <= w; ++k) { const int j = i + k; if (j >= 1 && j < (int) env.size()) { mean += fluxAt (j); ++cnt; } }
        mean /= (float) juce::jmax (1, cnt);

        const float thresh = mean * 1.5f + 0.0008f;
        if (fl > thresh && fl >= fluxAt (i - 1) && fl >= fluxAt (i + 1))
        {
            const double tsec = (double) ((juce::int64) i * hop) / deviceRate;
            if (onsets.empty() || tsec - onsets.back() > 0.12) onsets.push_back (tsec);
        }
    }
    return onsets;
}

void   AudioEngine::setPositionSeconds (double s) { transport.setPosition (juce::jmax (0.0, s)); }
double AudioEngine::getPositionSeconds() const     { return transport.getCurrentPosition(); }
double AudioEngine::getLengthSeconds() const        { return transport.getLengthInSeconds(); }

bool AudioEngine::renderMixToFile (const juce::File& outWav, double lengthSeconds)
{
    if (lengthSeconds <= 0.0) return false;
    transport.stop();                       // stops the audio thread touching the mixer

    outWav.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os (outWav.createOutputStream());
    if (os == nullptr) return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (os.get(), deviceRate, 2, 24, {}, 0));
    if (writer == nullptr) return false;
    os.release();                           // the writer owns the stream now

    const int block = juce::jmax (64, mixer.preparedBlock);   // match plugins' prepared block size
    juce::AudioBuffer<float> buf (2, block);
    const juce::int64 total = (juce::int64) (lengthSeconds * deviceRate);
    juce::int64 done = 0;

    const juce::int64 savedPos = mixer.getNextReadPosition();
    mixer.setNextReadPosition (0);
    while (done < total)
    {
        const int n = (int) juce::jmin ((juce::int64) block, total - done);
        buf.clear();
        juce::AudioSourceChannelInfo info (&buf, 0, n);
        mixer.getNextAudioBlock (info);
        if (! writer->writeFromAudioSampleBuffer (buf, 0, n)) break;
        done += n;
    }
    writer.reset();
    mixer.setNextReadPosition (savedPos);
    return done >= total;
}

//==============================================================================
void AudioEngine::prepareProcessor (juce::AudioProcessor& p) const
{
    p.setPlayConfigDetails (2, 2, mixer.rate, mixer.preparedBlock);
    p.prepareToPlay (mixer.rate, mixer.preparedBlock);
}

void AudioEngine::addProcessorToTrack (int trackId, std::unique_ptr<juce::AudioProcessor> p)
{
    if (p == nullptr) return;
    prepareProcessor (*p);                       // prepared before it can be hit by the audio thread
    {
        const juce::ScopedLock sl (mixer.lock);
        if (auto* t = findTrack (trackId)) t->chain.push_back (std::move (p));
    }
    // if the track wasn't found, p frees here (outside the lock)
}

void AudioEngine::addNativeEffect (int trackId, int which)
{
    std::unique_ptr<juce::AudioProcessor> p = (which == 0)
        ? std::unique_ptr<juce::AudioProcessor> (std::make_unique<NativeEQ>())
        : std::unique_ptr<juce::AudioProcessor> (std::make_unique<NativeCompressor>());
    addProcessorToTrack (trackId, std::move (p));
}

bool AudioEngine::addHostedPlugin (int trackId, const juce::PluginDescription& desc, juce::String& error)
{
    auto inst = pluginFormats.createPluginInstance (desc, mixer.rate, mixer.preparedBlock, error);
    if (inst == nullptr) return false;
    addProcessorToTrack (trackId, std::move (inst));
    return true;
}

int AudioEngine::trackPluginCount (int trackId)
{
    const juce::ScopedLock sl (mixer.lock);
    if (auto* t = findTrack (trackId)) return (int) t->chain.size();
    return 0;
}

juce::AudioProcessor* AudioEngine::trackPlugin (int trackId, int index)
{
    const juce::ScopedLock sl (mixer.lock);
    if (auto* t = findTrack (trackId))
        if (index >= 0 && index < (int) t->chain.size())
            return t->chain[(size_t) index].get();
    return nullptr;
}

juce::String AudioEngine::trackPluginName (int trackId, int index)
{
    const juce::ScopedLock sl (mixer.lock);
    if (auto* t = findTrack (trackId))
        if (index >= 0 && index < (int) t->chain.size())
            return t->chain[(size_t) index]->getName();
    return {};
}

void AudioEngine::removeTrackPlugin (int trackId, int index)
{
    std::unique_ptr<juce::AudioProcessor> doomed;   // freed outside the lock
    {
        const juce::ScopedLock sl (mixer.lock);
        if (auto* t = findTrack (trackId))
            if (index >= 0 && index < (int) t->chain.size())
            {
                doomed = std::move (t->chain[(size_t) index]);
                t->chain.erase (t->chain.begin() + index);
            }
    }
    if (doomed != nullptr) doomed->releaseResources();
}
