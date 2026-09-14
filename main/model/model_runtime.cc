#include "model_runtime.h"

#include <cmath>
#include "model_data.h"
#include "quantized_ops.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace fall_detection {

bool ModelRuntime::Initialize(uint8_t *arena, std::size_t arena_size) {
    error_ = "invalid/unaligned memory buffer or model already initialized";
    if (!arena || reinterpret_cast<uintptr_t>(arena) % 16 != 0 || interpreter_) return false;
    error_ = "model file verification failed";
    flatbuffers::Verifier verifier(g_fall_model, g_fall_model_len);
    if (!tflite::VerifyModelBuffer(verifier)) return false;
    const tflite::Model *model = tflite::GetModel(g_fall_model);
    error_ = "unsupported model schema version";
    if (model->version() != TFLITE_SCHEMA_VERSION) return false;

    // These are the 17 built-in operation types in the supplied model.
    error_ = "model operation registration failed";
    if (resolver_.AddShape() != kTfLiteOk ||
        resolver_.AddStridedSlice() != kTfLiteOk ||
        resolver_.AddPack() != kTfLiteOk ||
        resolver_.AddReshape() != kTfLiteOk ||
        resolver_.AddExpandDims() != kTfLiteOk ||
        resolver_.AddDepthwiseConv2D() != kTfLiteOk ||
        resolver_.AddConv2D() != kTfLiteOk ||
        resolver_.AddSpaceToBatchNd(RegisterQuantizedSpaceToBatchNd()) != kTfLiteOk ||
        resolver_.AddBatchToSpaceNd() != kTfLiteOk ||
        resolver_.AddMul() != kTfLiteOk ||
        resolver_.AddAdd() != kTfLiteOk ||
        resolver_.AddMean() != kTfLiteOk ||
        resolver_.AddLogistic() != kTfLiteOk ||
        resolver_.AddAveragePool2D() != kTfLiteOk ||
        resolver_.AddReduceMax() != kTfLiteOk ||
        resolver_.AddConcatenation() != kTfLiteOk ||
        resolver_.AddFullyConnected() != kTfLiteOk) return false;

    interpreter_.emplace(model, resolver_, arena, arena_size);
    error_ = "tensor allocation/preparation failed (memory size or model operation)";
    if (interpreter_->AllocateTensors() != kTfLiteOk) return false;
    error_ = "model must have one input and one output";
    if (interpreter_->inputs_size() != 1 || interpreter_->outputs_size() != 1) return false;
    const TfLiteTensor *input = interpreter_->input(0);
    const TfLiteTensor *output = interpreter_->output(0);
    error_ = "expected INT8 input [1,600] and output [1,1]";
    if (!(input && output && input->dims && output->dims &&
        input->type == kTfLiteInt8 && input->dims->size == 2 &&
        input->dims->data[0] == 1 && input->dims->data[1] == kTensorValues &&
        input->bytes == kTensorValues * sizeof(int8_t) &&
        output->type == kTfLiteInt8 && output->dims->size == 2 &&
        output->dims->data[0] == 1 && output->dims->data[1] == 1 &&
        output->bytes == sizeof(int8_t))) return false;

    // The exporter supplies per-tensor I/O quantization. Refuse mismatched
    // artifacts rather than silently scaling with another model's constants.
    error_ = "input/output quantization does not match scaler_data.h";
    const auto matches = [](const TfLiteTensor *tensor, float scale, int zero_point) {
        if (tensor->quantization.type != kTfLiteAffineQuantization ||
            !tensor->quantization.params) return false;
        const auto *q = static_cast<const TfLiteAffineQuantization *>(tensor->quantization.params);
        return std::isfinite(scale) && scale > 0.0f &&
            q->scale && q->zero_point && q->scale->size == 1 && q->zero_point->size == 1 &&
            q->scale->data[0] == scale && q->zero_point->data[0] == zero_point &&
            tensor->params.scale == scale && tensor->params.zero_point == zero_point;
    };
    if (!matches(input, kInputScale, kInputZeroPoint) ||
        !matches(output, kOutputScale, kOutputZeroPoint)) return false;
    ready_ = true;
    error_ = "none";
    return true;
}

bool ModelRuntime::Predict(const ModelInputWindow &window, float *score) {
    timing_ = {};
    error_ = "model not ready or missing score destination";
    if (!ready_ || !score) return false;
    error_ = "input window is incomplete";
    const int64_t started = clock_us_ ? clock_us_() : 0;
    if (!window.CopyQuantizedTo(interpreter_->input(0)->data.int8, kTensorValues)) return false;
    const int64_t prepared = clock_us_ ? clock_us_() : 0;
    timing_.input_us = prepared - started;
    error_ = "model execution failed";
    const TfLiteStatus status = interpreter_->Invoke();
    timing_.invoke_us = clock_us_ ? clock_us_() - prepared : 0;
    if (status != kTfLiteOk) return false;
    const float result = DequantizeOutput(interpreter_->output(0)->data.int8[0]);
    error_ = "model output is not a finite score between 0 and 1";
    if (!std::isfinite(result) || result < 0.0f || result > 1.0f) return false;
    *score = result;
    error_ = "none";
    return true;
}

std::size_t ModelRuntime::ArenaUsedBytes() const {
    return interpreter_ ? interpreter_->arena_used_bytes() : 0;
}
}  // namespace fall_detection
