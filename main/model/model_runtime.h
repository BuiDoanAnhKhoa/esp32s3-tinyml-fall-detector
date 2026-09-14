#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include "model_input.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

namespace fall_detection {

struct PredictionTiming {
    int64_t input_us = 0;
    int64_t invoke_us = 0;
};

// No ESP-IDF dependencies: the same model execution can be checked on a PC.
class ModelRuntime {
public:
    // Optional monotonic microsecond clock for hardware/host profiling.
    explicit ModelRuntime(int64_t (*clock_us)() = nullptr) : clock_us_(clock_us) {}
    bool Initialize(uint8_t *arena, std::size_t arena_size);
    bool Predict(const ModelInputWindow &window, float *score);
    std::size_t ArenaUsedBytes() const;
    const char *LastError() const { return error_; }
    const PredictionTiming &LastTiming() const { return timing_; }
private:
    tflite::MicroMutableOpResolver<17> resolver_;
    std::optional<tflite::MicroInterpreter> interpreter_;
    bool ready_ = false;
    const char *error_ = "model not initialized";
    int64_t (*clock_us_)();
    PredictionTiming timing_;
};
}  // namespace fall_detection
