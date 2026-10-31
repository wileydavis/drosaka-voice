#include "Align.h"
#include <algorithm>
#include <cmath>

namespace align
{
    namespace
    {
        constexpr float floorDb = 45.0f;     // envelope range below the loudest moment
        constexpr float onsetWeight = 3.0f;  // syllable onsets matter most for alignment

        double corrAt (const std::vector<float>& r, const std::vector<float>& o, int lag)
        {
            // sum_t r[t] * o[t - lag]
            double acc = 0.0;
            const int n = (int) r.size(), m = (int) o.size();
            const int t0 = std::max (0, lag), t1 = std::min (n, m + lag);
            for (int t = t0; t < t1; ++t) acc += (double) r[(size_t) t] * o[(size_t) (t - lag)];
            return acc;
        }

        double energy (const std::vector<float>& v)
        {
            double e = 0.0;
            for (auto x : v) e += (double) x * x;
            return e;
        }

        std::vector<float> decimate (const std::vector<float>& v, int f)
        {
            std::vector<float> d (v.size() / (size_t) f + 1, 0.0f);
            for (size_t i = 0; i < v.size(); ++i) d[i / (size_t) f] += v[i] / (float) f;
            return d;
        }
    }

    std::vector<float> envelope (const float* x, int n, double sr)
    {
        const int hop = std::max (1, (int) std::round (sr / envRate));
        const int win = std::max (hop, (int) std::round (sr * 0.010));
        const int frames = n / hop + 1;
        std::vector<float> logE ((size_t) frames, -120.0f);

        // Sliding 10 ms energy, centred on each 1 ms hop.
        std::vector<double> cum ((size_t) n + 1, 0.0);
        for (int i = 0; i < n; ++i) cum[(size_t) i + 1] = cum[(size_t) i] + (double) x[i] * x[i];
        float maxDb = -120.0f;
        for (int f = 0; f < frames; ++f)
        {
            const int c = f * hop;
            const int a = std::max (0, c - win / 2), b = std::min (n, c + win / 2);
            if (b <= a) continue;
            const double e = (cum[(size_t) b] - cum[(size_t) a]) / (b - a);
            logE[(size_t) f] = (float) (10.0 * std::log10 (e + 1e-12));
            maxDb = std::max (maxDb, logE[(size_t) f]);
        }

        std::vector<float> env ((size_t) frames, 0.0f);
        for (int f = 0; f < frames; ++f)
            env[(size_t) f] = std::max (0.0f, logE[(size_t) f] - (maxDb - floorDb)) / floorDb;

        // Add the rise over 10 ms: sharp at syllable starts, which is where alignment lives.
        std::vector<float> out ((size_t) frames, 0.0f);
        for (int f = 0; f < frames; ++f)
        {
            const float rise = f >= 10 ? std::max (0.0f, env[(size_t) f] - env[(size_t) (f - 10)]) : 0.0f;
            out[(size_t) f] = env[(size_t) f] + onsetWeight * rise;
        }

        // Remove the slow "there is speech here" component (250 ms moving average) so only
        // syllable rhythm remains: any two phrases overlap as blobs, but only the same
        // words share their rhythm.
        const int half = 125;
        std::vector<double> cs (out.size() + 1, 0.0);
        for (size_t i = 0; i < out.size(); ++i) cs[i + 1] = cs[i] + out[i];
        std::vector<float> hp (out.size());
        for (int f = 0; f < frames; ++f)
        {
            const int a = std::max (0, f - half), b = std::min (frames, f + half + 1);
            hp[(size_t) f] = out[(size_t) f] - (float) ((cs[(size_t) b] - cs[(size_t) a]) / (b - a));
        }
        return hp;
    }

    Result findOffset (const std::vector<float>& ref, const std::vector<float>& other, double refOffsetSec, double maxShiftSec)
    {
        Result res;
        if (ref.empty() || other.empty()) return res;

        // Coarse search at 100 Hz.
        constexpr int dec = 10;
        const auto r = decimate (ref, dec), o = decimate (other, dec);
        const double norm = std::sqrt (energy (r) * energy (o));
        if (norm <= 0.0) return res;

        const int maxLag = (int) (maxShiftSec * envRate / dec);
        std::vector<double> c ((size_t) (2 * maxLag + 1));
        int best = 0;
        for (int lag = -maxLag; lag <= maxLag; ++lag)
        {
            c[(size_t) (lag + maxLag)] = corrAt (r, o, lag) / norm;
            if (c[(size_t) (lag + maxLag)] > c[(size_t) (best + maxLag)]) best = lag;
        }
        const double peak = c[(size_t) (best + maxLag)];

        // Best alternative more than 150 ms away from the peak.
        double alt = 0.0;
        for (int lag = -maxLag; lag <= maxLag; ++lag)
            if (std::abs (lag - best) > 15) alt = std::max (alt, c[(size_t) (lag + maxLag)]);

        // Refine at 1 kHz within +-15 ms.
        const double fullNorm = std::sqrt (energy (ref) * energy (other));
        int fine = best * dec;
        double fineBest = -1.0;
        for (int lag = best * dec - 15; lag <= best * dec + 15; ++lag)
        {
            const double v = corrAt (ref, other, lag) / fullNorm;
            if (v > fineBest) { fineBest = v; fine = lag; }
        }

        res.offsetSec = refOffsetSec + fine / envRate;
        res.score = (float) peak;
        res.margin = (float) (peak - alt);
        res.confident = peak > 0.35 && res.margin > 0.15;   // calibrated: same words 0.25+, different words ~0.07
        return res;
    }
}
