#include "model_runtime.h"

#include <cmath>
#include "model_data.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace fall_detection {

bool ModelRuntime::Initialize(uint8_t *arena, std::size_t arena_size) {
    error_ = "invalid memory buffer or model already initialized";
    if (!arena || interpreter_) return false;
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
        resolver_.AddSpaceToBatchNd() != kTfLiteOk ||
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
    error_ = "expected float input [1,600] and output [1,1]";
    ready_ = input && output &&
        input->type == kTfLiteFloat32 && input->dims->size == 2 &&
        input->dims->data[0] == 1 && input->dims->data[1] == kTensorValues &&
        input->bytes == kTensorValues * sizeof(float) &&
        output->type == kTfLiteFloat32 && output->dims->size == 2 &&
        output->dims->data[0] == 1 && output->dims->data[1] == 1 &&
        output->bytes == sizeof(float);
    if (ready_) error_ = "none";
    return ready_;
}

bool ModelRuntime::Predict(const ModelInputWindow &window, float *score) {
    error_ = "model not ready or missing score destination";
    if (!ready_ || !score) return false;
    error_ = "input window is incomplete";
    if (!window.CopyTo(interpreter_->input(0)->data.f, kTensorValues)) return false;
    error_ = "model execution failed";
    if (interpreter_->Invoke() != kTfLiteOk) return false;
    const float result = interpreter_->output(0)->data.f[0];
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
