#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "model_fixtures.h"
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
    int8_t quantized[kValues];
    Check(!window.CopyTo(tensor, kValues), "no incomplete windows");
    Check(!window.CopyQuantizedTo(quantized, kValues), "no incomplete INT8 windows");
    for (uint32_t i = 0; i < 300; ++i) {
        const auto state = window.Push(Sample(i));
        Check((state == WindowUpdate::Ready) == (i == 199 || i == 299),
              "first prediction at sample 200, next at sample 300");
    }
    Check(window.CopyTo(tensor, kValues), "complete window");
    Check(!window.CopyTo(tensor, kValues - 1), "reject incorrect input capacity");
    Check(!window.CopyQuantizedTo(quantized, kValues - 1), "reject incorrect INT8 capacity");
    Check(!window.CopyQuantizedTo(nullptr, kValues), "reject null INT8 destination");
    for (int row = 0; row < 200; ++row) {
        const float x = (row + 100) * 0.001f;
        const float raw[3] = {x, 0.4f, 0.0f};
        for (int feature = 0; feature < 3; ++feature) {
            const float restored = tensor[row*3 + feature] * kScalerScale[feature] + kScalerMean[feature];
            Check(std::abs(restored - raw[feature]) < 1e-5f,
                  "feature order, scaling and chronological ring-buffer copy");
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
    bad.acc[2] = std::numeric_limits<float>::infinity();
    Check(window.Push(bad) == WindowUpdate::Invalid, "reject acceleration infinity");
    bad = Sample(503);
    bad.gyro[2] = std::numeric_limits<float>::infinity();
    Check(window.Push(bad) == WindowUpdate::Collecting, "unused gyro does not affect accelerometer model");
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

static void CheckQuantization() {
    Check(QuantizeScaledInput(0.0f) == -22, "standardized zero maps to input zero point");
    Check(QuantizeScaledInput(0.5f * kInputScale) == -22, "positive half rounds to even zero");
    Check(QuantizeScaledInput(3.5f * kInputScale) == -18, "positive half rounds up to even four");
    Check(QuantizeScaledInput(2.5f * kInputScale) == -20, "positive half does not round away from zero");
    Check(QuantizeScaledInput(-0.5f * kInputScale) == -22, "negative half rounds to even zero");
    Check(QuantizeScaledInput(-3.5f * kInputScale) == -26, "negative half rounds down to even minus four");
    Check(QuantizeScaledInput(-2.5f * kInputScale) == -24, "negative half does not round away from zero");
    // With this scale, 1.5 * scale / scale is just below 1.5 in float32.
    // These adjacent float32 values and expectations were checked with NumPy.
    Check(QuantizeScaledInput(0.28983354568481445f) == -21, "float32 value just below positive tie");
    Check(QuantizeScaledInput(0.28983357548713684f) == -20, "float32 value just above positive tie");
    Check(QuantizeScaledInput(1e6f) == 127, "positive saturation does not wrap");
    Check(QuantizeScaledInput(-1e6f) == -128, "negative saturation does not wrap");
    Check(DequantizeOutput(-128) == 0.0f, "minimum INT8 output represents zero probability");
    Check(DequantizeOutput(127) == 255.0f / 256, "maximum INT8 output represents 255/256");
    Check(DequantizeOutput(-77) < kFallThreshold, "last representable normal score");
    Check(DequantizeOutput(-76) >= kFallThreshold, "first representable fall score");
}

static int64_t HostClockUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void CheckReferenceFixtures(fall_detection::ModelRuntime &runtime) {
    bool scores_match = true;
    bool classifications_match = true;
    for (const auto &fixture : kModelFixtures) {
        ModelInputWindow window;
        // Force ring-buffer wrap so the reference also checks flattening order.
        for (uint32_t i = 0; i < 100; ++i) window.Push(Sample(i));
        for (uint32_t i = 0; i < 200; ++i) {
            auto sample = Sample(i + 100);
            std::memcpy(sample.acc, fixture.raw[i], sizeof(sample.acc));
            window.Push(sample);
        }
        int8_t quantized[kValues];
        Check(window.CopyQuantizedTo(quantized, kValues), "copy reference INT8 window");
        Check(std::memcmp(quantized, fixture.quantized, sizeof(quantized)) == 0,
              "all 600 input bytes match desktop NumPy preprocessing");
        float score;
        if (!runtime.Predict(window, &score)) {
            std::fprintf(stderr, "%s: %s\n", fixture.name, runtime.LastError());
            Check(false, "reference fixture invocation");
        }
        // Independent expected output from LiteRT BUILTIN_REF, not this runtime.
        const float expected = (int(fixture.output) + 128) / 256.0f;
        std::printf("%s: score=%.9g, reference=%.9g, host Invoke=%lld us\n",
                    fixture.name, double(score), double(expected),
                    (long long)runtime.LastTiming().invoke_us);
        scores_match &= score == expected;
        classifications_match &= (score >= kFallThreshold) == (expected >= kFallThreshold);
    }
    Check(scores_match, "INT8 output matches desktop reference exactly");
    Check(classifications_match, "reference classification uses exported threshold");
}

int main() {
    CheckWindow();
    CheckQuantization();
    alignas(16) static uint8_t arena[256 * 1024];
    // Heap-backed device arenas are not guaranteed to start zeroed.
    std::memset(arena, 0xa5, sizeof(arena));
    fall_detection::ModelRuntime runtime(HostClockUs);
    ModelInputWindow window;
    float score = -1.0f;
    Check(!runtime.Predict(window, &score), "reject prediction before initialization");
    Check(!runtime.Initialize(nullptr, sizeof(arena)), "reject null arena");
    Check(!runtime.Initialize(arena + 1, sizeof(arena) - 1), "reject unaligned arena");
    if (!runtime.Initialize(arena, sizeof(arena))) {
        std::fprintf(stderr, "Initialize: %s\n", runtime.LastError());
        Check(false, "real TFLite Micro INT8 allocation and input/output validation");
    }
    Check(!runtime.Predict(window, &score), "reject prediction with incomplete window");
    Check(score == -1.0f, "failed prediction leaves score untouched");
    for (uint32_t i = 0; i < 200; ++i) window.Push(Sample(i));

    Check(!runtime.Predict(window, nullptr), "reject missing score destination");
    Check(runtime.Predict(window, &score), "real model invocation and valid score");
    CheckReferenceFixtures(runtime);
    Check(!runtime.Initialize(arena, sizeof(arena)), "reject repeated initialization");
    std::printf("PASS: INT8 preprocessing/window/reference checks; host model arena=%zu bytes; score=%.9g\n",
                runtime.ArenaUsedBytes(), double(score));

    alignas(16) static uint8_t small_arena[4096];
    fall_detection::ModelRuntime too_small;
    Check(!too_small.Initialize(small_arena, sizeof(small_arena)), "reject insufficient arena");
    Check(!too_small.Predict(window, &score), "failed allocation cannot invoke");
}
