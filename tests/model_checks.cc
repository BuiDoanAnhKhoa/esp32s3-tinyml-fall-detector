#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "model_runtime.h"

using fall_detection::ModelInputWindow;
using fall_detection::WindowUpdate;
constexpr int kValues = fall_detection::kTensorValues;

static void Check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static imu_sample_t Sample(uint32_t index) {
    return {int64_t(index) * 10000, index,
            {float(index) * 0.001f, 0.4f, 0.0f}, {3.0f, 4.0f, 12.0f}};
}

static void CheckWindow() {
    ModelInputWindow window;
    float tensor[kValues];
    Check(!window.CopyTo(tensor, kValues), "no incomplete windows");
    for (uint32_t i = 0; i < 300; ++i) {
        const auto state = window.Push(Sample(i));
        Check((state == WindowUpdate::Ready) == (i == 199 || i == 299),
              "first prediction at sample 200, next at sample 300");
    }
    Check(window.CopyTo(tensor, kValues), "complete window");
    Check(!window.CopyTo(tensor, kValues - 1), "reject incorrect input capacity");
    for (int row = 0; row < 200; ++row) {
        const float x = (row + 100) * 0.001f;
        const float raw[8] = {x, 0.4f, 0, std::sqrt(x*x + 0.16f), 3, 4, 12, 13};
        for (int feature = 0; feature < 8; ++feature) {
            const float restored = tensor[row*8 + feature] * kScalerScale[feature] + kScalerMean[feature];
            Check(std::abs(restored - raw[feature]) < 1e-5f,
                  "feature order, magnitudes, scaling and chronological ring-buffer copy");
        }
    }

    Check(window.Push(Sample(301)) == WindowUpdate::Restarted, "missing sample resets window");
    Check(!window.CopyTo(tensor, kValues), "no window spanning a gap");
    for (uint32_t i = 302; i <= 500; ++i) {
        Check((window.Push(Sample(i)) == WindowUpdate::Ready) == (i == 500),
              "full warmup after a gap");
    }
    auto bad = Sample(501);
    bad.acc[0] = std::numeric_limits<float>::quiet_NaN();
    Check(window.Push(bad) == WindowUpdate::Invalid, "reject NaN");
    bad = Sample(502);
    bad.gyro[2] = std::numeric_limits<float>::infinity();
    Check(window.Push(bad) == WindowUpdate::Invalid, "reject infinity");
    window.Push(Sample(503));
    bad = Sample(504);
    bad.timestamp_us += 10000;
    Check(window.Push(bad) == WindowUpdate::Restarted, "timestamp gap resets even with consecutive IDs");
    bad.sequence++;
    Check(window.Push(bad) == WindowUpdate::Restarted, "duplicate time is not a fresh sample");

    window.Reset();
    auto wrap = Sample(0);
    wrap.sequence = UINT32_MAX;
    window.Push(wrap);
    wrap.sequence = 0;
    wrap.timestamp_us += 10000;
    Check(window.Push(wrap) == WindowUpdate::Collecting, "sequence counter wrap remains continuous");
}

int main() {
    CheckWindow();
    alignas(16) static uint8_t arena[256 * 1024];
    fall_detection::ModelRuntime runtime;
    Check(runtime.Initialize(arena, sizeof(arena)), "real TFLite Micro model allocation and input/output validation");
    ModelInputWindow window;
    for (uint32_t i = 0; i < 200; ++i) window.Push(Sample(i));

    float score;
    Check(runtime.Predict(window, &score), "real model invocation and valid score");
    std::printf("PASS: preprocessing/window checks; model arena=%zu bytes; score=%.9g\n",
                runtime.ArenaUsedBytes(), double(score));
}
