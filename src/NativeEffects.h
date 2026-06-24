#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
/** Built-in effects that are always available (no scanning), so every skin ships
    with at least an EQ and a compressor like a real DAW. Each exposes parameters,
    so JUCE's GenericAudioProcessorEditor gives a working knob/slider UI for free. */
class SimpleEffect : public juce::AudioProcessor
{
public:
    SimpleEffect()
        : juce::AudioProcessor (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}

    bool acceptsMidi() const override                          { return false; }
    bool producesMidi() const override                         { return false; }
    double getTailLengthSeconds() const override               { return 0.0; }
    int getNumPrograms() override                              { return 1; }
    int getCurrentProgram() override                           { return 0; }
    void setCurrentProgram (int) override                      {}
    const juce::String getProgramName (int) override           { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    bool hasEditor() const override                            { return true; }
    juce::AudioProcessorEditor* createEditor() override        { return new juce::GenericAudioProcessorEditor (*this); }
    void getStateInformation (juce::MemoryBlock& dest) override   // serialize normalized params, in add order
    {
        juce::MemoryOutputStream os (dest, false);
        for (auto* p : getParameters()) os.writeFloat (p->getValue());
    }
    void setStateInformation (const void* data, int size) override
    {
        juce::MemoryInputStream is (data, (size_t) size, false);
        for (auto* p : getParameters()) if (! is.isExhausted()) p->setValueNotifyingHost (is.readFloat());
    }
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    { return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
          && l.getMainInputChannelSet()  == juce::AudioChannelSet::stereo(); }
};

//==============================================================================
/** 3-band EQ: low shelf (120 Hz), sweepable mid peak, high shelf (8 kHz). */
class NativeEQ : public SimpleEffect
{
public:
    NativeEQ()
    {
        addParameter (lowGain  = new juce::AudioParameterFloat ({ "low",  1 }, "Low (dB)",   -18.0f, 18.0f, 0.0f));
        addParameter (midFreq  = new juce::AudioParameterFloat ({ "midf", 1 }, "Mid Freq",
                          juce::NormalisableRange<float> (200.0f, 6000.0f, 1.0f, 0.3f), 1000.0f));
        addParameter (midGain  = new juce::AudioParameterFloat ({ "mid",  1 }, "Mid (dB)",   -18.0f, 18.0f, 0.0f));
        addParameter (highGain = new juce::AudioParameterFloat ({ "high", 1 }, "High (dB)",  -18.0f, 18.0f, 0.0f));
    }

    const juce::String getName() const override { return "EQ (Layback)"; }

    void prepareToPlay (double sr, int spb) override
    {
        sampleRate = sr;
        juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, spb), 2 };
        chain.prepare (spec);
        chain.reset();
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        using C = juce::dsp::IIR::Coefficients<float>;
        chain.get<0>().state = C::makeLowShelf  (sampleRate, 120.0,  0.7f, juce::Decibels::decibelsToGain (lowGain->get()));
        chain.get<1>().state = C::makePeakFilter (sampleRate, (double) midFreq->get(), 0.8f, juce::Decibels::decibelsToGain (midGain->get()));
        chain.get<2>().state = C::makeHighShelf (sampleRate, 8000.0, 0.7f, juce::Decibels::decibelsToGain (highGain->get()));

        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        chain.process (ctx);
    }

private:
    using Filter = juce::dsp::IIR::Filter<float>;
    using Dup    = juce::dsp::ProcessorDuplicator<Filter, juce::dsp::IIR::Coefficients<float>>;
    juce::dsp::ProcessorChain<Dup, Dup, Dup> chain;
    double sampleRate = 44100.0;
    juce::AudioParameterFloat* lowGain  = nullptr;
    juce::AudioParameterFloat* midFreq  = nullptr;
    juce::AudioParameterFloat* midGain  = nullptr;
    juce::AudioParameterFloat* highGain = nullptr;
};

//==============================================================================
/** Compressor with threshold / ratio / attack / release / makeup, to balance a
    track against the others. */
class NativeCompressor : public SimpleEffect
{
public:
    NativeCompressor()
    {
        addParameter (threshold = new juce::AudioParameterFloat ({ "thr", 1 }, "Threshold (dB)", -48.0f, 0.0f,  -18.0f));
        addParameter (ratio     = new juce::AudioParameterFloat ({ "rat", 1 }, "Ratio",
                          juce::NormalisableRange<float> (1.0f, 20.0f, 0.1f, 0.4f), 3.0f));
        addParameter (attack    = new juce::AudioParameterFloat ({ "atk", 1 }, "Attack (ms)",
                          juce::NormalisableRange<float> (0.1f, 200.0f, 0.1f, 0.4f), 15.0f));
        addParameter (release   = new juce::AudioParameterFloat ({ "rel", 1 }, "Release (ms)",
                          juce::NormalisableRange<float> (10.0f, 1000.0f, 1.0f, 0.4f), 150.0f));
        addParameter (makeup    = new juce::AudioParameterFloat ({ "mk",  1 }, "Makeup (dB)",  0.0f, 24.0f, 0.0f));
    }

    const juce::String getName() const override { return "Compressor (Layback)"; }

    void prepareToPlay (double sr, int spb) override
    {
        juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, spb), 2 };
        comp.prepare (spec);
        comp.reset();
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        comp.setThreshold (threshold->get());
        comp.setRatio     (juce::jmax (1.0f, ratio->get()));
        comp.setAttack    (attack->get());
        comp.setRelease   (release->get());

        juce::dsp::AudioBlock<float> block (buffer);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        comp.process (ctx);

        if (const float mk = makeup->get(); mk > 0.001f)
            buffer.applyGain (juce::Decibels::decibelsToGain (mk));
    }

private:
    juce::dsp::Compressor<float> comp;
    juce::AudioParameterFloat* threshold = nullptr;
    juce::AudioParameterFloat* ratio     = nullptr;
    juce::AudioParameterFloat* attack    = nullptr;
    juce::AudioParameterFloat* release   = nullptr;
    juce::AudioParameterFloat* makeup    = nullptr;
};

//==============================================================================
/** Reverb send-style ambience. */
class NativeReverb : public SimpleEffect
{
public:
    NativeReverb()
    {
        addParameter (mix   = new juce::AudioParameterFloat ({ "mix",  1 }, "Mix",       0.0f, 1.0f, 0.25f));
        addParameter (size  = new juce::AudioParameterFloat ({ "size", 1 }, "Room Size", 0.0f, 1.0f, 0.5f));
        addParameter (damp  = new juce::AudioParameterFloat ({ "damp", 1 }, "Damping",   0.0f, 1.0f, 0.5f));
        addParameter (width = new juce::AudioParameterFloat ({ "wide", 1 }, "Width",     0.0f, 1.0f, 1.0f));
    }
    const juce::String getName() const override { return "Reverb (Layback)"; }
    void prepareToPlay (double sr, int spb) override { juce::dsp::ProcessSpec s { sr, (juce::uint32) juce::jmax (1, spb), 2 }; reverb.prepare (s); reverb.reset(); }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::Reverb::Parameters p;
        p.wetLevel = mix->get(); p.dryLevel = 1.0f - mix->get() * 0.5f;
        p.roomSize = size->get(); p.damping = damp->get(); p.width = width->get();
        reverb.setParameters (p);
        juce::dsp::AudioBlock<float> blk (b); juce::dsp::ProcessContextReplacing<float> ctx (blk); reverb.process (ctx);
    }
private:
    juce::dsp::Reverb reverb;
    juce::AudioParameterFloat *mix = nullptr, *size = nullptr, *damp = nullptr, *width = nullptr;
};

//==============================================================================
/** Stereo feedback delay. */
class NativeDelay : public SimpleEffect
{
public:
    NativeDelay()
    {
        addParameter (timeMs = new juce::AudioParameterFloat ({ "time", 1 }, "Time (ms)", juce::NormalisableRange<float> (10.0f, 1500.0f, 1.0f, 0.4f), 350.0f));
        addParameter (fb     = new juce::AudioParameterFloat ({ "fb",   1 }, "Feedback",  0.0f, 0.95f, 0.35f));
        addParameter (mix    = new juce::AudioParameterFloat ({ "mix",  1 }, "Mix",       0.0f, 1.0f, 0.3f));
    }
    const juce::String getName() const override { return "Delay (Layback)"; }
    void prepareToPlay (double sr, int spb) override
    {
        sampleRate = sr;
        juce::dsp::ProcessSpec s { sr, (juce::uint32) juce::jmax (1, spb), 2 };
        delay.prepare (s); delay.setMaximumDelayInSamples ((int) (sr * 1.6) + 4); delay.reset();
    }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        delay.setDelay (juce::jlimit (1.0f, (float) (sampleRate * 1.5), (float) (timeMs->get() * 0.001 * sampleRate)));
        const float f = fb->get(), m = mix->get();
        const int n = b.getNumSamples(), ch = juce::jmin (2, b.getNumChannels());
        for (int c = 0; c < ch; ++c)
        {
            auto* x = b.getWritePointer (c);
            for (int i = 0; i < n; ++i)
            {
                const float in = x[i];
                const float dl = delay.popSample (c);
                delay.pushSample (c, in + dl * f);
                x[i] = in * (1.0f - m) + dl * m;
            }
        }
    }
private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delay { 96000 };
    double sampleRate = 44100.0;
    juce::AudioParameterFloat *timeMs = nullptr, *fb = nullptr, *mix = nullptr;
};

//==============================================================================
/** Brickwall-ish limiter for the master / loud cues. */
class NativeLimiter : public SimpleEffect
{
public:
    NativeLimiter()
    {
        addParameter (thr = new juce::AudioParameterFloat ({ "thr", 1 }, "Threshold (dB)", -24.0f, 0.0f, -1.0f));
        addParameter (rel = new juce::AudioParameterFloat ({ "rel", 1 }, "Release (ms)", juce::NormalisableRange<float> (1.0f, 500.0f, 1.0f, 0.4f), 100.0f));
    }
    const juce::String getName() const override { return "Limiter (Layback)"; }
    void prepareToPlay (double sr, int spb) override { juce::dsp::ProcessSpec s { sr, (juce::uint32) juce::jmax (1, spb), 2 }; lim.prepare (s); lim.reset(); }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        lim.setThreshold (thr->get()); lim.setRelease (rel->get());
        juce::dsp::AudioBlock<float> blk (b); juce::dsp::ProcessContextReplacing<float> ctx (blk); lim.process (ctx);
    }
private:
    juce::dsp::Limiter<float> lim;
    juce::AudioParameterFloat *thr = nullptr, *rel = nullptr;
};

//==============================================================================
/** Noise gate to clean up quiet tails / bleed. */
class NativeGate : public SimpleEffect
{
public:
    NativeGate()
    {
        addParameter (thr = new juce::AudioParameterFloat ({ "thr", 1 }, "Threshold (dB)", -80.0f, 0.0f, -45.0f));
        addParameter (rat = new juce::AudioParameterFloat ({ "rat", 1 }, "Ratio", juce::NormalisableRange<float> (1.0f, 20.0f, 0.1f, 0.4f), 10.0f));
        addParameter (atk = new juce::AudioParameterFloat ({ "atk", 1 }, "Attack (ms)",  juce::NormalisableRange<float> (0.1f, 100.0f, 0.1f, 0.4f), 1.0f));
        addParameter (rel = new juce::AudioParameterFloat ({ "rel", 1 }, "Release (ms)", juce::NormalisableRange<float> (10.0f, 1000.0f, 1.0f, 0.4f), 100.0f));
    }
    const juce::String getName() const override { return "Gate (Layback)"; }
    void prepareToPlay (double sr, int spb) override { juce::dsp::ProcessSpec s { sr, (juce::uint32) juce::jmax (1, spb), 2 }; gate.prepare (s); gate.reset(); }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        gate.setThreshold (thr->get()); gate.setRatio (juce::jmax (1.0f, rat->get()));
        gate.setAttack (atk->get()); gate.setRelease (rel->get());
        juce::dsp::AudioBlock<float> blk (b); juce::dsp::ProcessContextReplacing<float> ctx (blk); gate.process (ctx);
    }
private:
    juce::dsp::NoiseGate<float> gate;
    juce::AudioParameterFloat *thr = nullptr, *rat = nullptr, *atk = nullptr, *rel = nullptr;
};
