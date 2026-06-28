#include "AudioEngine.h"
#include "FfmpegTool.h"
#include "NativeEffects.h"
#include <cmath>
#include <vector>
#include <SoundTouch.h>

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

// Plugin delay compensation: delay each track so all paths line up at the master.
// path latency = track chain latency + (its destination bus's chain latency). Each track is
// delayed by (globalMax - itsPathLatency). Runs under the lock (exclusive with the audio thread).
void AudioEngine::Mixer::recomputePDC()
{
    const juce::ScopedLock sl (lock);
    const int blk = preparedBlock.load();
    auto chainLat = [] (const std::vector<std::unique_ptr<juce::AudioProcessor>>& chain)
    {
        int L = 0; for (auto& p : chain) if (p != nullptr) L += juce::jmax (0, p->getLatencySamples());
        return L;
    };
    std::vector<int> busLat (buses.size(), 0);
    for (size_t i = 0; i < buses.size(); ++i) busLat[i] = chainLat (buses[i]->chain);

    std::vector<int> pathLat (tracks.size(), 0);
    int globalMax = 0;
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        int L = chainLat (tracks[i]->chain);
        const int o = tracks[i]->output.load();
        if (o >= 0 && o < (int) buses.size()) L += busLat[(size_t) o];
        pathLat[i] = L; globalMax = juce::jmax (globalMax, L);
    }
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        auto* t = tracks[i].get();
        const int cd = juce::jmax (0, globalMax - pathLat[i]);
        const int need = cd + blk + 8;
        for (int ch = 0; ch < 2; ++ch)
        {
            if ((int) t->dlBuf[ch].size() < need) t->dlBuf[ch].assign ((size_t) need, 0.0f);
            else std::fill (t->dlBuf[ch].begin(), t->dlBuf[ch].end(), 0.0f);
        }
        t->dlPos = 0;
        t->compDelay.store (cd);
    }
}

void AudioEngine::Mixer::prepareToPlay (int samplesPerBlock, double sr)
{
    rate = sr;
    preparedBlock = juce::jmax (preparedBlock.load(), juce::jmax (1, samplesPerBlock));   // grow-only
    const int blk = preparedBlock.load();
    ensureScratch (2, blk);                       // allocate outside the lock

    // Device (re)start pauses the audio callback, so preparing under the lock here cannot
    // contend with a live getNextAudioBlock; the lock just guards the chain vector traversal.
    loudness.prepare (sr, 2);

    const juce::ScopedLock sl (lock);
    for (auto& t : tracks)
        for (auto& p : t->chain)
            if (p != nullptr)
            {
                p->setRateAndBufferSizeDetails (sr, blk);
                p->prepareToPlay (sr, blk);
            }
    recomputeLength();
    recomputePDC();
}

void AudioEngine::Mixer::ensureScratch (int channels, int samples)   // non-realtime only
{
    if (scratch.getNumChannels() < channels || scratch.getNumSamples() < samples)
        scratch.setSize (juce::jmax (channels, scratch.getNumChannels()),
                         juce::jmax (samples,  scratch.getNumSamples()), false, true, true);
    if (aux.getNumChannels() < 2 || aux.getNumSamples() < samples)
        aux.setSize (2, juce::jmax (samples, aux.getNumSamples()), false, true, true);
    for (auto& bp : buses)
        if (bp->buf.getNumChannels() < 2 || bp->buf.getNumSamples() < samples)
            bp->buf.setSize (2, juce::jmax (samples, bp->buf.getNumSamples()), false, true, true);
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
    const bool auxActive = ! auxChain.empty() && aux.getNumSamples() >= n;
    if (auxActive) aux.clear (0, n);   // FX/aux bus accumulator for this block
    for (auto& bp : buses) if (bp->buf.getNumSamples() >= n) bp->buf.clear (0, n);   // output-bus accumulators

    const int soloRender = soloRenderId.load();
    for (auto& tptr : tracks)
    {
        auto* t = tptr.get();
        if (soloRender >= 0 && t->id != soloRender) continue;   // stem render: isolate one track (FX+bus+master still apply)
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

                const juce::AudioBuffer<float>* srcBuf = &t->audio;   // a baked stretch buffer plays in place of the source
                juce::int64 srcStart;
                if (c.stretched != nullptr && c.stretched->getNumSamples() > 0)
                { srcBuf = c.stretched.get(); srcStart = (ovStart - clipStart); }            // baked buffer starts at the clip
                else
                { srcStart = (juce::int64) (c.sourceIn * rt) + (ovStart - clipStart); }
                if (srcStart < 0) continue;
                const int sCh = srcBuf->getNumChannels();
                const int sLen = srcBuf->getNumSamples();
                if (sCh <= 0 || sLen <= 0) continue;

                int count = (int) (ovEnd - ovStart);
                if (srcStart + count > sLen) count = (int) (sLen - srcStart);
                if (count <= 0) continue;

                const juce::int64 clipLenS = clipEnd - clipStart;
                const juce::int64 srcAvail = (c.stretched != nullptr) ? (juce::int64) sLen
                                                                      : (sLen - (juce::int64) (c.sourceIn * rt));   // samples actually decoded
                const juce::int64 effLen   = juce::jmax ((juce::int64) 1, juce::jmin (clipLenS, srcAvail));   // fade against the real audio end
                const juce::int64 declick = (juce::int64) (0.005 * rt);                       // min 5 ms declick
                const juce::int64 halfMax = juce::jmax ((juce::int64) 1, effLen / 2);
                const juce::int64 fIn  = juce::jlimit ((juce::int64) 1, halfMax, juce::jmax (declick, (juce::int64) (c.fadeIn  * rt)));
                const juce::int64 fOut = juce::jlimit ((juce::int64) 1, halfMax, juce::jmax (declick, (juce::int64) (c.fadeOut * rt)));
                const float clipGain = juce::Decibels::decibelsToGain (c.gainDb);   // per-clip gain (pre-fader)

                for (int ch = 0; ch < numCh; ++ch)
                {
                    const int sch = juce::jmin (ch, sCh - 1);
                    const float* sp = srcBuf->getReadPointer (sch, (int) srcStart);
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

        // 2b) plugin delay compensation: delay this track's output to align with the latency graph
        if (const int cd = t->compDelay.load(); cd > 0 && (int) t->dlBuf[0].size() > cd && (int) t->dlBuf[1].size() > cd)
        {
            const int sz = (int) t->dlBuf[0].size();
            const int dch = juce::jmin (numCh, 2);
            int p = t->dlPos;
            for (int i = 0; i < n; ++i)
            {
                const int rp = (p - cd + sz) % sz;
                for (int ch = 0; ch < dch; ++ch)
                {
                    float* s = scratch.getWritePointer (ch);
                    const float out = t->dlBuf[ch][(size_t) rp];
                    t->dlBuf[ch][(size_t) p] = s[i];
                    s[i] = out;
                }
                p = (p + 1 >= sz) ? 0 : p + 1;
            }
            t->dlPos = p;
        }

        // 3) sum into the output with channel gain + pan
        if (g > 0.0001f)
        {
            // route the track's output to a bus sub-mix or straight to master
            const int outIdx = t->output.load();
            const bool toBus = (outIdx >= 0 && outIdx < (int) buses.size() && buses[(size_t) outIdx]->buf.getNumSamples() >= n);
            juce::AudioBuffer<float>& dst = toBus ? buses[(size_t) outIdx]->buf : *info.buffer;
            const int dstOff = toBus ? 0 : info.startSample;

            if (numCh >= 2)
            {
                const float p  = juce::jlimit (-1.0f, 1.0f, t->pan.load());
                const float lg = g * (p <= 0.0f ? 1.0f : 1.0f - p);
                const float rg = g * (p >= 0.0f ? 1.0f : 1.0f + p);
                dst.addFrom (0, dstOff, scratch, 0, 0, n, lg);
                dst.addFrom (1, dstOff, scratch, 1, 0, n, rg);
                if (! toBus) for (int ch = 2; ch < numCh; ++ch)   // buses are stereo
                    info.buffer->addFrom (ch, info.startSample, scratch, ch, 0, n, g);
                if (const float snd = t->send.load(); auxActive && snd > 0.0001f)   // post-fader send to the FX bus
                {
                    aux.addFrom (0, 0, scratch, 0, 0, n, lg * snd);
                    aux.addFrom (1, 0, scratch, 1, 0, n, rg * snd);
                }
            }
            else
            {
                dst.addFrom (0, dstOff, scratch, 0, 0, n, g);
            }
        }
    }

    for (auto& bp : buses)   // output buses: FX chain -> fader/mute -> meter -> sum to master
    {
        auto& b = *bp;
        if (b.buf.getNumSamples() < n) continue;
        if (! b.chain.empty())
        {
            float* bch[2] = { b.buf.getWritePointer (0), b.buf.getWritePointer (1) };
            juce::AudioBuffer<float> proxy (bch, 2, n);
            juce::MidiBuffer midi;
            for (auto& p : b.chain) if (p != nullptr) p->processBlock (proxy, midi);
        }
        const float bg = b.mute.load() ? 0.0f : b.gain.load();
        if (! juce::approximatelyEqual (bg, 1.0f)) b.buf.applyGain (0, n, bg);
        float bpk = 0.0f;
        for (int ch = 0; ch < 2; ++ch) { const float* d = b.buf.getReadPointer (ch); for (int i = 0; i < n; ++i) bpk = juce::jmax (bpk, std::abs (d[i])); }
        b.peak.store (bpk, std::memory_order_relaxed);
        const int mc = juce::jmin (numCh, 2);
        for (int ch = 0; ch < mc; ++ch) info.buffer->addFrom (ch, info.startSample, b.buf, ch, 0, n);
    }

    if (auxActive)   // FX/aux bus: run its chain on the summed sends, then return to master
    {
        float* ach[2] = { aux.getWritePointer (0), aux.getWritePointer (1) };
        juce::AudioBuffer<float> proxy (ach, 2, n);
        juce::MidiBuffer midi;
        for (auto& p : auxChain) if (p != nullptr) p->processBlock (proxy, midi);
        const int mc = juce::jmin (numCh, 2);
        for (int ch = 0; ch < mc; ++ch) info.buffer->addFrom (ch, info.startSample, aux, ch, 0, n);
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

    loudness.processBlock (*info.buffer, info.startSample, n);   // BS.1770/R128 on the final master mix

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

    deviceManager.initialiseWithDefaultDevices (2, 2);   // 2 in (for recording) + 2 out
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        if (dev->getCurrentSampleRate() > 0.0)
            deviceRate = dev->getCurrentSampleRate();

    mixer.rate = deviceRate;
    transport.setSource (&mixer);          // in-memory mix: no read-ahead thread needed
    sourcePlayer.setSource (&transport);
    deviceManager.addAudioCallback (&sourcePlayer);   // writes the mix to the output
    deviceManager.addAudioCallback (&recordTap);      // reads the input (after the mix is written)

    addAuxEffect (2);   // pre-load a reverb on the FX bus so a track's Send knob is immediately useful
}

bool AudioEngine::hasAudioInput() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice()) return d->getActiveInputChannels().countNumberOfSetBits() > 0;
    return false;
}
void AudioEngine::startRecording() { recordTap.writePos.store (0); recordTap.active.store (true); }
bool AudioEngine::isRecording() const { return recordTap.active.load(); }
juce::AudioBuffer<float> AudioEngine::stopRecording()
{
    recordTap.active.store (false);
    const int n = recordTap.writePos.load();
    juce::AudioBuffer<float> take (2, juce::jmax (1, n));
    take.clear();
    if (n > 0) for (int ch = 0; ch < 2; ++ch) take.copyFrom (ch, 0, recordTap.buf, ch, 0, n);
    return take;
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeAudioCallback (&recordTap);
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

double AudioEngine::sampleRate() const { return mixer.rate.load() > 0.0 ? mixer.rate.load() : 44100.0; }

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

std::shared_ptr<juce::AudioBuffer<float>> AudioEngine::makeStretchedClip (int trackId, double sourceIn, double srcSeconds, double ratio)
{
    if (ratio <= 0.02 || ratio > 50.0) return nullptr;
    juce::AudioBuffer<float> region;
    double rt = 44100.0;
    {
        const juce::ScopedLock sl (mixer.lock);
        auto* t = findTrack (trackId);
        if (t == nullptr || t->audio.getNumSamples() == 0) return nullptr;
        rt = mixer.rate.load();
        const int start = juce::jlimit (0, t->audio.getNumSamples() - 1, (int) (sourceIn * rt));
        const int len   = juce::jmin (t->audio.getNumSamples() - start, juce::jmax (1, (int) (srcSeconds * rt)));
        region.setSize (2, len);
        for (int ch = 0; ch < 2; ++ch)
            region.copyFrom (ch, 0, t->audio, juce::jmin (ch, t->audio.getNumChannels() - 1), start, len);
    }
    const int inLen = region.getNumSamples();
    if (inLen <= 0) return nullptr;

    soundtouch::SoundTouch st;
    st.setSampleRate ((unsigned int) rt);
    st.setChannels (2);
    st.setTempo (1.0 / ratio);                 // tempo < 1 => longer output, pitch preserved
    st.setSetting (SETTING_USE_AA_FILTER, 1);

    std::vector<float> in ((size_t) inLen * 2);
    for (int i = 0; i < inLen; ++i) { in[(size_t) i * 2] = region.getSample (0, i); in[(size_t) i * 2 + 1] = region.getSample (1, i); }
    st.putSamples (in.data(), (unsigned int) inLen);
    st.flush();

    const int cap = (int) (inLen * ratio) + 16384;
    auto out = std::make_shared<juce::AudioBuffer<float>> (2, cap);
    out->clear();
    std::vector<float> buf (4096 * 2);
    int written = 0;
    for (;;)
    {
        const unsigned int got = st.receiveSamples (buf.data(), 4096);
        if (got == 0) break;
        for (unsigned int i = 0; i < got && written < cap; ++i, ++written)
        { out->setSample (0, written, buf[(size_t) i * 2]); out->setSample (1, written, buf[(size_t) i * 2 + 1]); }
    }
    if (written <= 0) return nullptr;
    out->setSize (2, written, true, true, true);
    return out;
}

std::shared_ptr<juce::AudioBuffer<float>> AudioEngine::makeSpeedFaded (int trackId, double sourceIn, double srcSeconds, double speedInSec, double speedOutSec)
{
    juce::AudioBuffer<float> region;
    double rt = 44100.0;
    {
        const juce::ScopedLock sl (mixer.lock);
        auto* t = findTrack (trackId);
        if (t == nullptr || t->audio.getNumSamples() == 0) return nullptr;
        rt = mixer.rate.load();
        const int start = juce::jlimit (0, t->audio.getNumSamples() - 1, (int) (sourceIn * rt));
        const int len   = juce::jmin (t->audio.getNumSamples() - start, juce::jmax (1, (int) (srcSeconds * rt)));
        region.setSize (2, len);
        for (int ch = 0; ch < 2; ++ch)
            region.copyFrom (ch, 0, t->audio, juce::jmin (ch, t->audio.getNumChannels() - 1), start, len);
    }
    const int N = region.getNumSamples();
    if (N <= 2) return nullptr;

    const double inS  = juce::jlimit (0.0, srcSeconds * 0.9, speedInSec)  * rt;   // head ramp (source samples)
    const double outS = juce::jlimit (0.0, srcSeconds * 0.9, speedOutSec) * rt;   // tail ramp
    const double rMin = 0.35;                                                     // slowest playback rate (tape stop feel)
    auto rateAt = [&] (double sp) -> double
    {
        double r = 1.0;
        if (inS  > 0.0 && sp < inS)         r = juce::jmin (r, rMin + (1.0 - rMin) * (sp / inS));            // spin up
        if (outS > 0.0 && sp > (double) N - outS) r = juce::jmin (r, rMin + (1.0 - rMin) * juce::jlimit (0.0, 1.0, ((double) N - sp) / outS));  // slow down
        return juce::jmax (0.05, r);
    };

    const int cap = (int) ((double) N / rMin) + 8;
    auto out = std::make_shared<juce::AudioBuffer<float>> (2, cap);
    out->clear();
    double sp = 0.0; int w = 0;
    while (sp < (double) (N - 1) && w < cap)
    {
        const int i0 = (int) sp; const int i1 = juce::jmin (N - 1, i0 + 1); const float fr = (float) (sp - i0);
        out->setSample (0, w, region.getSample (0, i0) * (1.0f - fr) + region.getSample (0, i1) * fr);
        out->setSample (1, w, region.getSample (1, i0) * (1.0f - fr) + region.getSample (1, i1) * fr);
        ++w; sp += rateAt (sp);
    }
    if (w <= 0) return nullptr;
    out->setSize (2, w, true, true, true);
    return out;
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

float AudioEngine::getMomentaryLufs()  const  { return mixer.loudness.getMomentaryLufs(); }
float AudioEngine::getShortTermLufs()  const  { return mixer.loudness.getShortTermLufs(); }
float AudioEngine::getIntegratedLufs() const  { return mixer.loudness.getIntegratedLufs(); }
float AudioEngine::getTruePeakDb()     const  { return mixer.loudness.getTruePeakDb(); }
void  AudioEngine::resetLoudness()            { mixer.loudness.reset(); }

bool AudioEngine::measureFileLoudness (juce::AudioFormatManager& fm, const juce::File& wav,
                                       float& integratedLufs, float& truePeakDb)
{
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (wav));
    if (reader == nullptr) return false;
    LoudnessMeter meter;
    meter.prepare (reader->sampleRate, 2);
    const int block = 8192;
    juce::AudioBuffer<float> buf (2, block);
    juce::int64 left = (juce::int64) reader->lengthInSamples, posn = 0;
    while (left > 0)
    {
        const int n = (int) juce::jmin ((juce::int64) block, left);
        buf.clear();
        reader->read (&buf, 0, n, posn, true, reader->numChannels > 1);
        if (reader->numChannels == 1) buf.copyFrom (1, 0, buf, 0, 0, n);   // mono -> dual mono
        meter.processBlock (buf, 0, n);
        posn += n; left -= n;
    }
    integratedLufs = meter.getIntegratedLufs();
    truePeakDb     = meter.getTruePeakDb();
    return true;
}

bool AudioEngine::applyGainToWav (juce::AudioFormatManager& fm, const juce::File& wav, float gainDb)
{
    if (std::abs (gainDb) < 0.01f) return true;
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (wav));
    if (reader == nullptr) return false;
    const double sr = reader->sampleRate;
    const int ch = (int) juce::jmin ((unsigned) 2, reader->numChannels);
    const int bits = (int) juce::jmax ((unsigned) 16, reader->bitsPerSample);
    juce::AudioBuffer<float> all ((int) juce::jmax (1u, reader->numChannels), (int) reader->lengthInSamples);
    reader->read (&all, 0, (int) reader->lengthInSamples, 0, true, true);
    reader.reset();
    all.applyGain (juce::Decibels::decibelsToGain (gainDb));

    juce::File tmp = wav.getSiblingFile (wav.getFileNameWithoutExtension() + "_norm.wav");
    if (auto* fmt = fm.findFormatForFileExtension ("wav"))
    {
        std::unique_ptr<juce::FileOutputStream> os (tmp.createOutputStream());
        if (os == nullptr) return false;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            fmt->createWriterFor (os.get(), sr, (unsigned) ch, bits, {}, 0));
        if (writer == nullptr) return false;
        os.release();
        writer->writeFromAudioSampleBuffer (all, 0, all.getNumSamples());
        writer.reset();
    }
    return tmp.existsAsFile() && tmp.moveFileTo (wav);
}

bool AudioEngine::normalizeFileToLufs (const juce::File& wav, float targetLufs, float ceilingTpDb, juce::String& report)
{
    float integ = LoudnessMeter::kSilence, tp = -200.0f;
    if (! measureFileLoudness (formatManager, wav, integ, tp)) return false;
    if (integ <= LoudnessMeter::kSilence) { report = "silent (no gain applied)"; return false; }
    float gain = targetLufs - integ;
    if (tp + gain > ceilingTpDb) gain = ceilingTpDb - tp;   // never push true peak past the ceiling
    if (! applyGainToWav (formatManager, wav, gain)) return false;
    report = juce::String (integ + gain, 1) + " LUFS, peak " + juce::String (tp + gain, 1) + " dBTP";
    return true;
}

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

bool AudioEngine::renderTrackStem (int trackId, const juce::File& outWav, double lengthSeconds)
{
    mixer.soloRenderId.store (trackId);          // isolate this track for the render
    const bool ok = renderMixToFile (outWav, lengthSeconds);
    mixer.soloRenderId.store (-1);
    return ok;
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
    mixer.recomputePDC();
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

static std::unique_ptr<juce::AudioProcessor> makeNativeEffect (int which)
{
    switch (which)
    {
        case 1:  return std::make_unique<NativeCompressor>();
        case 2:  return std::make_unique<NativeReverb>();
        case 3:  return std::make_unique<NativeDelay>();
        case 4:  return std::make_unique<NativeLimiter>();
        case 5:  return std::make_unique<NativeGate>();
        default: return std::make_unique<NativeEQ>();
    }
}

void AudioEngine::setTrackSend (int trackId, float level)
{
    const juce::ScopedLock sl (mixer.lock);
    if (auto* t = findTrack (trackId)) t->send.store (juce::jlimit (0.0f, 1.0f, level));
}
void AudioEngine::addAuxEffect (int which)
{
    auto p = makeNativeEffect (which);
    prepareProcessor (*p);
    const juce::ScopedLock sl (mixer.lock);
    mixer.auxChain.push_back (std::move (p));
}
int AudioEngine::auxPluginCount() { const juce::ScopedLock sl (mixer.lock); return (int) mixer.auxChain.size(); }
juce::AudioProcessor* AudioEngine::auxPlugin (int index)
{ const juce::ScopedLock sl (mixer.lock); return (index >= 0 && index < (int) mixer.auxChain.size()) ? mixer.auxChain[(size_t) index].get() : nullptr; }
juce::String AudioEngine::auxPluginName (int index)
{ const juce::ScopedLock sl (mixer.lock); return (index >= 0 && index < (int) mixer.auxChain.size()) ? mixer.auxChain[(size_t) index]->getName() : juce::String(); }
void AudioEngine::removeAuxPlugin (int index)
{
    std::unique_ptr<juce::AudioProcessor> doomed;   // freed outside the lock
    { const juce::ScopedLock sl (mixer.lock);
      if (index >= 0 && index < (int) mixer.auxChain.size()) { doomed = std::move (mixer.auxChain[(size_t) index]); mixer.auxChain.erase (mixer.auxChain.begin() + index); } }
}

//== Output buses ==
int AudioEngine::addBus (const juce::String& name)
{
    auto b = std::make_unique<Mixer::Bus>();
    b->name = name;
    b->buf.setSize (2, juce::jmax (512, mixer.preparedBlock.load()), false, true, true);
    const juce::ScopedLock sl (mixer.lock);
    mixer.buses.push_back (std::move (b));
    return (int) mixer.buses.size() - 1;
}
int  AudioEngine::busCount() { const juce::ScopedLock sl (mixer.lock); return (int) mixer.buses.size(); }
juce::String AudioEngine::busName (int idx)
{ const juce::ScopedLock sl (mixer.lock); return (idx >= 0 && idx < (int) mixer.buses.size()) ? mixer.buses[(size_t) idx]->name : juce::String(); }
void AudioEngine::setTrackOutput (int trackId, int out)
{ { const juce::ScopedLock sl (mixer.lock); if (auto* t = findTrack (trackId)) t->output.store (out); } mixer.recomputePDC(); }
int  AudioEngine::getTrackOutput (int trackId)
{ const juce::ScopedLock sl (mixer.lock); if (auto* t = findTrack (trackId)) return t->output.load(); return -1; }
void  AudioEngine::setBusGain (int idx, float g) { const juce::ScopedLock sl (mixer.lock); if (idx >= 0 && idx < (int) mixer.buses.size()) mixer.buses[(size_t) idx]->gain.store (juce::jmax (0.0f, g)); }
float AudioEngine::getBusGain (int idx) { const juce::ScopedLock sl (mixer.lock); return (idx >= 0 && idx < (int) mixer.buses.size()) ? mixer.buses[(size_t) idx]->gain.load() : 1.0f; }
void  AudioEngine::setBusMute (int idx, bool m) { const juce::ScopedLock sl (mixer.lock); if (idx >= 0 && idx < (int) mixer.buses.size()) mixer.buses[(size_t) idx]->mute.store (m); }
bool  AudioEngine::getBusMute (int idx) { const juce::ScopedLock sl (mixer.lock); return (idx >= 0 && idx < (int) mixer.buses.size()) && mixer.buses[(size_t) idx]->mute.load(); }
float AudioEngine::getBusPeak (int idx) { const juce::ScopedLock sl (mixer.lock); return (idx >= 0 && idx < (int) mixer.buses.size()) ? mixer.buses[(size_t) idx]->peak.load() : 0.0f; }
void AudioEngine::addBusEffect (int idx, int which)
{
    auto p = makeNativeEffect (which);
    prepareProcessor (*p);
    { const juce::ScopedLock sl (mixer.lock);
      if (idx >= 0 && idx < (int) mixer.buses.size()) mixer.buses[(size_t) idx]->chain.push_back (std::move (p)); }
    mixer.recomputePDC();
}
int AudioEngine::busPluginCount (int idx) { const juce::ScopedLock sl (mixer.lock); return (idx >= 0 && idx < (int) mixer.buses.size()) ? (int) mixer.buses[(size_t) idx]->chain.size() : 0; }
juce::AudioProcessor* AudioEngine::busPlugin (int idx, int index)
{ const juce::ScopedLock sl (mixer.lock); if (idx >= 0 && idx < (int) mixer.buses.size()) { auto& c = mixer.buses[(size_t) idx]->chain; if (index >= 0 && index < (int) c.size()) return c[(size_t) index].get(); } return nullptr; }
juce::String AudioEngine::busPluginName (int idx, int index)
{ const juce::ScopedLock sl (mixer.lock); if (idx >= 0 && idx < (int) mixer.buses.size()) { auto& c = mixer.buses[(size_t) idx]->chain; if (index >= 0 && index < (int) c.size()) return c[(size_t) index]->getName(); } return {}; }
void AudioEngine::removeBusPlugin (int idx, int index)
{
    std::unique_ptr<juce::AudioProcessor> doomed;
    { const juce::ScopedLock sl (mixer.lock);
      if (idx >= 0 && idx < (int) mixer.buses.size()) { auto& c = mixer.buses[(size_t) idx]->chain; if (index >= 0 && index < (int) c.size()) { doomed = std::move (c[(size_t) index]); c.erase (c.begin() + index); } } }
    mixer.recomputePDC();
}
void AudioEngine::removeBus (int idx)
{
    std::unique_ptr<Mixer::Bus> doomed;
    { const juce::ScopedLock sl (mixer.lock);
      if (idx < 0 || idx >= (int) mixer.buses.size()) return;
      doomed = std::move (mixer.buses[(size_t) idx]);
      mixer.buses.erase (mixer.buses.begin() + idx);
      for (auto& t : mixer.tracks)   // re-point routings
      { const int o = t->output.load(); if (o == idx) t->output.store (-1); else if (o > idx) t->output.store (o - 1); }
    }
    mixer.recomputePDC();
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

juce::var AudioEngine::saveBusFx (int b)
{
    juce::var arr;
    for (int i = 0, n = busPluginCount (b); i < n; ++i)
    {
        auto* proc = busPlugin (b, i); if (proc == nullptr) continue;
        int which = -1;
        if      (dynamic_cast<NativeEQ*> (proc))         which = 0;
        else if (dynamic_cast<NativeCompressor*> (proc)) which = 1;
        else if (dynamic_cast<NativeReverb*> (proc))     which = 2;
        else if (dynamic_cast<NativeDelay*> (proc))      which = 3;
        else if (dynamic_cast<NativeLimiter*> (proc))    which = 4;
        else if (dynamic_cast<NativeGate*> (proc))       which = 5;
        else continue;
        auto* o = new juce::DynamicObject();
        o->setProperty ("which", which);
        juce::MemoryBlock mb; proc->getStateInformation (mb); o->setProperty ("state", mb.toBase64Encoding());
        arr.append (juce::var (o));
    }
    return arr;
}
void AudioEngine::restoreBusFx (int b, const juce::var& arr)
{
    auto* a = arr.getArray(); if (a == nullptr) return;
    for (auto& fv : *a)
    {
        addBusEffect (b, (int) fv.getProperty ("which", 0));
        if (auto* proc = busPlugin (b, busPluginCount (b) - 1))
        { juce::MemoryBlock mb; mb.fromBase64Encoding (fv.getProperty ("state", "").toString()); if (mb.getSize() > 0) proc->setStateInformation (mb.getData(), (int) mb.getSize()); }
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
    mixer.recomputePDC();
    if (doomed != nullptr) doomed->releaseResources();
}
