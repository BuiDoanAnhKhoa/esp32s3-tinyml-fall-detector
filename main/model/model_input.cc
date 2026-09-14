#include "model_input.h"

#include <cmath>
#include <cstring>

static_assert(kNumFeatures == 3 && kInputSize == kWindowSize * kNumFeatures);
static_assert(kScalerSize == kNumFeatures);
static_assert(kWindowSize == 200 && kWindowStep > 0 && kWindowStep <= kWindowSize);
static_assert(sizeof(kScalerMean) / sizeof(float) == kNumFeatures);
static_assert(sizeof(kScalerScale) / sizeof(float) == kNumFeatures);

namespace fall_detection {

void ModelInputWindow::Reset() {
    next_ = count_ = since_prediction_ = 0;
    previous_timestamp_ = 0;
    previous_sequence_ = 0;
}

WindowUpdate ModelInputWindow::Push(const imu_sample_t &sample) {
    const float *a = sample.acc;
    const float raw[kNumFeatures] = { a[0], a[1], a[2] };
    float scaled[kNumFeatures];
    for (int i = 0; i < kNumFeatures; ++i) {
        if (!std::isfinite(raw[i]) || !std::isfinite(kScalerScale[i]) || kScalerScale[i] <= 0) {
            Reset();
            return WindowUpdate::Invalid;
        }
        scaled[i] = StandardizeInput(raw[i], i);
        if (!std::isfinite(scaled[i])) {
            Reset();
            return WindowUpdate::Invalid;
        }
    }

    bool restarted = false;
    if (count_ != 0) {
        const int64_t interval = sample.timestamp_us - previous_timestamp_;
        if (sample.sequence != uint32_t(previous_sequence_ + 1) ||
            interval < kMinSampleIntervalUs || interval > kMaxSampleIntervalUs) {
            Reset();
            restarted = true;
        }
    }
    previous_timestamp_ = sample.timestamp_us;
    previous_sequence_ = sample.sequence;
    std::memcpy(features_[next_], scaled, sizeof(scaled));
    next_ = (next_ + 1) % kWindowSize;

    if (count_ < kWindowSize) {
        if (++count_ == kWindowSize) {
            since_prediction_ = 0;
            return WindowUpdate::Ready;
        }
    } else if (++since_prediction_ == kWindowStep) {
        since_prediction_ = 0;
        return WindowUpdate::Ready;
    }
    return restarted ? WindowUpdate::Restarted : WindowUpdate::Collecting;
}

bool ModelInputWindow::CopyTo(float *destination, std::size_t count) const {
    if (!destination || count != kTensorValues || count_ != kWindowSize) {
        return false;
    }
    for (int i = 0; i < kWindowSize; ++i) {
        std::memcpy(destination + i * kNumFeatures,
                    features_[(next_ + i) % kWindowSize],
                    kNumFeatures * sizeof(float));
    }
    return true;
}

bool ModelInputWindow::CopyQuantizedTo(int8_t *destination, std::size_t count) const {
    if (!destination || count != kTensorValues || count_ != kWindowSize) {
        return false;
    }
    for (int row = 0; row < kWindowSize; ++row) {
        const float *features = features_[(next_ + row) % kWindowSize];
        for (int axis = 0; axis < kNumFeatures; ++axis) {
            destination[row * kNumFeatures + axis] = QuantizeScaledInput(features[axis]);
        }
    }
    return true;
}

}  // namespace fall_detection
