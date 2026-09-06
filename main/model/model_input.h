#pragma once

#include <cstddef>
#include "imu_sample.h"
#include "scaler_data.h"

namespace fall_detection {

constexpr int kTensorValues = kWindowSize * kNumFeatures;
constexpr int64_t kMinSampleIntervalUs = 5000;
constexpr int64_t kMaxSampleIntervalUs = 15000;

enum class WindowUpdate { Collecting, Ready, Restarted, Invalid };

// Single owner: the inference task. Oldest sample is flattened first, with
// eight adjacent features per sample, matching a [200, 8] row-major window.
class ModelInputWindow {
public:
    WindowUpdate Push(const imu_sample_t &sample);
    void Reset();
    bool CopyTo(float *destination, std::size_t count) const;

private:
    float features_[kWindowSize][kNumFeatures] = {};
    int next_ = 0;
    int count_ = 0;
    int since_prediction_ = 0;
    int64_t previous_timestamp_ = 0;
    uint32_t previous_sequence_ = 0;
};

}  // namespace fall_detection
