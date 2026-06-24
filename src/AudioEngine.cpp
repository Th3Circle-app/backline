#include "AudioEngine.h"
#include "FfmpegTool.h"
#include "NativeEffects.h"
#include <cmath>

// Linear-interpolated value of a (time, gain) envelope at time t (clamped to the ends).
static inline float envelopeAt (const std::vector<std::pair<double, float>>& pts, double t) noexcept
{
    if (pts.empty())               return 1.0f;
    if (t <= pts.front().first)    return pts.front().second;
    if (t >= pts.back().first)     return pts.back().second;
    for (size_t i = 1; i < pts.size(); ++i)
        if (t <= pts[i].first)
        {
            const double t0 = pts[i - 1].first, t1 = pts[i].first;
            const float  v0 = pts[i - 1].second, v1 = pts[i].second;
            const float  a  = (float) ((t - t0) / juce::jmax (1.0e-9, t1 - t0));
            return v0 + (v1 - v0) * a;
        }
    return pts.back().second;
}

// Fade curve shapes applied to the normalized 0..1 fade fraction.
static inline float fadeShape (float x, int s) noexcept
{
    x = juce::jlimit (0.0f, 1.0f, x);
    switch (s)
    {
        case 1:  return x * x;                          // exponential (slow start)
        case 2:  return x * x * (3.0f - 2.0f * x);      // s-curve (bell)
        case 3:  return std::sqrt (x);                  // logarithmic (fast start)
        default: return x;                              // linear
    }
}

//==============================================================================
void AudioEngine::Mixer::recomputeLength()
{
    const double rt = rate.load();
    juce::int64 len = (juce::int64) (minLengthSeconds * rt);
    for (auto& t : tracks)
        for (auto& c : t->clips)
            len = juce::jmax (len, (juce::int64) (c.timelineEnd() * rt));
    totalLen.store (len, std::memory_order_relaxed);
}

void AudioEngine::Mixer::prepareToPlay (int samplesPerBlock, double sr)
{
    rate = sr;
    preparedBlock = juce::jmax (preparedBlock.load(), juce::jmax (1, samplesPerBlock));   // grow-only
    const int blk = preparedBlock.load();
    ensureScratch (2, blk);                       // allocate outside the lock

    // Device (re)start pauses the audio callback, so preparing under the lock here cannot
    // contend with a live getNextAudioBlock; the lock just guards the chain vector traversal.
    const juce::ScopedLock sl (lock);
    for (auto& t : tracks)
        for (auto& p : t->chain)
            if (p != nullptr)
            {
                p->setRateAndBufferSizeDetails (sr, blk);
                p->prepareToPlay (sr, blk);
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
    const double rt = rate.load();
    if (numCh <= 0 || n <= 0 || n > scratch.getNumSamples()) { pos.store (blockStart + n, std::memory_order_relaxed); return; }

    for (auto& tptr : tracks)
    {
        auto* t = tptr.get();
        float g = t->gain.load();
        if (t->autoOn.load() && ! t->autoEnv.empty())               // read the volume envelope at this block's start
            g = envelopeAt (t->autoEnv, (double) blockStart / rt);
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
                const juce::int64 clipStart = (juce::int64) (c.timelineStart * rt);
                const juce::int64 clipEnd   = (juce::int64) (c.timelineEnd()  * rt);
                const juce::int64 ovStart = juce::jmax (clipStart, blockStart);
                const juce::int64 ovEnd   = juce::jmin (clipEnd, blockStart + n);
                if (ovEnd <= ovStart) continue;

                const int dest = (int) (ovStart - blockStart);
                const juce::int64 srcStart = (juce::int64) (c.sourceIn * rt) + (ovStart - clipStart);
                if (srcStart < 0) continue;

                int count = (int) (ovEnd - ovStart);
                if (srcStart + count > tLen) count = (int) (tLen - srcStart);
                if (count <= 0) continue;

                const juce::int64 clipLenS = clipEnd - clipStart;
                const juce::int64 srcAvail = tLen - (juce::int64) (c.sourceIn * rt);          // samples actually decoded
                const juce::int64 effLen   = juce::jmax ((juce::int64) 1, juce::jmin (clipLenS, srcAvail));   // fade against the real audio end
                const juce::int64 declick = (juce::int64) (0.005 * rt);                       // min 5 ms declick
                const juce::int64 halfMax = juce::jmax ((juce::int64) 1, effLen / 2);
                const juce::int64 fIn  = juce::jlimit ((juce::int64) 1, halfMax, juce::jmax (declick, (juce::int64) (c.fadeIn  * rt)));
                const juce::int64 fOut = juce::jlimit ((juce::int64) 1, halfMax, juce::jmax (declick, (juce::int64) (c.fadeOut * rt)));
                const float clipGain = juce::Decibels::decibelsToGain (c.gainDb);   // per-clip gain (pre-fader)

                for (int ch = 0; ch < numCh; ++ch)
                {
                    const int sch = juce::jmin (ch, tCh - 1);
                    const float* sp = t->audio.getReadPointer (sch, (int) srcStart);
                    float* dp = scratch.getWritePointer (ch, dest);
                    for (int i = 0; i < count; ++i)
                    {
                        const juce::int64 cl = (ovStart - clipStart) + i;
                        float fdf = 1.0f;
                        if (cl < fIn)                fdf = fadeShape ((float) cl / (float) fIn, c.fadeInShape);
                        else if (cl > effLen - fOut) fdf = fadeShape ((float) (effLen - cl) / (float) fOut, c.fadeOutShape);
                        fdf = juce::jlimit (0.0f, 1.0f, fdf);
                        dp[i] += sp[i] * fdf * clipGain;
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

        // post-fader peak for the channel meter
        float pk = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* d = scratch.getReadPointer (ch);
            for (int i = 0; i < n; ++i) pk = juce::jmax (pk, std::abs (d[i]));
        }
        t->peak.store (pk * g, std::memory_order_relaxed);

        // 3) sum into the output with channel gain + pan
        if (g > 0.0001f)
        {
            if (numCh >= 2)
            {
                const float p  = juce::jlimit (-1.0f, 1.0f, t->pan.load());
                const float lg = g * (p <= 0.0f ? 1.0f : 1.0f - p);
                const float rg = g * (p >= 0.0f ? 1.0f : 1.0f + p);
                info.buffer->addFrom (0, info.startSample, scratch, 0, 0, n, lg);
                info.buffer->addFrom (1, info.startSample, scratch, 1, 0, n, rg);
                for (int ch = 2; ch < numCh; ++ch)
                    info.buffer->addFrom (ch, info.startSample, scratch, ch, 0, n, g);
            }
            else
            {
                info.buffer->addFrom (0, info.startSample, scratch, 0, 0, n, g);
            }
        }
    }

    const float mg = masterMute.load() ? 0.0f : masterGain.load();   // master fader (0 when muted)
    if (! juce::approximatelyEqual (mg, 1.0f))
        info.buffer->applyGain (info.startSample, n, mg);

    float mpk = 0.0f;                                   // master meter
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* d = info.buffer->getReadPointer (ch, info.startSample);
        for (int i = 0; i < n; ++i) mpk = juce::jmax (mpk, std::abs (d[i]));
    }
    masterPeak.store (mpk, std::memory_order_relaxed);

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

float AudioEngine::clipPeak (int trackId, double sourceIn, double duration)
{
    const juce::ScopedLock sl (mixer.lock);
    auto* t = findTrack (trackId);
    if (t == nullptr || t->audio.getNumSamples() == 0) return 0.0f;
    const double rt = mixer.rate.load();
    const int start = juce::jlimit (0, t->audio.getNumSamples() - 1, (int) (sourceIn * rt));
    const int len   = juce::jmin (t->audio.getNumSamples() - start, (int) (duration * rt));
    if (len <= 0) return 0.0f;
    float pk = 0.0f;
    for (int ch = 0; ch < t->audio.getNumChannels(); ++ch)
        pk = juce::jmax (pk, t->audio.getMagnitude (ch, start, len));
    return pk;
}

void AudioEngine::setTrackAutomation (int trackId, const std::vector<std::pair<double, float>>& env, bool on)
{
    std::vector<std::pair<double, float>> staged (env);   // copy outside the audio lock
    {
        const juce::ScopedLock sl (mixer.lock);
        if (auto* t = findTrack (trackId)) { std::swap (t->autoEnv, staged); t->autoOn.store (on); }
    }
}

void AudioEngine::setTrackGain (int trackId, float gain)
{
    const juce::ScopedLock sl (mixer.lock);
    if (auto* t = findTrack (trackId)) t->gain.store (gain);
}

void AudioEngine::setTrackPan (int trackId, float pan)
{
    const juce::ScopedLock sl (mixer.lock);
    if (auto* t = findTrack (trackId)) t->pan.store (juce::jlimit (-1.0f, 1.0f, pan));
}

void  AudioEngine::setMasterGain (float g)    { mixer.masterGain.store (juce::jmax (0.0f, g)); }
float AudioEngine::getMasterGain() const      { return mixer.masterGain.load(); }
void  AudioEngine::setMasterMute (bool m)     { mixer.masterMute.store (m); }
bool  AudioEngine::getMasterMute() const      { return mixer.masterMute.load(); }
void  AudioEngine::setExternalPeak (float v)  { mixer.externalPeak.store (juce::jmax (0.0f, v)); }
float AudioEngine::getMasterPeak() const      { return juce::jmax (mixer.masterPeak.load(), mixer.externalPeak.load()); }

float AudioEngine::getTrackPeak (int trackId)
{
    if (! mixer.lock.tryEnter()) return -1.0f;   // never block the message thread (meters) behind a slow plugin
    float v = 0.0f;
    if (auto* t = findTrack (trackId)) v = t->peak.load();
    mixer.lock.exit();
    return v;
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

    const int block = juce::jmax (1, mixer.preparedBlock.load());   // == scratch + plugins' prepared block (no silent export on tiny buffers)
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
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)            // safety: never write past 0 dBFS on bounce
            juce::FloatVectorOperations::clip (buf.getWritePointer (ch), buf.getReadPointer (ch), -1.0f, 1.0f, n);
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
    std::unique_ptr<juce::AudioProcessor> p;
    switch (which)
    {
        case 1:  p = std::make_unique<NativeCompressor>(); break;
        case 2:  p = std::make_unique<NativeReverb>();     break;
        case 3:  p = std::make_unique<NativeDelay>();      break;
        case 4:  p = std::make_unique<NativeLimiter>();    break;
        case 5:  p = std::make_unique<NativeGate>();       break;
        default: p = std::make_unique<NativeEQ>();         break;
    }
    addProcessorToTrack (trackId, std::move (p));
}

bool AudioEngine::addHostedPlugin (int trackId, const juce::PluginDescription& desc, juce::String& error)
{
    auto inst = pluginFormats.createPluginInstance (desc, mixer.rate, mixer.preparedBlock, error);
    if (inst == nullptr) return false;
    addProcessorToTrack (trackId, std::move (inst));
    return true;
}

void AudioEngine::addHostedPluginAsync (int trackId, const juce::PluginDescription& desc,
                                        std::function<void (bool, juce::String)> done)
{
    pluginFormats.createPluginInstanceAsync (desc, mixer.rate, mixer.preparedBlock,
        [this, trackId, done] (std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err)
        {
            if (inst != nullptr) { addProcessorToTrack (trackId, std::move (inst)); if (done) done (true, {}); }
            else                 { if (done) done (false, err); }
        });
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

juce::var AudioEngine::saveTrackFx (int trackId)
{
    juce::var arr;
    const int n = trackPluginCount (trackId);
    for (int i = 0; i < n; ++i)
    {
        auto* proc = trackPlugin (trackId, i);
        if (proc == nullptr) continue;

        int which = -1; juce::String descXml;
        if      (dynamic_cast<NativeEQ*> (proc))         which = 0;
        else if (dynamic_cast<NativeCompressor*> (proc)) which = 1;
        else if (dynamic_cast<NativeReverb*> (proc))     which = 2;
        else if (dynamic_cast<NativeDelay*> (proc))      which = 3;
        else if (dynamic_cast<NativeLimiter*> (proc))    which = 4;
        else if (dynamic_cast<NativeGate*> (proc))       which = 5;
        else if (auto* inst = dynamic_cast<juce::AudioPluginInstance*> (proc))
        { if (auto xml = inst->getPluginDescription().createXml()) descXml = xml->toString(); }
        else continue;   // unknown processor type -> skip

        auto* o = new juce::DynamicObject();
        if (which >= 0) { o->setProperty ("kind", "native"); o->setProperty ("which", which); }
        else            { o->setProperty ("kind", "plugin"); o->setProperty ("desc", descXml); }
        juce::MemoryBlock mb; proc->getStateInformation (mb);
        o->setProperty ("state", mb.toBase64Encoding());
        arr.append (juce::var (o));
    }
    return arr;
}

void AudioEngine::restoreTrackFx (int trackId, const juce::var& arr)
{
    auto* a = arr.getArray();
    if (a == nullptr) return;
    for (auto& fv : *a)
    {
        const juce::String kind = fv.getProperty ("kind", "").toString();
        if (kind == "native")
        {
            addNativeEffect (trackId, (int) fv.getProperty ("which", 0));
        }
        else if (kind == "plugin")
        {
            juce::PluginDescription desc;
            if (auto xml = juce::parseXML (fv.getProperty ("desc", "").toString())) desc.loadFromXml (*xml);
            juce::String err;
            if (! addHostedPlugin (trackId, desc, err)) continue;   // plugin not installed here -> skip gracefully
        }
        else continue;

        if (auto* proc = trackPlugin (trackId, trackPluginCount (trackId) - 1))   // restore state into the one just added
        {
            juce::MemoryBlock mb; mb.fromBase64Encoding (fv.getProperty ("state", "").toString());
            if (mb.getSize() > 0) proc->setStateInformation (mb.getData(), (int) mb.getSize());
        }
    }
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
