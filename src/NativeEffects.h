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
    void getStateInformation (juce::MemoryBlock&) override     {}
    void setStateInformation (const void*, int) override       {}
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
