#include "LpcCodec.h"
#include <cmath>
#include <vector>

namespace
{
    constexpr float preEmph = 0.9f;
    constexpr float hammingPower = 0.3974f;    // mean of w^2 for a Hamming window
    constexpr int pitchWin = 320;              // 40 ms for the pitch search
    constexpr int minLag = 20, maxLag = 160;   // 400 Hz .. 50 Hz at 8 kHz
    constexpr float voicingThreshold = 0.38f;
    constexpr float damping = 0.996f;          // bandwidth expansion, ~10 Hz at 8 kHz
    constexpr float makeup = 1.2f;             // +1.6 dB: centres codec loudness on the dry voice (measured)

    // Coarsest bit allocation (bitRate = 1); finest is 8 bits everywhere.
    constexpr int coarseBits[LpcCodec::order + 1] = { 0, 3, 3, 2, 2, 2, 1, 1, 1, 1, 1 };

    // Windowed-sinc interpolator: sincTaps taps, sincPhases (+1) fractional phases.
    constexpr int sincTaps = 16, sincHalf = 8, sincPhases = 256;
    constexpr double sincCutoff = 0.9;   // of the 4 kHz Nyquist

    const std::vector<float>& sincTable()
    {
        static const auto t = []
        {
            std::vector<float> v ((size_t) (sincPhases + 1) * sincTaps);
            for (int p = 0; p <= sincPhases; ++p)
            {
                const double frac = (double) p / sincPhases;
                for (int k = 0; k < sincTaps; ++k)
                {
                    const double x = (double) (k - sincHalf + 1) - frac;   // distance from the read position
                    const double arg = juce::MathConstants<double>::pi * sincCutoff * x;
                    const double sinc = std::abs (x) < 1e-9 ? sincCutoff : std::sin (arg) / (juce::MathConstants<double>::pi * x);
                    const double wx = (x + sincHalf) / (2.0 * sincHalf);   // 0..1 across the kernel
                    const double win = (wx <= 0.0 || wx >= 1.0) ? 0.0
                                     : 0.42 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * wx) + 0.08 * std::cos (2.0 * juce::MathConstants<double>::twoPi * wx);
                    v[(size_t) p * sincTaps + (size_t) k] = (float) (sinc * win);
                }
            }
            return v;
        }();
        return t;
    }

    const std::array<float, LpcCodec::winLen>& hamming()
    {
        static const auto w = []
        {
            std::array<float, LpcCodec::winLen> a {};
            for (int i = 0; i < LpcCodec::winLen; ++i)
                a[(size_t) i] = 0.54f - 0.46f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (LpcCodec::winLen - 1));
            return a;
        }();
        return w;
    }

    float lerp (float a, float b, float t) { return a + (b - a) * t; }
}

//==============================================================================
int LpcCodec::latencySamples (double rate)
{
    // One frame of buffering, plus half an analysis window (parameters describe the
    // window's centre but are reached at the end of the output frame), plus the
    // interpolator. Measured envelope lag matches this; the tests check it.
    return (int) std::round ((frameLen + winLen / 2 + sincHalf + 1.5) * rate / codecRate);
}

void LpcCodec::prepare (double rate, juce::uint32 s)
{
    hostRate = rate;
    seed = s;
    const double cut = juce::jmin (3400.0, rate * 0.45);
    // Three cascaded biquads ~ 6th-order low-pass, both directions.
    const double qs[3] = { 0.5176, 0.7071, 1.9319 };
    for (int i = 0; i < 3; ++i)
    {
        auto c = juce::IIRCoefficients::makeLowPass (rate, cut, qs[i]);
        downLp[i].setCoefficients (c);
    }
    // DC blocker: the impulse train has a DC component that de-emphasis boosts ~20 dB,
    // and it adds coherently across a crowd of encoded voices. 50 Hz keeps deep fundamentals.
    outHp.setCoefficients (juce::IIRCoefficients::makeHighPass (codecRate, 50.0, 0.7071));
    reset();
}

void LpcCodec::reset()
{
    for (auto& f : downLp) f.reset();
    outHp.reset();
    downPos = 0.0; downPrev = 0.0f;
    histEmph.fill (0.0f); histRaw.fill (0.0f);
    histPos = 0; preEmphPrev = 0.0f; sinceFrame = 0;

    fifo.fill (0.0f);
    fifoRead = 0;
    fifoWrite = frameLen + sincHalf + 2;   // prefill: one frame plus the interpolator's look-ahead
    upFrac = 0.0;
    deEmphPrev = 0.0f;

    lattice.fill (0.0f);
    pulsePhase = 0.0;
    prevFrame = {}; lastGood = {};
    frameCounter = 0; lossRun = 0; prevLost = false;
    trackedPitch = 0.0f; pitchHistory.fill (0.0f);
    rng.setSeed ((juce::int64) seed * 7919 + 17);
}

void LpcCodec::setParams (float br, float fd, PitchLock pl, float fixedHz)
{
    bitRate = juce::jlimit (0.0f, 1.0f, br);
    frameDrop = juce::jlimit (0.0f, 1.0f, fd);
    lock = pl;
    fixedPitch = fixedHz > 30.0f ? fixedHz : 110.0f;
}

//==============================================================================
void LpcCodec::process (const float* in, float* out, int n)
{
    const double step8 = codecRate / hostRate;   // 8 kHz samples per host sample
    const double hostPer8 = hostRate / codecRate;

    for (int i = 0; i < n; ++i)
    {
        // Down: band-limit, then sample at 8 kHz by linear interpolation.
        float y = in[i];
        for (auto& f : downLp) y = f.processSingleSampleRaw (y);
        while (downPos <= 0.0)
        {
            push8k (downPrev + (float) (downPos + 1.0) * (y - downPrev));
            downPos += hostPer8;
        }
        downPos -= 1.0;
        downPrev = y;

        // Up: windowed-sinc interpolation from the 8 kHz FIFO (no images above 4 kHz).
        float z = 0.0f;
        {
            const auto& tbl = sincTable();
            // upFrac is in [0, 1) but can round to exactly 1.0 in float: clamp so we never
            // read past the table's last phase.
            const double ph = upFrac * sincPhases;
            const int p0 = juce::jlimit (0, sincPhases - 1, (int) ph);
            const float pf = (float) juce::jlimit (0.0, 1.0, ph - p0);
            const float* h0 = tbl.data() + (size_t) p0 * sincTaps;
            const float* h1 = h0 + sincTaps;
            for (int t = 0; t < sincTaps; ++t)
            {
                const float h = h0[t] + pf * (h1[t] - h0[t]);
                z += h * fifo[(size_t) ((fifoRead + t - sincHalf + 1) & (fifoLen - 1))];
            }
        }
        upFrac += step8;
        while (upFrac >= 1.0)
        {
            upFrac -= 1.0;
            fifoRead = (fifoRead + 1) & (fifoLen - 1);
        }
        out[i] = z;
    }
}

void LpcCodec::push8k (float x)
{
    histRaw[(size_t) histPos] = x;
    histEmph[(size_t) histPos] = x - preEmph * preEmphPrev;
    preEmphPrev = x;
    histPos = (histPos + 1) & (histLen - 1);

    if (++sinceFrame >= frameLen)
    {
        sinceFrame = 0;
        runFrame();
    }
}

//==============================================================================
LpcCodec::Frame LpcCodec::analyse()
{
    Frame fr;
    const auto& w = hamming();

    // LPC on the pre-emphasised window.
    std::array<float, winLen> x {};
    for (int i = 0; i < winLen; ++i)
        x[(size_t) i] = histEmph[(size_t) ((histPos - winLen + i) & (histLen - 1))] * w[(size_t) i];

    double R[order + 1] {};
    for (int lag = 0; lag <= order; ++lag)
        for (int i = lag; i < winLen; ++i)
            R[lag] += (double) x[(size_t) i] * x[(size_t) (i - lag)];

    // Silence gate (~ -90 dBFS frame RMS): below this the LPC fit is meaningless.
    if (R[0] < 1e-9 * winLen * hammingPower) return fr;
    R[0] *= 1.0001;               // white-noise correction keeps the filter well-conditioned

    double a[order + 1] {}, tmp[order + 1] {};
    a[0] = 1.0;
    double E = R[0];
    for (int i = 1; i <= order; ++i)
    {
        double acc = R[i];
        for (int j = 1; j < i; ++j) acc += a[j] * R[i - j];
        double k = -acc / E;
        k = juce::jlimit (-0.999, 0.999, k);
        for (int j = 1; j < i; ++j) tmp[j] = a[j] + k * a[i - j];
        for (int j = 1; j < i; ++j) a[j] = tmp[j];
        a[i] = k;
        E *= (1.0 - k * k);
        fr.k[(size_t) i] = (float) k;
    }
    fr.gain = (float) std::sqrt (juce::jmax (0.0, E) / (winLen * hammingPower));

    // Pitch: centre-clipped, normalised autocorrelation of the raw signal.
    std::array<float, pitchWin> p {};
    float peak = 0.0f;
    for (int i = 0; i < pitchWin; ++i)
    {
        p[(size_t) i] = histRaw[(size_t) ((histPos - pitchWin + i) & (histLen - 1))];
        peak = juce::jmax (peak, std::abs (p[(size_t) i]));
    }
    const float clip = 0.3f * peak;
    for (auto& v : p) v = v > clip ? v - clip : (v < -clip ? v + clip : 0.0f);

    float best = 0.0f; int bestLag = 0;
    float corr[maxLag + 2] {};
    for (int lag = minLag - 1; lag <= maxLag + 1; ++lag)
    {
        double num = 0.0, e1 = 0.0, e2 = 0.0;
        for (int i = lag; i < pitchWin; ++i)
        {
            num += (double) p[(size_t) i] * p[(size_t) (i - lag)];
            e1 += (double) p[(size_t) i] * p[(size_t) i];
            e2 += (double) p[(size_t) (i - lag)] * p[(size_t) (i - lag)];
        }
        corr[lag] = e1 > 0.0 && e2 > 0.0 ? (float) (num / std::sqrt (e1 * e2)) : 0.0f;
    }
    for (int lag = minLag; lag <= maxLag; ++lag)
        if (corr[lag] > best) { best = corr[lag]; bestLag = lag; }

    // Prefer the shortest lag that is nearly as good (avoids octave-down errors).
    for (int lag = minLag; lag < bestLag; ++lag)
        if (corr[lag] > 0.85f * best && corr[lag] >= corr[lag - 1] && corr[lag] >= corr[lag + 1]) { bestLag = lag; break; }

    if (bestLag > 0 && best > voicingThreshold)
    {
        const float y0 = corr[bestLag - 1], y1 = corr[bestLag], y2 = corr[bestLag + 1];
        const float d = y0 - 2.0f * y1 + y2;
        const float shift = std::abs (d) > 1e-6f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (y0 - y2) / d) : 0.0f;
        fr.pitchHz = (float) codecRate / ((float) bestLag + shift);
        fr.voiced = true;
    }
    return fr;
}

void LpcCodec::quantise (Frame& fr) const
{
    const float b = bitRate;
    if (fr.gain <= 0.0f) return;   // silence carries no filter

    // Reflection coefficients: uniform in the arcsine domain, mid-rise, so |k| < 1 always.
    // The filter's power gain is 1 / prod(1 - k^2); keep it constant through quantisation
    // so coarse coefficients change the timbre, not the level.
    double powerBefore = 1.0, powerAfter = 1.0;
    for (int i = 1; i <= order; ++i)
    {
        powerBefore *= 1.0 - (double) fr.k[(size_t) i] * fr.k[(size_t) i];
        const int bits = juce::roundToInt (lerp (8.0f, (float) coarseBits[i], b));
        const int levels = 1 << bits;
        const float stepT = juce::MathConstants<float>::pi / (float) levels;
        const float theta = std::asin (juce::jlimit (-1.0f, 1.0f, fr.k[(size_t) i]));
        const int idx = juce::jlimit (0, levels - 1, (int) std::floor ((theta + juce::MathConstants<float>::halfPi) / stepT));
        fr.k[(size_t) i] = std::sin (((float) idx + 0.5f) * stepT - juce::MathConstants<float>::halfPi);
        powerAfter *= 1.0 - (double) fr.k[(size_t) i] * fr.k[(size_t) i];
    }
    if (powerBefore > 0.0 && powerAfter > 0.0)
        fr.gain *= (float) juce::jlimit (0.0625, 4.0, std::sqrt (powerAfter / powerBefore));   // -24 .. +12 dB

    // Gain in dB steps.
    if (fr.gain > 0.0f)
    {
        const float stepDb = lerp (0.5f, 6.0f, b);
        const float db = 20.0f * std::log10 (fr.gain);
        fr.gain = std::pow (10.0f, std::round (db / stepDb) * stepDb / 20.0f);
    }

    // Pitch in semitone steps (none when clean).
    const float semis = lerp (0.0f, 3.0f, b * b);
    if (fr.pitchHz > 0.0f && semis > 0.05f)
    {
        const float st = 12.0f * std::log2 (fr.pitchHz / 110.0f);
        fr.pitchHz = 110.0f * std::exp2 (std::round (st / semis) * semis / 12.0f);
    }
}

void LpcCodec::runFrame()
{
    // Starved bit rates transmit less often; between transmissions the decoder
    // just repeats what it last played (including any loss attenuation).
    const int hold = bitRate < 0.6f ? 1 : (bitRate < 0.85f ? 2 : 3);
    if (frameCounter++ % hold != 0)
    {
        synthesise (prevFrame, prevFrame);
        return;
    }

    Frame fr = analyse();

    // Pitch lock.
    switch (lock)
    {
        case PitchLock::fixed:
            fr.pitchHz = fixedPitch;   // monotone; unvoiced frames still hiss
            break;
        case PitchLock::tracking:
            if (fr.voiced)
            {
                pitchHistory[2] = pitchHistory[1]; pitchHistory[1] = pitchHistory[0]; pitchHistory[0] = fr.pitchHz;
                auto h = pitchHistory;
                std::sort (h.begin(), h.end());
                const float med = h[1] > 0.0f ? h[1] : fr.pitchHz;
                trackedPitch = trackedPitch > 0.0f ? 0.5f * (trackedPitch + med) : med;
                fr.pitchHz = trackedPitch;
            }
            break;
        case PitchLock::free:
            // Raw detector, held through consonants, never switches to noise:
            // everything buzzes and the pitch jumps wherever the detector lands.
            if (fr.voiced) trackedPitch = fr.pitchHz;
            fr.pitchHz = trackedPitch > 0.0f ? trackedPitch : fixedPitch;
            fr.voiced = fr.gain > 0.0f;
            break;
    }
    if (fr.pitchHz > 0.0f) fr.pitchHz *= harmony::ratio (fr.pitchHz, pitchMap);   // Spread harmony / Tune
    quantise (fr);

    // Packet loss on transmitted frames: bursty, overall loss ~= q.
    // A lost frame replays the last good one once at half level; further losses are silent.
    const float q = frameDrop * 0.5f;
    bool lost = false;
    if (q > 0.0f)
    {
        const float pLoss = prevLost ? 0.2f + 0.6f * q : q * (0.8f - 0.6f * q) / (1.0f - q);
        lost = rng.nextFloat() < pLoss;
    }
    prevLost = lost;

    Frame use = fr;
    if (lost)
    {
        ++lossRun;
        use = lastGood;
        use.gain *= lossRun == 1 ? 0.5f : 0.0f;
    }
    else
    {
        lossRun = 0;
        lastGood = fr;
    }

    synthesise (prevFrame, use);
    prevFrame = use;
}

void LpcCodec::synthesise (const Frame& from, const Frame& to)
{
    for (int j = 0; j < frameLen; ++j)
    {
        const float t = (float) (j + 1) / (float) frameLen;
        const float g = lerp (from.gain, to.gain, t);

        float exc;
        if (to.voiced && to.pitchHz > 0.0f)
        {
            const double period = codecRate / (to.pitchHz * pitchRatio);
            pulsePhase += 1.0 / period;
            if (pulsePhase >= 1.0) { pulsePhase -= 1.0; exc = (float) std::sqrt (period); }
            else exc = 0.0f;
        }
        else
        {
            exc = (rng.nextFloat() * 2.0f - 1.0f) * 1.7320508f;   // unit variance
        }

        // All-pole lattice with per-sample interpolated reflection coefficients.
        // Every delay is scaled by 'damping' (bandwidth expansion: all poles pulled in
        // by that factor), so coarse coefficients can't build a near-oscillator.
        float f = exc * g;
        for (int i = order; i >= 1; --i)
        {
            const float k = lerp (from.k[(size_t) i], to.k[(size_t) i], t);
            const float delayed = damping * lattice[(size_t) i - 1];
            f -= k * delayed;
            lattice[(size_t) i] = k * f + delayed;
        }
        lattice[0] = f;

        // De-emphasis, soft safety limit.
        deEmphPrev = f + preEmph * deEmphPrev;
        float hp = deEmphPrev;
        hp = outHp.processSingleSampleRaw (hp);
        hp *= makeup;
        const float yv = std::tanh (hp * 0.8f) / 0.8f;
        fifo[(size_t) fifoWrite] = yv;
        fifoWrite = (fifoWrite + 1) & (fifoLen - 1);
    }
}
