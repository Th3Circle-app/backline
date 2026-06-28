#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <atomic>
#include <cmath>

//==============================================================================
/** ITU-R BS.1770-4 / EBU R128 loudness measurement: momentary (400 ms),
    short-term (3 s), gated integrated loudness, and 4x-oversampled true-peak.
    Stereo program (L/R channel weights = 1.0). K-weighting coefficients are
    re-derived for the actual sample rate (libebur128 method) so it's correct
    at 44.1/48/96k, not just 48k. Audio-thread safe: processBlock() runs on the
    audio thread, getters read std::atomic results from any thread. */
class LoudnessMeter
{
public:
    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        ch = juce::jlimit (1, 2, numChannels);
        designKWeighting();
        reset();
    }

    /** Restart all integration windows (e.g. before measuring a program). */
    void reset()
    {
        for (auto& c : st) c = State{};
        momWin = juce::jmax (1, (int) std::llround (0.400 * fs));
        stWin  = juce::jmax (1, (int) std::llround (3.000 * fs));
        gateHop = juce::jmax (1, (int) std::llround (0.100 * fs));
        momRing.assign ((size_t) momWin, 0.0); momPos = 0; momSum = 0.0; momFilled = 0;
        stRing.assign  ((size_t) stWin, 0.0);  stPos  = 0; stSum  = 0.0; stFilled  = 0;
        hopCount = 0;
        gateBlocks.clear(); gateBlocks.reserve (4096);
        truePeakMax = 0.0f;
        aMom.store (kSilence); aShort.store (kSilence); aInt.store (kSilence); aTP.store (-200.0f);
    }

    void processBlock (const juce::AudioBuffer<float>& buf, int startSample, int numSamples)
    {
        const int nc = juce::jmin (ch, buf.getNumChannels());
        const double offset = -0.691;   // BS.1770 absolute offset
        for (int i = 0; i < numSamples; ++i)
        {
            double zsum = 0.0;                    // sum of per-channel K-weighted squares
            for (int c = 0; c < nc; ++c)
            {
                const float x = buf.getReadPointer (c)[startSample + i];
                trackTruePeak (c, x);
                double y = biquad (st[c].s1, k1, (double) x);   // stage 1: high shelf
                y = biquad (st[c].s2, k2, y);                   // stage 2: high-pass
                zsum += y * y;
            }

            momSum += zsum - momRing[(size_t) momPos]; momRing[(size_t) momPos] = zsum;
            if (++momPos >= momWin) momPos = 0;
            if (momFilled < momWin) ++momFilled;

            stSum += zsum - stRing[(size_t) stPos]; stRing[(size_t) stPos] = zsum;
            if (++stPos >= stWin) stPos = 0;
            if (stFilled < stWin) ++stFilled;

            if (++hopCount >= gateHop)            // every 100 ms: capture an overlapping 400 ms gating block
            {
                hopCount = 0;
                if (momFilled >= momWin)
                    gateBlocks.push_back (momSum / (double) momWin);   // mean-square of this 400 ms block
            }
        }

        aMom.store   (momFilled  > 0 ? (float) (offset + 10.0 * std::log10 (momSum / (double) momFilled + 1e-12)) : kSilence);
        aShort.store (stFilled   > 0 ? (float) (offset + 10.0 * std::log10 (stSum  / (double) stFilled  + 1e-12)) : kSilence);
        aTP.store    (truePeakMax > 0.0f ? juce::Decibels::gainToDecibels (truePeakMax) : -200.0f);
        aInt.store   ((float) integratedNow());
    }

    float getMomentaryLufs()  const { return aMom.load(); }
    float getShortTermLufs()  const { return aShort.load(); }
    float getIntegratedLufs() const { return aInt.load(); }
    float getTruePeakDb()     const { return aTP.load(); }

    static constexpr float kSilence = -100.0f;

private:
    struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct Biquad { double z1 = 0, z2 = 0; };
    struct State  { Biquad s1, s2; };

    static double biquad (Biquad& b, const Coeffs& c, double x)   // direct form II transposed
    {
        const double y = c.b0 * x + b.z1;
        b.z1 = c.b1 * x - c.a1 * y + b.z2;
        b.z2 = c.b2 * x - c.a2 * y;
        return y;
    }

    void designKWeighting()
    {
        {   // stage 1: high-shelf (RLB pre-filter)
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            k1.b0 = (Vh + Vb * K / Q + K * K) / a0;
            k1.b1 = 2.0 * (K * K - Vh) / a0;
            k1.b2 = (Vh - Vb * K / Q + K * K) / a0;
            k1.a1 = 2.0 * (K * K - 1.0) / a0;
            k1.a2 = (1.0 - K / Q + K * K) / a0;
        }
        {   // stage 2: high-pass
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            k2.b0 = 1.0; k2.b1 = -2.0; k2.b2 = 1.0;
            k2.a1 = 2.0 * (K * K - 1.0) / a0;
            k2.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }

    // Gated integrated loudness (EBU R128): -70 LUFS absolute gate, then -10 LU relative gate.
    double integratedNow() const
    {
        if (gateBlocks.empty()) return kSilence;
        const double offset = -0.691;
        const double absGate = std::pow (10.0, (-70.0 - offset) / 10.0);   // mean-square threshold for -70 LUFS
        double sum = 0.0; size_t cnt = 0;
        for (double z : gateBlocks) if (z >= absGate) { sum += z; ++cnt; }
        if (cnt == 0) return kSilence;
        const double relLoud = offset + 10.0 * std::log10 (sum / (double) cnt);   // ungated (abs-gated) mean
        const double relGate = std::pow (10.0, (relLoud - 10.0 - offset) / 10.0);
        sum = 0.0; cnt = 0;
        for (double z : gateBlocks) if (z >= absGate && z >= relGate) { sum += z; ++cnt; }
        if (cnt == 0) return kSilence;
        return offset + 10.0 * std::log10 (sum / (double) cnt);
    }

    // 4x cubic (Catmull-Rom) oversampled true peak, delayed by one sample.
    void trackTruePeak (int c, float x)
    {
        float* h = tpHist[c];
        const float p0 = h[0], p1 = h[1], p2 = h[2], p3 = x;   // interpolate the [p1,p2] interval
        truePeakMax = juce::jmax (truePeakMax, std::abs (p1));
        for (int s = 1; s < 4; ++s)
        {
            const float t = (float) s * 0.25f;
            const float y = 0.5f * ((2.0f * p1) + (-p0 + p2) * t
                                    + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t
                                    + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
            truePeakMax = juce::jmax (truePeakMax, std::abs (y));
        }
        h[0] = p1; h[1] = p2; h[2] = p3;
    }

    double fs = 48000.0;
    int ch = 2;
    Coeffs k1, k2;
    State st[2];
    float tpHist[2][3] { { 0, 0, 0 }, { 0, 0, 0 } };
    float truePeakMax = 0.0f;

    int momWin = 1, stWin = 1, gateHop = 1, momPos = 0, stPos = 0, momFilled = 0, stFilled = 0, hopCount = 0;
    double momSum = 0.0, stSum = 0.0;
    std::vector<double> momRing, stRing, gateBlocks;

    std::atomic<float> aMom { kSilence }, aShort { kSilence }, aInt { kSilence }, aTP { -200.0f };
};
