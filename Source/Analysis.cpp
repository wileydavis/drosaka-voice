#include "Analysis.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

namespace
{
    constexpr double anaRate = 16000.0;
    constexpr int frameLen = 1024;      // 64 ms
    constexpr int hop = 320;            // 20 ms
    constexpr float minF0 = 55.0f, maxF0 = 500.0f;
    constexpr float yinThreshold = 0.15f;

    // Band-limit then resample to 16 kHz.
    std::vector<float> toAnalysisRate (const float* in, int n, double rate)
    {
        std::vector<float> filtered (in, in + n);
        if (rate > anaRate * 1.01)
        {
            for (int pass = 0; pass < 2; ++pass)   // 4th-order-ish low-pass at 7 kHz
            {
                juce::IIRFilter f;
                f.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, 7000.0));
                f.processSamples (filtered.data(), n);
            }
        }
        const double ratio = rate / anaRate;
        const int outLen = (int) std::floor (n / ratio);
        std::vector<float> out ((size_t) juce::jmax (0, outLen));
        if (outLen > 0)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, filtered.data(), out.data(), outLen, n, 0);
        }
        return out;
    }

    // YIN pitch for one frame; returns 0 if unvoiced.
    float yinFrame (const float* x, std::vector<float>& d)
    {
        const int tauMin = (int) (anaRate / maxF0);
        const int tauMax = (int) (anaRate / minF0);
        const int W = frameLen - tauMax;   // comparison window

        d[0] = 1.0f;
        float running = 0.0f;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            float sum = 0.0f;
            for (int j = 0; j < W; ++j)
            {
                const float diff = x[j] - x[j + tau];
                sum += diff * diff;
            }
            running += sum;
            d[(size_t) tau] = running > 0.0f ? sum * (float) tau / running : 1.0f;   // CMND
        }

        for (int tau = tauMin; tau < tauMax; ++tau)
        {
            if (d[(size_t) tau] < yinThreshold)
            {
                while (tau + 1 < tauMax && d[(size_t) tau + 1] < d[(size_t) tau]) ++tau;
                // parabolic interpolation around the dip
                const float a = d[(size_t) tau - 1], b = d[(size_t) tau], c = d[(size_t) tau + 1];
                const float denom = a - 2.0f * b + c;
                const float shift = std::abs (denom) > 1e-9f ? 0.5f * (a - c) / denom : 0.0f;
                return (float) anaRate / ((float) tau + juce::jlimit (-1.0f, 1.0f, shift));
            }
        }
        return 0.0f;
    }
}

StemAnalysis analyseStem (const float* mono, int numSamples, double sampleRate)
{
    StemAnalysis result;
    auto x = toAnalysisRate (mono, numSamples, sampleRate);
    const int n = (int) x.size();
    if (n < frameLen) return result;

    // Frame energies, for the silence gate.
    const int numFrames = 1 + (n - frameLen) / hop;
    std::vector<float> rms ((size_t) numFrames);
    float maxRms = 0.0f;
    for (int f = 0; f < numFrames; ++f)
    {
        double s = 0.0;
        for (int j = 0; j < frameLen; ++j) s += (double) x[(size_t) (f * hop + j)] * x[(size_t) (f * hop + j)];
        rms[(size_t) f] = (float) std::sqrt (s / frameLen);
        maxRms = juce::jmax (maxRms, rms[(size_t) f]);
    }
    if (maxRms <= 0.0f) return result;
    const float gate = maxRms * 0.05f;   // −26 dB below the loudest frame

    juce::dsp::FFT fft (10);   // 1024
    juce::dsp::WindowingFunction<float> window ((size_t) frameLen, juce::dsp::WindowingFunction<float>::hann, false);
    std::vector<float> fftBuf ((size_t) frameLen * 2);
    std::vector<float> d ((size_t) frameLen);
    std::vector<float> pitches;
    double centroidSum = 0.0, centroidWeight = 0.0;
    double fallbackSum = 0.0, fallbackWeight = 0.0;   // for stems with no voiced frames
    int loudFrames = 0;

    const float binHz = (float) anaRate / frameLen;
    const int binLo = (int) std::ceil (200.0f / binHz), binHi = (int) (6000.0f / binHz);

    for (int f = 0; f < numFrames; ++f)
    {
        if (rms[(size_t) f] < gate) continue;
        ++loudFrames;
        const float* frame = x.data() + f * hop;

        const float f0 = yinFrame (frame, d);
        if (f0 > 0.0f) pitches.push_back (f0);

        std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
        std::copy (frame, frame + frameLen, fftBuf.begin());
        window.multiplyWithWindowingTable (fftBuf.data(), (size_t) frameLen);
        fft.performFrequencyOnlyForwardTransform (fftBuf.data(), true);

        double num = 0.0, den = 0.0;
        for (int b = binLo; b <= binHi; ++b)
        {
            num += (double) b * binHz * fftBuf[(size_t) b];
            den += fftBuf[(size_t) b];
        }
        if (den <= 0.0) continue;
        const double c = num / den;
        const double w = (double) rms[(size_t) f] * rms[(size_t) f];
        if (f0 > 0.0f) { centroidSum += c * w; centroidWeight += w; }
        fallbackSum += c * w; fallbackWeight += w;
    }

    if (! pitches.empty())
    {
        auto mid = pitches.begin() + (long) pitches.size() / 2;
        std::nth_element (pitches.begin(), mid, pitches.end());
        result.pitchHz = *mid;
    }
    if (centroidWeight > 0.0)      result.centroidHz = (float) (centroidSum / centroidWeight);
    else if (fallbackWeight > 0.0) result.centroidHz = (float) (fallbackSum / fallbackWeight);
    result.voicedFraction = loudFrames > 0 ? (float) pitches.size() / (float) loudFrames : 0.0f;
    return result;
}

//==============================================================================
PitchMarks findPitchMarks (const float* x, int n, double sr)
{
    PitchMarks m;
    if (n <= 0) return m;

    // F0 track every 10 ms at the analysis rate.
    auto a = toAnalysisRate (x, n, sr);
    const int an = (int) a.size();
    const int trackHop = 160;   // 10 ms at 16 kHz
    std::vector<float> f0;
    std::vector<float> d ((size_t) frameLen);
    float maxRms = 0.0f;
    std::vector<float> rms;
    for (int f = 0; f * trackHop + frameLen <= an; ++f)
    {
        double e = 0.0;
        for (int j = 0; j < frameLen; ++j) e += (double) a[(size_t) (f * trackHop + j)] * a[(size_t) (f * trackHop + j)];
        rms.push_back ((float) std::sqrt (e / frameLen));
        maxRms = juce::jmax (maxRms, rms.back());
    }
    for (size_t f = 0; f < rms.size(); ++f)
        f0.push_back (rms[f] > maxRms * 0.03f ? yinFrame (a.data() + f * trackHop, d) : 0.0f);

    // Median-of-3 to remove single-frame octave slips.
    for (size_t f = 1; f + 1 < f0.size(); ++f)
        if (f0[f - 1] > 0 && f0[f] > 0 && f0[f + 1] > 0)
        {
            float v[3] = { f0[f - 1], f0[f], f0[f + 1] };
            std::sort (v, v + 3);
            f0[f] = v[1];
        }

    // f0 at a source sample: frames are centred at (f*hop + frameLen/2) in analysis samples.
    const double toAna = anaRate / sr;
    auto f0At = [&] (int srcPos) -> float
    {
        const int f = (int) std::round ((srcPos * toAna - frameLen / 2) / trackHop);
        return (f >= 0 && f < (int) f0.size()) ? f0[(size_t) f] : 0.0f;
    };

    const int unvoicedStep = juce::jmax (1, (int) std::round (0.005 * sr));
    int t = 0;
    while (t < n)
    {
        const float hz = f0At (t);
        if (hz > 0.0f)
        {
            const float P = (float) (sr / hz);
            // Snap to the largest peak within a third of a period, so marks sit at the
            // same point of each glottal cycle.
            const int lo = juce::jmax (0, t - (int) (P / 3)), hi = juce::jmin (n - 1, t + (int) (P / 3));
            int best = t < n ? t : n - 1;
            for (int i = lo; i <= hi; ++i) if (x[i] > x[best]) best = i;
            if (! m.pos.empty() && best <= m.pos.back()) best = m.pos.back() + juce::jmax (1, (int) (P * 0.5f));
            if (best >= n) break;
            m.pos.push_back (best);
            m.period.push_back (P);
            m.voiced.push_back (1);
            t = best + (int) std::round (P);
        }
        else
        {
            m.pos.push_back (t);
            m.period.push_back ((float) unvoicedStep);
            m.voiced.push_back (0);
            t += unvoicedStep;
        }
    }
    return m;
}
