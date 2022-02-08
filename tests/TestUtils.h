#pragma once

#include <array>
#include <iostream>
#include <numeric>
#include <vector>

#include <catch2/catch2.hpp>

#include <sst/filters.h>

namespace TestUtils
{
using sst::filters::FilterType, sst::filters::FilterSubType;

constexpr bool printRMSs = false; // change this to true when generating test data

constexpr auto sampleRate = 48000.0f;
constexpr int blockSize = 2048;

constexpr float A440 = 69.0f;
constexpr int numTestFreqs = 5;
constexpr std::array<float, numTestFreqs> testFreqs { 80.0f, 200.0f, 440.0f, 1000.0f, 10000.0f };

auto runSine = [](auto &filterState, auto &filterUnitPtr, float testFreq, int numSamples) {
    // reset filter state
    std::fill (filterState.R, &filterState.R[sst::filters::n_filter_registers], _mm_setzero_ps());

    std::vector<float> y(numSamples, 0.0f);
    for (int i = 0; i < numSamples; ++i)
    {
        auto x = (float)std::sin(2.0 * M_PI * (double)i * testFreq / sampleRate);

        auto yVec = filterUnitPtr(&filterState, _mm_set_ps1(x));

        float yArr alignas(16)[4];
        _mm_store_ps (yArr, yVec);
        y[i] = yArr[0];
    }

    auto squareSum = std::inner_product(y.begin(), y.end(), y.begin(), 0.0f);
    auto rms = std::sqrt(squareSum / (float) numSamples);
    return 20.0f * std::log10 (rms);
};

using RMSSet = std::array<float, numTestFreqs>;
struct TestConfig
{
    FilterType type;
    FilterSubType subType;
    RMSSet expRmsDBs;
};

static float delayBufferData[4][MAX_FB_COMB] {};

auto runTest = [] (const TestConfig& testConfig)
{
    auto filterState = sst::filters::QuadFilterUnitState{};
    for (int i = 0; i < 4; ++i)
    {
        std::fill (delayBufferData[i], delayBufferData[i] + MAX_FB_COMB, 0.0f);
        filterState.DB[i] = delayBufferData[i];
        filterState.active[i] = (int) 0xffffffff;
        filterState.WP[i] = 0;
    }

    auto filterUnitPtr = sst::filters::GetQFPtrFilterUnit(testConfig.type, testConfig.subType);

    sst::filters::FilterCoefficientMaker coefMaker;
    coefMaker.setSampleRateAndBlockSize(sampleRate, blockSize);
    coefMaker.MakeCoeffs(A440, 0.5f, testConfig.type, testConfig.subType, nullptr, false);
    coefMaker.updateState(filterState);

    std::array<float, numTestFreqs> actualRMSs {};
    for (int i = 0; i < numTestFreqs; ++i)
    {
        auto rmsDB = runSine(filterState, filterUnitPtr, testFreqs[i], blockSize);

        if constexpr (! printRMSs)
            REQUIRE(rmsDB == Approx(testConfig.expRmsDBs[i]).margin(1.0e-4f));

        actualRMSs[i] = rmsDB;
    }

    if constexpr (printRMSs)
    {
        std::cout << "{ ";
        for (int i = 0; i < numTestFreqs; ++i)
            std::cout << actualRMSs[i] << "f, ";

        std::cout << "}" << std::endl;
    }
};
}
