#ifndef SST_FILTERS_CUTOFFWARP_H
#define SST_FILTERS_CUTOFFWARP_H

#include "QuadFilterUnit.h"
#include "FilterCoefficientMaker.h"
#include "sst/basic-blocks/dsp/FastMath.h"
#include "sst/basic-blocks/dsp/Clippers.h"

/**
 * This namespace contains an adaptation of the filter found at
 * https://ccrma.stanford.edu/~jatin/ComplexNonlinearities/NLFeedback.html
 * with coefficient calculation from
 * https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
 */
namespace sst::filters::CutoffWarp
{
template <typename TuningProvider>
static float clampedFrequency(float pitch, float sampleRate, TuningProvider *provider)
{
    auto freq =
        provider->note_to_pitch_ignoring_tuning(pitch + 69) * (float)TuningProvider::MIDI_0_FREQ;
    freq = std::clamp(freq, 5.f, sampleRate * 0.3f);
    return freq;
}

#define F(a) _mm_set_ps1(a)
#define M(a, b) _mm_mul_ps(a, b)
#define A(a, b) _mm_add_ps(a, b)
#define S(a, b) _mm_sub_ps(a, b)

enum Saturator
{
    SAT_TANH = 0,
    SAT_SOFT,
    SAT_OJD,
    SAT_SINE // removed
};

// this is a duplicate of the code in QuadFilterWaveshapers.cpp except without the multiplication by
// 'drive' and without the unused QuadFilterWaveshaperState pointer.
static inline __m128 ojd_waveshaper_ps(const __m128 x) noexcept
{
    const auto pm17 = _mm_set1_ps(-1.7f);
    const auto p11 = _mm_set1_ps(1.1f);
    const auto pm03 = _mm_set1_ps(-0.3f);
    const auto p09 = _mm_set1_ps(0.9f);

    const auto denLow = _mm_set1_ps(1.f / (4 * (1 - 0.3f)));
    const auto denHigh = _mm_set1_ps(1.f / (4 * (1 - 0.9f)));

    auto maskNeg = _mm_cmple_ps(x, pm17);                         // in <= -1.7f
    auto maskPos = _mm_cmpge_ps(x, p11);                          // in > 1.1f
    auto maskLow = _mm_andnot_ps(maskNeg, _mm_cmplt_ps(x, pm03)); // in > -1.7 && in < =0.3
    auto maskHigh = _mm_andnot_ps(maskPos, _mm_cmpgt_ps(x, p09)); // in > 0.9 && in < 1.1
    auto maskMid = _mm_and_ps(_mm_cmpge_ps(x, pm03), _mm_cmple_ps(x, p09)); // the middle

    const auto vNeg = _mm_set1_ps(-1.0);
    const auto vPos = _mm_set1_ps(1.0);
    auto vMid = x;

    auto xlow = _mm_sub_ps(x, pm03);
    auto vLow = _mm_add_ps(xlow, _mm_mul_ps(denLow, _mm_mul_ps(xlow, xlow)));
    vLow = _mm_add_ps(vLow, pm03);

    auto xhi = _mm_sub_ps(x, p09);
    auto vHi = _mm_sub_ps(xhi, _mm_mul_ps(denHigh, _mm_mul_ps(xhi, xhi)));
    vHi = _mm_add_ps(vHi, p09);

    return _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_and_ps(maskNeg, vNeg), _mm_and_ps(maskLow, vLow)),
                                 _mm_add_ps(_mm_and_ps(maskHigh, vHi), _mm_and_ps(maskPos, vPos))),
                      _mm_and_ps(maskMid, vMid));
}

static inline __m128 doNLFilter(const __m128 input, const __m128 a1, const __m128 a2,
                                const __m128 b0, const __m128 b1, const __m128 b2,
                                const __m128 makeup, const int sat, __m128 &z1, __m128 &z2) noexcept
{
    // out = z1 + b0 * input
    const __m128 out = A(z1, M(b0, input));

    // nonlinear feedback = saturator(out)
    __m128 nf;
    switch (sat)
    {
    case SAT_SOFT:
        nf = basic_blocks::dsp::softclip_ps(out); // note, this is a bit different to Jatin's softclipper
        break;
    case SAT_OJD:
        nf = ojd_waveshaper_ps(out);
        break;
    default: // SAT_TANH; the removed SAT_SINE and others are also caught here
        nf = basic_blocks::dsp::fasttanhSSEclamped(out);
        break;
    }

    // z1 = z2 + b1 * input - a1 * nf
    z1 = A(z2, S(M(b1, input), M(a1, nf)));
    // z2 = b2 * input - a2 * nf
    z2 = S(M(b2, input), M(a2, nf));
    return M(out, makeup);
}

enum nlf_coeffs
{
    nlf_a1 = 0,
    nlf_a2,
    nlf_b0,
    nlf_b1,
    nlf_b2,
    nlf_makeup,
    n_nlf_coeff
};

enum dlf_state
{
    nlf_z1, // 1st z-1 state for first  stage
    nlf_z2, // 2nd z-1 state for first  stage
    nlf_z3, // 1st z-1 state for second stage
    nlf_z4, // 2nd z-1 state for second stage
    nlf_z5, // 1st z-1 state for third  stage
    nlf_z6, // 2nd z-1 state for third  stage
    nlf_z7, // 1st z-1 state for fourth stage
    nlf_z8, // 2nd z-1 state for fourth stage
};

template <typename TuningProvider>
void makeCoefficients(FilterCoefficientMaker<TuningProvider> *cm, float freq, float reso, int type,
                      int subtype, float sampleRate, TuningProvider *provider)
{
    float C[n_cm_coeffs];

    reso = std::clamp(reso, 0.f, 1.f);

    const float q = ((reso * reso * reso) * 18.0f + 0.1f);

    const float normalisedFreq = 2.0f * clampedFrequency(freq, sampleRate, provider) / sampleRate;
    const float wc = (float)M_PI * normalisedFreq;

    const float wsin = basic_blocks::dsp::fastsin(wc);
    const float wcos = basic_blocks::dsp::fastcos(wc);
    const float alpha = wsin / (2.0f * q);

    // note we actually calculate the reciprocal of a0 because we only use a0 to normalize the
    // other coefficients, and multiplication by reciprocal is cheaper than dividing.
    const float a0r = 1.0f / (1.0f + alpha);

    C[nlf_a1] = -2.0f * wcos * a0r;
    C[nlf_a2] = (1.0f - alpha) * a0r;
    C[nlf_makeup] = 1.0f;

    /*
     * To see where this table comes from look in the HeadlessNonTestFunctions.
     */
    constexpr bool useNormalization = true;
    float normNumerator = 1.0f;

    // tweaked these by hand/ear after the RMS measuring program did its thing... this world still
    // needs humans! :) - EvilDragon
    constexpr float lpNormTable[12] = {1.53273f,  1.33407f,  1.08197f,  0.958219f,
                                       1.27374f,  0.932342f, 0.761765f, 0.665462f,
                                       0.776856f, 0.597575f, 0.496207f, 0.471714f};

    // extra resonance makeup term for OJD subtypes
    float expMin = type == fut_cutoffwarp_lp ? 0.1f : 0.35f;
    float resMakeup = subtype < 8 ? 1.0f : 1.0f / std::pow(std::max(reso, expMin), 0.5f);

    switch (type)
    {
    case fut_cutoffwarp_lp: // lowpass
        C[nlf_b1] = (1.0f - wcos) * a0r;
        C[nlf_b0] = C[nlf_b1] * 0.5f;
        C[nlf_b2] = C[nlf_b0];

        if constexpr (useNormalization)
        {
            normNumerator = lpNormTable[subtype];
        }
        C[nlf_makeup] =
            resMakeup * normNumerator / std::pow(std::max(normalisedFreq, 0.001f), 0.1f);

        break;
    case fut_cutoffwarp_hp: // highpass
        C[nlf_b1] = -(1.0f + wcos) * a0r;
        C[nlf_b0] = C[nlf_b1] * -0.5f;
        C[nlf_b2] = C[nlf_b0];

        if constexpr (useNormalization)
        {
            normNumerator = lpNormTable[subtype];
        }
        C[nlf_makeup] =
            resMakeup * normNumerator / std::pow(std::max(1.0f - normalisedFreq, 0.001f), 0.1f);

        break;
    case fut_cutoffwarp_n: // notch
        C[nlf_b0] = a0r;
        C[nlf_b1] = -2.0f * wcos * a0r;
        C[nlf_b2] = C[nlf_b0];
        break;
    case fut_cutoffwarp_bp: // bandpass
        C[nlf_b0] = wsin * 0.5f * a0r;
        C[nlf_b1] = 0.0f;
        C[nlf_b2] = -C[nlf_b0];
        break;
    default: // allpass
        C[nlf_b0] = C[nlf_a2];
        C[nlf_b1] = C[nlf_a1];
        C[nlf_b2] = 1.0f; // (1+a) / (1+a) = 1 (from normalising by a0)
        break;
    }

    cm->FromDirect(C);
}

template <FilterSubType subtype>
inline __m128 process(QuadFilterUnitState *__restrict f, __m128 input)
{
    // lower 2 bits of subtype is the stage count
    const int stages = subtype & 3;
    // next two bits after that select the saturator
    const int sat = ((subtype >> 2) & 3);

    // n.b. stages are zero-indexed so use <=
    for (int stage = 0; stage <= stages; ++stage)
    {
        input =
            doNLFilter(input, f->C[nlf_a1], f->C[nlf_a2], f->C[nlf_b0], f->C[nlf_b1], f->C[nlf_b2],
                       f->C[nlf_makeup], sat, f->R[nlf_z1 + stage * 2], f->R[nlf_z2 + stage * 2]);
    }

    for (int i = 0; i < n_nlf_coeff; ++i)
    {
        f->C[i] = A(f->C[i], f->dC[i]);
    }

    return input;
}

#undef F
#undef M
#undef A
#undef S

} // namespace sst::filters::CutoffWarp

#endif // SST_FILTERS_CUTOFFWARP_H
