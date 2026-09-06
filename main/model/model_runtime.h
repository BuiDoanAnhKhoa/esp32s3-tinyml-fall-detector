#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include "model_input.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

namespace fall_detection {

// No ESP-IDF dependencies: the same model execution can be checked on a PC.
class ModelRuntime {
public:
    bool Initialize(uint8_t *arena, std::size_t arena_size);
    bool Predict(const ModelInputWindow &window, float *score);
    std::size_t ArenaUsedBytes() const;
    const char *LastError() const { return error_; }
private:
    tflite::MicroMutableOpResolver<17> resolver_;
    std::optional<tflite::MicroInterpreter> interpreter_;
    bool ready_ = false;
    const char *error_ = "model not initialized";
};
}  // namespace fall_detection
